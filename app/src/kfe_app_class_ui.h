class KernelFileExplorer {
    friend struct KfeFileOps;
private:
    // View state
    std::vector<std::string> roots;
    bool showRoots = false;

    // Data for current device
    std::string currentDevice;
    std::map<std::string, std::vector<GameItem>> categories; // key = CAT_* or "Uncategorized"
    std::vector<GameItem> uncategorized;
    std::vector<GameItem> flatAll;
    std::vector<std::string> categoryNames;
    bool hasCategories = false;

    enum View { View_Categories, View_CategoryContents, View_AllFlat, View_GclSettings } view = View_AllFlat;
        std::string currentCategory;
        // Category sort-mode helpers
        bool        catPickActive = false;   // is a category "picked" for reordering?
        int         catPickIndex  = -1;      // picked row index (in entries)

    // Active list for content view (this is what we reorder & save)
    std::vector<GameItem> workingList;

    // UI cache
    std::vector<SceIoDirent> entries;
    std::vector<std::string> entryPaths;
    std::vector<GameItem::Kind> entryKinds;

    // Selected item icon cache
    Texture* selectionIconTex = nullptr;
    std::string selectionIconKey;

    // During rename we temporarily hold the current icon so refresh doesn't drop it.
    Texture* iconCarryTex = nullptr;
    std::string iconCarryForPath;
    Texture* categoryIconTex = nullptr;
    std::string categoryIconKey;
    bool categoryIconMissing = false;


    // Per-row flags (roots view)
    std::vector<uint8_t> rowFlags;
    static constexpr uint8_t ROW_DISABLED = 1 << 0;

    // NEW: reason + numbers for warning text in roots view
    enum RowDisableReason { RD_NONE = 0, RD_RUNNING_FROM_EF0 = 1, RD_NO_SPACE = 2 };
    std::vector<uint64_t> rowFreeBytes;
    std::vector<RowDisableReason> rowReason;   // <--- add
    std::vector<uint64_t>        rowNeedBytes; // <--- add
    std::vector<uint64_t>        rowTotalBytes;
    std::vector<uint8_t>        rowPresent;   // 1 if device row exists on hardware

    int selectedIndex = 0;
    int scrollOffset  = 0;
    bool scanAnimActive = false;
    unsigned long long scanAnimNextUs = 0;

    // Paths currently checked
    std::unordered_set<std::string> checked;

    // --- Game Categories Lite toggle state ---
    bool        gclArkOn = false;
    bool        gclProOn = false;
    std::string gclDevice;   // "ef0:/" on PSP Go if present; else "ms0:/"
    std::string gclPrxPath;  // full path to found category_lite.prx (if any)

    // --- NEW: in-memory gclite config (matches plugin) ---
    struct GclConfig {
        uint32_t mode;
        uint32_t prefix;
        uint32_t uncategorized;
        uint32_t selection;
        uint32_t catsort;
    };
    static GclConfig gclCfg;        // initialized out-of-class
    static bool      gclCfgLoaded;  // initialized out-of-class
    static std::unordered_map<std::string, std::vector<std::string>> gclBlacklistMap;        // key: root ("ms0:/", "ef0:/")
    static std::unordered_map<std::string, std::vector<std::string>> gclPendingUnblacklistMap;
    static std::unordered_map<std::string, std::vector<std::string>> gclCategoryFilterMap;   // key: filter root ("ms0:/", "ef0:/")
    static std::unordered_map<std::string, std::vector<std::string>> gclGameFilterMap;       // key: filter root ("ms0:/", "ef0:/")
    static bool gclFiltersLoaded;
    static bool gclFiltersScrubbed;
    static inline bool blacklistActive() { return gclCfg.prefix != 0; }
    bool isUncategorizedEnabledForDevice(const std::string& dev) const {
        if (gclCfg.uncategorized == 0) return false;
        if (gclCfg.uncategorized == 3) return true;
        if (dev.empty()) return gclCfg.uncategorized != 0;
        if (gclCfg.uncategorized == 1) return strncasecmp(dev.c_str(), "ms0:/", 5) == 0;
        if (gclCfg.uncategorized == 2) return strncasecmp(dev.c_str(), "ef0:/", 5) == 0;
        return true;
    }

    // ---- Lightweight cache patch: update in-memory categories without rescanning disk ----
    void patchCategoryCacheFromSettings(bool forceStripNumbers = false, bool preferOnDiskNames = false){
        const bool stripNumbers = forceStripNumbers || gclCfg.catsort;
        // 1) Collect bases and existing numbers from current cache keys (skip "Uncategorized")
        std::set<std::string, bool(*)(const std::string&, const std::string&)> baseSet(
            [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; }
        );
        std::map<std::string, std::vector<int>> baseExistingNums;

        const bool hasUncat = (categories.find("Uncategorized") != categories.end());
        (void)hasUncat; // silence unused warning if compiled out elsewhere

        for (const auto& kv : categories){
            const std::string& key = kv.first;
            if (!strcasecmp(key.c_str(), "Uncategorized")) continue; // never renumber/rename this pseudo-folder

            std::string base = stripCategoryPrefixes(key, stripNumbers);
            if (isBlacklistedBaseNameFor(currentDevice, base)) continue;
            baseSet.insert(base);
            int n = extractLeadingXXAfterOptionalCAT(key.c_str());
            if (n > 0) baseExistingNums[base].push_back(n);
        }

        if (baseSet.empty()){
            std::map<std::string, std::vector<GameItem>> keep;
            auto unc = categories.find("Uncategorized");
            if (unc != categories.end()) keep.emplace("Uncategorized", std::move(unc->second));
            categories.swap(keep);

            categoryNames.clear();
            hasCategories = false;

            std::string key = rootPrefix(currentDevice);
            auto &snap = deviceCache[key].snap;
            std::map<std::string, std::vector<GameItem>> snapKeep;
            auto uncSnap = snap.categories.find("Uncategorized");
            if (uncSnap != snap.categories.end()) snapKeep.emplace("Uncategorized", uncSnap->second);
            snap.categories.swap(snapKeep);
            snap.categoryNames.clear();
            snap.hasCategories = false;
            return;
        }

        // 2) Build sorted base list and determine desired numbering range 1..N
        std::vector<std::string> baseList(baseSet.begin(), baseSet.end());
        std::sort(baseList.begin(), baseList.end(),
                [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
        const int N = (int)baseList.size();

        // 3) Reserve valid existing numbers (dedupe), then fill the gaps for the rest
        std::unordered_set<int> used;
        std::map<std::string,int> reserved;  // base -> kept number
        for (const auto& base : baseList){
            auto it = baseExistingNums.find(base);
            if (it == baseExistingNums.end()) continue;

            // keep the smallest valid number within [1..N]
            int keep = 0;
            for (int n : it->second){
                if (n >= 1 && n <= N) keep = (keep == 0) ? n : std::min(keep, n);
            }
            if (keep > 0 && !used.count(keep)) {
                reserved[base] = keep;
                used.insert(keep);
            }
        }

        std::map<std::string,int> assigned = reserved;
        if (gclCfg.catsort){
            int next = 1;
            for (const auto& base : baseList){
                if (assigned.count(base)) continue;
                while (next <= N && used.count(next)) ++next;
                if (next > N) break;
                assigned[base] = next;
                used.insert(next);
                ++next;
            }
        } else {
            // Sorting off: numbers aren’t rendered, but keep map complete
            for (const auto& base : baseList) if (!assigned.count(base)) assigned[base] = 0;
        }

        // Optional: use on-disk names as the source of truth (prevents cache drift if renames fail).
        std::unordered_map<std::string, std::string> diskNameByBase;
        if (preferOnDiskNames && !currentDevice.empty()) {
            auto chooseDiskName = [&](const std::string& base, const std::string& candidate){
                std::string& cur = diskNameByBase[base];
                if (cur.empty()) { cur = candidate; return; }

                const std::string want = formatCategoryNameFromBase(base, assigned[base]);
                if (!strcasecmp(candidate.c_str(), want.c_str())) { cur = candidate; return; }
                if (!strcasecmp(cur.c_str(), want.c_str())) return;

                const bool candNum = (extractLeadingXXAfterOptionalCAT(candidate.c_str()) > 0);
                const bool curNum  = (extractLeadingXXAfterOptionalCAT(cur.c_str()) > 0);

                if (gclCfg.catsort) {
                    if (candNum && !curNum) { cur = candidate; return; }
                } else {
                    if (!candNum && curNum) { cur = candidate; return; }
                }
            };
            auto scanRoot = [&](const char* r){
                std::string abs = currentDevice + std::string(r);
                std::vector<std::string> subs;
                listSubdirs(abs, subs);
                for (auto &sub : subs) {
                    std::string subAbs = joinDirFile(abs, sub.c_str());
                    if (!findEbootCaseInsensitive(subAbs).empty()) continue; // skip real games
                    std::string base = stripCategoryPrefixes(sub, stripNumbers);
                    if (base.empty()) continue;
                    chooseDiskName(base, sub);
                }
            };
            scanRoot("ISO/");
            scanRoot("PSP/GAME/");
            scanRoot("PSP/GAME/PSX/");
            scanRoot("PSP/GAME/Utility/");
            scanRoot("PSP/GAME150/");
        }

        // 4) Re-key the categories map (and each GameItem.path), carrying "Uncategorized" through unchanged.
        std::map<std::string, std::vector<GameItem>> newCats;
        std::vector<std::pair<std::string, std::string>> oldToNew; // capture display-name rewrites

        for (auto &kv : categories){
            const std::string& oldCat = kv.first;

            if (!strcasecmp(oldCat.c_str(), "Uncategorized")) {
                newCats.emplace("Uncategorized", std::move(kv.second));
                continue;
            }

            std::string base = stripCategoryPrefixes(oldCat, stripNumbers);
            if (isBlacklistedBaseNameFor(currentDevice, base)) continue;
            std::string want = formatCategoryNameFromBase(base, assigned[base]);
            if (preferOnDiskNames) {
                auto itDisk = diskNameByBase.find(base);
                if (itDisk != diskNameByBase.end()) want = itDisk->second;
            }

            if (strcasecmp(oldCat.c_str(), want.c_str()) != 0) {
                // Category display name changed -> update each GameItem.path
                std::vector<GameItem> moved = std::move(kv.second);
                for (auto &gi : moved) {
                    replaceCatSegmentInPath(oldCat, want, gi.path);
                }
                oldToNew.emplace_back(oldCat, want);
                newCats.emplace(want, std::move(moved));
            } else {
                newCats.emplace(want, std::move(kv.second));
            }
        }
        categories.swap(newCats);

        // --- Remap "no icon" memoization set after category display rewrites ---
        if (!oldToNew.empty() && !noIconPaths.empty()) {
            std::unordered_set<std::string> remapped;
            remapped.reserve(noIconPaths.size());
            for (const auto &p : noIconPaths) {
                std::string q = p;
                for (const auto &m : oldToNew) {
                    replaceCatSegmentInPath(m.first, m.second, q);
                }
                remapped.insert(std::move(q));
            }
            noIconPaths.swap(remapped);
        }

        // --- Carry currently selected ICON0 across the key rewrite ---
        if (!oldToNew.empty()
            && selectionIconTex
            && selectionIconTex != placeholderIconTexture
            && !selectionIconKey.empty())
        {
            std::string newKey = selectionIconKey;
            for (const auto &m : oldToNew) {
                replaceCatSegmentInPath(m.first, m.second, newKey);
            }
            if (newKey != selectionIconKey) {
                iconCarryTex     = selectionIconTex;     // ensureSelectionIcon() will reattach immediately
                iconCarryForPath = newKey;
                selectionIconTex = nullptr;
                selectionIconKey.clear();
            }
        }

        // Rebuild categoryNames (includes "Uncategorized" exactly as-is)
        categoryNames.clear();
        for (auto &kv : categories) {
            if (!strcasecmp(kv.first.c_str(), "Uncategorized")) continue; // never put this into categoryNames
            categoryNames.push_back(kv.first);
        }

        // Existing sorting code can remain; "Uncategorized" will sort by its plain name
        // and is unaffected by XX/CAT_ since it has none.
        if (gclCfg.catsort){
            std::sort(categoryNames.begin(), categoryNames.end(), [](const std::string& a, const std::string& b){
                auto parseXX = [](const std::string& s)->int{
                    const char* p = s.c_str();
                    if (startsWithCAT(p)) p += 4;
                    if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')
                        return (p[0]-'0')*10 + (p[1]-'0');
                    return 0;
                };
                int ax = parseXX(a), bx = parseXX(b);
                if (ax > 0 || bx > 0){
                    if (ax != bx) return ax < bx;
                    return strcasecmp(a.c_str(), b.c_str()) < 0;
                }
                return strcasecmp(a.c_str(), b.c_str()) < 0;
            });
        } else {
            sortCategoryNamesByMtime(categoryNames, currentDevice);
        }
        hasCategories = !categoryNames.empty();

        // --- Mirror into device snapshot so cached rebuilds keep the new order & paths ---
        {
            std::string key = rootPrefix(currentDevice);
            auto &snap = deviceCache[key].snap;

            // Re-key snapshot categories with the same mapping (and rewrite GameItem.path)
            std::map<std::string, std::vector<GameItem>> snapNewCats;
            for (auto &kv : snap.categories) {
                const std::string& disp = kv.first;
                if (!strcasecmp(disp.c_str(), "Uncategorized")) {
                    snapNewCats.emplace("Uncategorized", kv.second); // keep as-is
                    continue;
                }
                std::string base = stripCategoryPrefixes(disp, stripNumbers);
                if (isBlacklistedBaseNameFor(currentDevice, base)) continue;
                std::string want = formatCategoryNameFromBase(base, assigned[base]);

                if (strcasecmp(disp.c_str(), want.c_str()) != 0) {
                    std::vector<GameItem> moved = kv.second; // copy; snapshot not moved-from elsewhere
                    for (auto &gi : moved) {
                        replaceCatSegmentInPath(disp, want, gi.path);
                    }
                    snapNewCats.emplace(want, std::move(moved));
                } else {
                    snapNewCats.emplace(want, kv.second);
                }
            }
            snap.categories.swap(snapNewCats);

            // Rebuild snapshot categoryNames to match
            snap.categoryNames.clear();
            for (auto &kv : snap.categories) {
                if (!strcasecmp(kv.first.c_str(), "Uncategorized")) continue;
                snap.categoryNames.push_back(kv.first);
            }
            if (gclCfg.catsort) {
                std::sort(snap.categoryNames.begin(), snap.categoryNames.end(), [](const std::string& a, const std::string& b){
                    auto parseXX = [](const std::string& s)->int{
                        const char* p = s.c_str();
                        if (startsWithCAT(p)) p += 4;
                        if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')
                            return (p[0]-'0')*10 + (p[1]-'0');
                        return 0;
                    };
                    int ax = parseXX(a), bx = parseXX(b);
                    if (ax > 0 || bx > 0) return (ax != bx) ? ax < bx : (strcasecmp(a.c_str(), b.c_str()) < 0);
                    return strcasecmp(a.c_str(), b.c_str()) < 0;
                });
            } else {
                sortCategoryNamesByMtime(snap.categoryNames, currentDevice);
            }
            snap.hasCategories = !snap.categoryNames.empty();
        }
    }




    static bool isPspGo() {
        int model = kuKernelGetModel();
        if (model >= 0) return model == 4;
        SceUID d = kfeIoOpenDir("ef0:/"); if (d >= 0) { kfeIoCloseDir(d); return true; }
        return false;
    }

    // Find the visible row index of a category name inside the current Categories screen.
    // This accounts for the top "Category Settings" row and the bottom "Uncategorized" row.
    int findCategoryRowByName(const std::string& name) const {
        for (int i = 0; i < (int)entries.size(); ++i) {
            if (!FIO_S_ISDIR(entries[i].d_stat.st_mode)) continue; // categories are shown as DIR rows
            if (!strcasecmp(entries[i].d_name, name.c_str())) return i;
        }
        return 0;
    }

    // ===== Category numbering & naming helpers/enforcement (ADD THIS AFTER isPspGo()) =====

    static inline bool hasTwoDigitsAfter(const char* p){
        return p && p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9';
    }

    static int extractLeadingXXAfterOptionalCAT(const char* name){
        if (!name) return 0;
        const char* s = name;
        if (startsWithCAT(s)) s += 4;
        if (hasTwoDigitsAfter(s)) return (s[0]-'0')*10 + (s[1]-'0');
        return 0;
    }

    // Strip CAT_, XX, or CAT_XX from a display/category folder name to its base.
    static std::string stripCategoryPrefixes(const std::string& in, bool stripNumbers){
        std::string s = in;
        if (startsWithCAT(s.c_str())) {
            s.erase(0, 4); // remove "CAT_"
        }
        if (stripNumbers && hasTwoDigitsAfter(s.c_str())) {
            s.erase(0, 2);
        }
        return s;
    }
    static std::string stripCategoryPrefixes(const std::string& in){
        return stripCategoryPrefixes(in, gclCfg.catsort != 0);
    }

    void sortCategoryNamesByMtime(std::vector<std::string>& names, const std::string& dev) const {
        if (names.size() < 2) return;
        std::unordered_map<std::string, u64> mt;
        mt.reserve(names.size());
        for (const auto& n : names) {
            mt[n] = categoryFolderMtime(dev, n);
        }
        std::sort(names.begin(), names.end(),
            [&](const std::string& a, const std::string& b){
                u64 ta = mt[a], tb = mt[b];
                if (ta != tb) return ta > tb; // newest first
                return strcasecmp(a.c_str(), b.c_str()) < 0;
            });
    }

    // Find the enforced on-disk folder name for a base (e.g., base "ARPG" -> "CAT_03ARPG" or "03ARPG")
    static std::string findDisplayNameForCategoryBase(const std::string& dev, const std::string& base) {
        const char* roots[] = { "ISO/","PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/" };
        for (auto r : roots) {
            std::string absRoot = dev + std::string(r);
            std::vector<std::string> subs;
            listSubdirs(absRoot, subs); // class-static helper
            for (auto& sub : subs) {
                // Skip real game folders; only consider category folders
                std::string subAbs = joinDirFile(absRoot, sub.c_str());
                if (!findEbootCaseInsensitive(subAbs).empty()) continue; // global helper is already visible
                if (!strcasecmp(stripCategoryPrefixes(sub).c_str(), base.c_str())) {
                    return sub; // enforced on-disk name we want
                }
            }
        }
        return base; // fallback (shouldn't happen immediately after enforcement)
    }

    // Build the final folder name from a base + index according to current settings.
    // Build the final folder name from a base + index according to current settings.
    static std::string formatCategoryNameFromBase(const std::string& base, int idx /*1-based*/){
        if (!strcasecmp(base.c_str(), "Uncategorized")) return "Uncategorized";

        char num[8]; num[0] = 0;
        if (gclCfg.catsort) snprintf(num, sizeof(num), "%02d", idx); // only when Sort=ON

        if (gclCfg.catsort) {
            // Sort ON -> numbers are part of the on-disk name
            if (gclCfg.prefix) return std::string("CAT_") + num + base; // CAT_XXbase
            else               return std::string(num) + base;          // XXbase
        } else {
            // Sort OFF -> numbers must not appear on disk
            if (gclCfg.prefix) return std::string("CAT_") + base;       // CAT_base
            else               return base;                             // base
        }
    }




    // Only blacklist ISO/VIDEO as a non-category (per plugin behavior) plus user-defined blacklist.
    static bool isBlacklistedCategoryFolder(const std::string& rootLabel, const std::string& sub,
                                            const std::string& absRoot, bool forceStripNumbers = false){
        // rootLabel is like "ISO/", "PSP/GAME/", etc.
        if (!strcasecmp(rootLabel.c_str(), "ISO/") && !strcasecmp(sub.c_str(), "VIDEO")) return true;
        if (!blacklistActive()) return false;

        const bool stripNumbers = forceStripNumbers || gclCfg.catsort;
        std::string base = stripCategoryPrefixes(sub, stripNumbers);
        if (isBlacklistedBaseNameFor(absRoot, base)) {
            if (strcasecmp(sub.c_str(), base.c_str()) != 0) {
                renameIfExists(absRoot, sub, base); // strip CAT_/## from blacklisted folders
            }
            return true;
        }
        return false;
    }

    // Enumerate immediate subfolders under a root.
    static void listSubdirs(const std::string& root, std::vector<std::string>& out){
        forEachEntry(root, [&](const SceIoDirent& e){
            if (FIO_S_ISDIR(e.d_stat.st_mode)){
                std::string n = e.d_name;
                if (n != "." && n != "..") out.push_back(n);
            }
        });
    }

    // Merge src -> dst, preferring existing entries in dst (src duplicates are deleted).
    static bool mergeDirPreferDest(const std::string& srcDir, const std::string& dstDir){
        if (!dirExists(srcDir) || !dirExists(dstDir)) return false;

        SceUID d = kfeIoOpenDir(srcDir.c_str());
        if (d < 0) return false;

        bool ok = true;
        SceIoDirent ent; memset(&ent, 0, sizeof(ent));
        while (kfeIoReadDir(d, &ent) > 0) {
            if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }

            std::string s = joinDirFile(srcDir, ent.d_name);
            std::string t = joinDirFile(dstDir, ent.d_name);

            if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
                if (dirExists(t)) {
                    if (!mergeDirPreferDest(s, t)) ok = false;
                    int rr = sceIoRmdir(s.c_str());
                    if (rr < 0) { ok = false; logf("cat merge: rmdir fail %s rc=%d", s.c_str(), rr); }
                } else {
                    int rr = sceIoRename(s.c_str(), t.c_str());
                    if (rr < 0) { ok = false; logf("cat merge: rename dir fail %s -> %s rc=%d", s.c_str(), t.c_str(), rr); }
                }
            } else {
                if (pathExists(t)) {
                    logf("cat merge: conflict keep dst %s", t.c_str());
                    int rr = sceIoRemove(s.c_str());
                    if (rr < 0) { ok = false; logf("cat merge: remove src fail %s rc=%d", s.c_str(), rr); }
                } else {
                    int rr = sceIoRename(s.c_str(), t.c_str());
                    if (rr < 0) { ok = false; logf("cat merge: rename file fail %s -> %s rc=%d", s.c_str(), t.c_str(), rr); }
                }
            }

            memset(&ent, 0, sizeof(ent));
            sceKernelDelayThread(0);
        }
        kfeIoCloseDir(d);

        return ok;
    }

    static bool mergeCategoryFolders(const std::string& absRoot,
                                     const std::string& from,
                                     const std::string& to) {
        std::string src = joinDirFile(absRoot, from.c_str());
        std::string dst = joinDirFile(absRoot, to.c_str());
        if (!dirExists(src) || !dirExists(dst)) return false;

        logInit();
        logf("cat merge: %s -> %s", src.c_str(), dst.c_str());
        bool ok = mergeDirPreferDest(src, dst);
        if (ok) updateHiddenAppPathsForFolderRename(absRoot, from, to);
        int rr = sceIoRmdir(src.c_str());
        if (rr < 0) { ok = false; logf("cat merge: source not empty %s rc=%d", src.c_str(), rr); }
        logClose();

        return ok;
    }

    static void normalizeCategoryFolderForBase(const std::string& absRoot,
                                               const std::vector<std::string>& subs,
                                               const std::string& base,
                                               const std::string& want,
                                               bool stripNumbers) {
        std::vector<std::string> matches;
        matches.reserve(subs.size());
        for (const auto &sub : subs) {
            if (strcasecmp(stripCategoryPrefixes(sub, stripNumbers).c_str(), base.c_str()) != 0) continue;
            std::string subAbs = joinDirFile(absRoot, sub.c_str());
            if (!findEbootCaseInsensitive(subAbs).empty()) continue; // skip real games
            matches.push_back(sub);
        }
        if (matches.empty()) return;

        auto isWant = [&](const std::string& s){ return !strcasecmp(s.c_str(), want.c_str()); };
        bool haveWant = false;
        for (const auto& m : matches) {
            if (isWant(m)) { haveWant = true; break; }
        }

        std::string wantAbs = joinDirFile(absRoot, want.c_str());
        if (!haveWant) {
            if (dirExists(wantAbs)) {
                haveWant = true;
            } else {
                std::string primary = matches[0];
                renameIfExists(absRoot, primary, want);
                if (dirExists(wantAbs)) haveWant = true;
                else {
                    logInit();
                    logf("cat normalize: rename fail %s -> %s",
                         joinDirFile(absRoot, primary.c_str()).c_str(), wantAbs.c_str());
                    logClose();
                }
            }
        }

        if (haveWant) {
            for (const auto& m : matches) {
                if (isWant(m)) continue;
                mergeCategoryFolders(absRoot, m, want);
            }
        }
    }

    // Rename “from”→“to” if it exists and differs (ignores case-only changes)
    static void renameIfExists(const std::string& root, const std::string& from, const std::string& to){
        if (!strcasecmp(from.c_str(), to.c_str())) return;
        std::string a = joinDirFile(root, from.c_str());
        std::string b = joinDirFile(root, to.c_str());
        if (dirExists(a)) {
            int rc = sceIoRename(a.c_str(), b.c_str());
            if (rc >= 0) {
                updateHiddenAppPathsForFolderRename(root, from, to);
            } else {
                logInit();
                logf("renameIfExists FAIL: %s -> %s rc=%d", a.c_str(), b.c_str(), rc);
                logClose();
            }
        }
    }

    // Enforce naming/numbering for category folders on a device, obeying rules:
    // • Leave folders that already have an XX number alone, UNLESS the number > total categories (then reassign).
    // • Never produce duplicate XX across different categories; if duplicates exist, keep the first by base (A→Z).
    // • Assign numbers to unnumbered bases in alphabetical order, filling the remaining 01..N slots without gaps.
    // • Respect "Use CAT prefix" (gclCfg.prefix) and "Sort Categories" (gclCfg.catsort).
    // • Skip game folders (subdirs that contain an EBOOT.PBP) and ISO/VIDEO.
    // • Skip game folders (subdirs that contain an EBOOT.PBP) and ISO/VIDEO.
    static void enforceCategorySchemeForDevice(const std::string& dev, bool forceStripNumbers = false){
        const char* isoRoots[]  = {"ISO/"};                // drop ISO/PSP/ as a root
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};

        // (absRoot, rootLabel)
        std::vector<std::pair<std::string,std::string>> roots;

        for (auto r : isoRoots)  roots.emplace_back(dev + std::string(r), std::string(r));
        for (auto r : gameRoots) roots.emplace_back(dev + std::string(r), std::string(r));

        // 1) Discover candidate bases (exclude EBOOT folders and blacklisted)
        std::unordered_set<std::string> baseSet;
        // Track existing numbers per base
        std::map<std::string, std::vector<int>> baseExistingNums;

        const bool stripNumbers = forceStripNumbers || gclCfg.catsort;

        for (auto &rp : roots){
            const std::string& absRoot  = rp.first;
            const std::string& rootLabel= rp.second;
            std::vector<std::string> subs;
            listSubdirs(absRoot, subs);
            for (auto &sub : subs){
                if (isBlacklistedCategoryFolder(rootLabel, sub, absRoot, forceStripNumbers)) continue;
                // Skip real game folders
                std::string subAbs = joinDirFile(absRoot, sub.c_str());
                if (!findEbootCaseInsensitive(subAbs).empty()) continue;

                std::string base = stripCategoryPrefixes(sub, stripNumbers);
                baseSet.insert(base);

                // Record any existing two-digit number (after optional CAT_)
                int n = extractLeadingXXAfterOptionalCAT(sub.c_str());
                if (n > 0) baseExistingNums[base].push_back(n);
            }
        }

        if (baseSet.empty()) return;

        // 2) Build sorted base list and desired numbering range 1..N
        std::vector<std::string> baseList(baseSet.begin(), baseSet.end());
        std::sort(baseList.begin(), baseList.end(),
                [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
        const int N = (int)baseList.size();

        // 3) Reserve already-numbered bases (unique, within 1..N). If duplicates or out-of-range → treat as unnumbered.
        std::unordered_set<int> used;
        std::map<std::string,int> reserved; // base -> number we will keep

        // Choose a stable "keeper" for duplicates by alphabetical base.
        for (const auto& base : baseList){
            auto it = baseExistingNums.find(base);
            if (it == baseExistingNums.end()) continue;
            // prefer the smallest valid number present
            int keep = 0;
            for (int n : it->second){
                if (n >= 1 && n <= N) keep = (keep == 0) ? n : (std::min(keep, n));
            }
            if (keep == 0) continue; // none valid; will be assigned later
            // If number already used, we cannot keep it for this base (avoid duplicates)
            if (!used.count(keep)) { reserved[base] = keep; used.insert(keep); }
            // else: duplicate → this base becomes unnumbered (will be assigned later)
        }

        // 4) Assign numbers for all unnumbered bases, filling remaining 01..N in order.
        std::map<std::string,int> assigned = reserved;
        if (gclCfg.catsort){
            int next = 1;
            for (const auto& base : baseList){
                if (assigned.count(base)) continue;
                while (used.count(next) && next <= N) ++next;
                if (next > N) break; // safety
                assigned[base] = next;
                used.insert(next);
                ++next;
            }
        } else {
            // Sorting disabled: numbers ignored by formatter, but keep map complete
            for (const auto& base : baseList){
                if (!assigned.count(base)) assigned[base] = 0;
            }
        }

        // 5) For each root, normalize any present variants → desired formatted name
        for (auto &rp : roots){
            const std::string& absRoot = rp.first;
            std::vector<std::string> subs;
            listSubdirs(absRoot, subs);

            for (const auto& base : baseList){
                const int idx = assigned[base];
                const std::string want = formatCategoryNameFromBase(base, idx);
                normalizeCategoryFolderForBase(absRoot, subs, base, want, stripNumbers);
            }
        }
    }






    static std::string gclConfigPath() {
        const char* root = isPspGo() ? "ef0:/" : "ms0:/";
        return std::string(root) + "seplugins/gclite.bin";
    }

    // Pick/drop state
    bool moving = false;

    intraFont*  font   = nullptr;
    intraFont*  fontJpn = nullptr;
    intraFont*  fontKr  = nullptr;
    MessageBox* msgBox = nullptr;
    FileOpsMenu* fileMenu = nullptr;
    OptionListMenu* optMenu = nullptr;   // ← NEW: modal option picker
    std::vector<std::string> optMenuOwnedLabels; // keep dynamic labels alive for OptionListMenu
    std::string msgBoxOwnedText; // keep transient MessageBox text alive
    static bool rootPickGcl;             // ← declaration only; no in-class initializer
    static bool rootKeepGclSelection;    // keep selection on "Game Categories:" after toggle
    bool inputWaitRelease = false;
    bool bulkSquareUncheck = false;

    // Which Categories Lite setting is currently being edited
    enum GclSettingKey { GCL_SK_None = -1, GCL_SK_Mode = 0, GCL_SK_Prefix = 1, GCL_SK_Uncat = 2, GCL_SK_Sort = 3, GCL_SK_Blacklist = 4 };
    static GclSettingKey gclPending;
    bool gclBlacklistDirty = false;

    // Track which kind of menu is open (content vs categories)
    enum MenuContext { MC_ContentOps, MC_CategoryOps };
    MenuContext menuContext = MC_ContentOps;

    // Debug overlay
    bool showDebugTimes = false; // ...
    bool showTitles     = false; // ...

    // ---- Categories sort mode (UI) ----
    bool catSortMode     = false;   // SELECT toggles this while in View_Categories
    int catScrollIndex = -1;
    unsigned long long catScrollStartUs = 0;
    int gameScrollIndex = -1;
    unsigned long long gameScrollStartUs = 0;

    // Helper: is a visible row non-movable (header/footer)?
    bool isCategoryRowLocked(int row) const {
        if (row < 0 || row >= (int)entries.size()) return true;
        // Top row is Category Settings
        if (!strcasecmp(entries[row].d_name, kCatSettingsLabel)) return true;
        if (!strcasecmp(entries[row].d_name, "__GCL_SETTINGS__")) return true;
        // Bottom row is Uncategorized (when present)
        if (!strcasecmp(entries[row].d_name, "Uncategorized"))     return true;
        return false;
    }

    static constexpr float CAT_ROW_H = 16.0f;
    static constexpr float CAT_LIST_OFFSET_Y = 5.0f;
    static constexpr float CAT_SETTINGS_GAP = 9.0f;
    static constexpr float GAME_ROW_H = 16.0f;
    static constexpr float GAME_LIST_OFFSET_Y = CAT_LIST_OFFSET_Y - 5.0f;
    int categoryVisibleRows() const {
        const float panelH = 226.0f;
        float listH = panelH - 8.0f - CAT_LIST_OFFSET_Y;
        if (listH < CAT_ROW_H) listH = CAT_ROW_H;
        int visible = (int)(listH / CAT_ROW_H);
        if (!showRoots && view == View_Categories) {
            bool hasSettingsRow = false;
            if (!entries.empty()) {
                const char* nm = entries[0].d_name;
                hasSettingsRow = (!strcasecmp(nm, kCatSettingsLabel));
            }
            const bool opHeader = (!hasSettingsRow &&
                actionMode != AM_None && (opPhase == OP_SelectCategory || opPhase == OP_Confirm));
            if (hasSettingsRow || opHeader) {
                float scrollH = listH - (CAT_ROW_H + CAT_SETTINGS_GAP);
                if (scrollH < CAT_ROW_H) scrollH = CAT_ROW_H;
                visible = (int)(scrollH / CAT_ROW_H);
            }
            visible += 1;
        }
        if (visible < 1) visible = 1;
        return visible;
    }
    int gclSettingsVisibleRows() const {
        const float panelH = 226.0f;
        const float rowH = CAT_ROW_H + 3.0f;
        float listH = panelH - 8.0f - CAT_LIST_OFFSET_Y;
        if (listH < rowH) listH = rowH;
        int visible = (int)(listH / rowH);
        if (visible < 1) visible = 1;
        return visible;
    }
    int contentVisibleRows() const {
        const float panelH = 226.0f;
        float listH = panelH - 8.0f - GAME_LIST_OFFSET_Y;
        if (listH < GAME_ROW_H) listH = GAME_ROW_H;
        int visible = (int)(listH / GAME_ROW_H);
        if (visible < 1) visible = 1;
        return visible + 1;
    }
    const char* currentDeviceHeaderName() const {
        if (!currentDevice.empty()) {
            if (!strncasecmp(currentDevice.c_str(), "ms0:", 4)) return "Memory Stick";
            if (!strncasecmp(currentDevice.c_str(), "ef0:", 4)) return "Internal Storage";
            return rootDisplayName(currentDevice.c_str());
        }
        return "Memory Stick";
    }
    int categoryDisplayCount() const {
        int count = 0;
        for (const auto& e : entries) {
            if (!strcasecmp(e.d_name, kCatSettingsLabel)) continue;
            if (!strcasecmp(e.d_name, "__GCL_SETTINGS__")) continue;
            count++;
        }
        return count;
    }


    // Edge detection for analog-stick up → debug toggle
    bool analogUpHeld = false;

    // Running location
    bool runningFromEf0 = false;

    // ====== Key repeat state ======
    unsigned lastButtons = 0;
    unsigned long long upHoldStartUS   = 0, upLastRepeatUS   = 0;
    unsigned long long downHoldStartUS = 0, downLastRepeatUS = 0;

    // Cache of entries that have no embedded icon; use placeholder and don't retry.
    std::unordered_set<std::string> noIconPaths;

    // Apply current on-screen order of categories to XX numbering and on-disk names.
    // - Skips "Uncategorized" entirely.
    // - If catsort==ON: assigns 01..N in the exact visible order; renames CAT folders across roots.
    // - If catsort==OFF: just patches the cache order (no XX, no renames).
    void applyCategoryOrderAndPersist() {
        // 1) Build an ordered list of BASE names from the visible entries
        std::vector<std::string> orderedBases;
        orderedBases.reserve(entries.size());
        for (int i = 0; i < (int)entries.size(); ++i) {
            const char* nm = entries[i].d_name;
            if (!strcasecmp(nm, kCatSettingsLabel)) continue;
            if (!strcasecmp(nm, "Uncategorized"))     continue;
            orderedBases.push_back(stripCategoryPrefixes(nm));
        }
        if (orderedBases.empty()) return;

        if (msgBox) { delete msgBox; msgBox = nullptr; }
        const char* savingText = "Saving...";
        const char* returnText = "Returning...";
        const float popScale = 1.0f;
        const int popPadX = 10;
        const int popPadY = 24;
        const int popLineH = (int)(24.0f * popScale + 0.5f);
        const float popTextW = measureTextWidth(popScale, returnText);
        const int popExtraW = 4;
        int popPanelW = (int)(popTextW + popPadX * 2 + popExtraW + 0.5f);
        popPanelW -= 6;
        popPanelW -= 27; // 20px narrower than the Returning modal
        if (popPanelW < 40) popPanelW = 40;
        const int popBottom = 14;
        const int popPanelH = popPadY + popLineH + popBottom - 24;
        const int popWrapTweak = 32;
        const int popForcedPxPerChar = 8;
        msgBox = new MessageBox(savingText, nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                popScale, 0, "", popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                popPanelW, popPanelH);
        renderOneFrame();

        // 2) Make a base->index map according to the on-screen order
        std::map<std::string,int> assigned;
        if (gclCfg.catsort) {
            for (size_t i = 0; i < orderedBases.size(); ++i)
                assigned[orderedBases[i]] = (int)i + 1; // 01..N visual order
        } else {
            for (auto& b : orderedBases) assigned[b] = 0; // numbers suppressed
        }

        // 3) Build desired final names for each base
        std::map<std::string,std::string> baseToWant;
        for (auto& b : orderedBases) {
            baseToWant[b] = formatCategoryNameFromBase(b, assigned[b]);
        }

        // 4) Rename on disk (for devices that exist) to match the new wanted names
        //    Only touch category folders inside ISO/ and PSP/GAME*/ roots. Never touch root-level PSP/.
        const char* isoRoots[]  = {"ISO/"};
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME150/","PSP/GAME/PSX/","PSP/GAME/Utility/"};
        auto doDevice = [&](const std::string& dev){
            if (dev.empty() || dev[3] != ':') return;
            for (auto& kv : baseToWant) {
                const std::string& base = kv.first;
                const std::string& want = kv.second;

                // For each root, find any variant for this base and rename to the wanted name
                auto renameIn = [&](const char* r){
                    std::string abs = dev + std::string(r);
                    std::vector<std::string> subs; listSubdirs(abs, subs);
                    normalizeCategoryFolderForBase(abs, subs, base, want, gclCfg.catsort);
                };
                for (const char* r : isoRoots)  renameIn(r);
                for (const char* r : gameRoots) renameIn(r);
            }
        };

        // Persist renames for the *current* device.
        doDevice(currentDevice);

        // Mirror to the other root (PSP Go: ms0 <-> ef0), if present.
        if (isPspGo()) {
            const std::string cur = rootPrefix(currentDevice);   // "ms0:/" or "ef0:/"
            const std::string other = (!strcasecmp(cur.c_str(), "ms0:/")) ? "ef0:/" : "ms0:/";

            // Only touch the other root if it exists / is mounted.
            SceUID d = kfeIoOpenDir(other.c_str());
            if (d >= 0) {
                kfeIoCloseDir(d);
                doDevice(other);          // perform the same rename enforcement on the other root
                markDeviceDirty(other);   // if you already use a “dirty” flag in your cache
            }
        }

        // PSP Go may have both devices; if both exist, update the other too
        if (isPspGo()) {
            const std::string other = (rootPrefix(currentDevice) == "ms0:/") ? std::string("ef0:/")
                                                                            : std::string("ms0:/");
            if (!rootPrefix(other).empty()) {
                doDevice(other);
                markDeviceDirty(other); // force a rescan if you switch devices later
            }
        }


        // 5) Patch the in-memory cache to reflect new names WITHOUT rescanning
        //    Re-key categories and rebuild categoryNames using the SAME mapping (baseToWant)
        {
            // Build a new categories map using the wanted display names
            // ... inside KernelFileExplorer::applyCategoryOrderAndPersist()

            // Declare these ONCE at the top of your “rebuild categories” section
            // ... inside KernelFileExplorer::applyCategoryOrderAndPersist()
            std::map<std::string, std::vector<GameItem>> newCats;
            std::vector<std::pair<std::string, std::string>> oldToNew;

            // re-key categories to wanted names and record old→new for patching icon caches
            for (auto &kv : categories) {
                const std::string& oldCat = kv.first;
                if (!strcasecmp(oldCat.c_str(), "Uncategorized")) {
                    newCats.emplace("Uncategorized", std::move(kv.second));
                    continue;
                }
                std::string base   = stripCategoryPrefixes(oldCat);
                std::string newCat = formatCategoryNameFromBase(base, assigned[base]);
                if (strcasecmp(oldCat.c_str(), newCat.c_str()) != 0) {
                    std::vector<GameItem> moved = std::move(kv.second);
                    for (auto &gi : moved) replaceCatSegmentInPath(oldCat, newCat, gi.path);
                    oldToNew.emplace_back(oldCat, newCat);
                    newCats.emplace(newCat, std::move(moved));
                } else {
                    newCats.emplace(newCat, std::move(kv.second));
                }
            }
            categories.swap(newCats);


            // --- Remap "no icon" memoization set after category display rewrites ---
            if (!oldToNew.empty() && !noIconPaths.empty()) {
                std::unordered_set<std::string> remapped;
                remapped.reserve(noIconPaths.size());
                for (const auto &p : noIconPaths) {
                    std::string q = p;
                    for (const auto &m : oldToNew) {
                        replaceCatSegmentInPath(m.first, m.second, q);
                    }
                    remapped.insert(std::move(q));
                }
                noIconPaths.swap(remapped);
            }

            // --- Carry currently selected ICON0 across the key rewrite ---
            if (!oldToNew.empty()
                && selectionIconTex
                && selectionIconTex != placeholderIconTexture
                && !selectionIconKey.empty())
            {
                std::string newKey = selectionIconKey;
                for (const auto &m : oldToNew) {
                    replaceCatSegmentInPath(m.first, m.second, newKey);
                }
                if (newKey != selectionIconKey) {
                    iconCarryTex     = selectionIconTex;
                    iconCarryForPath = newKey;
                    selectionIconTex = nullptr;
                    selectionIconKey.clear();
                }
            }


            // --- NEW: remap the "no icon" memoization set to the new paths
            if (!oldToNew.empty() && !noIconPaths.empty()) {
                std::unordered_set<std::string> remapped;
                remapped.reserve(noIconPaths.size());
                for (const auto &p : noIconPaths) {
                    std::string q = p;
                    for (const auto &m : oldToNew) {
                        replaceCatSegmentInPath(m.first, m.second, q);
                    }
                    remapped.insert(std::move(q));
                }
                noIconPaths.swap(remapped);
            }

            // --- NEW: carry the currently selected ICON0 across the key rewrite
            if (!oldToNew.empty() && selectionIconTex && selectionIconTex != placeholderIconTexture && !selectionIconKey.empty()) {
                std::string newKey = selectionIconKey;
                for (const auto &m : oldToNew) {
                    replaceCatSegmentInPath(m.first, m.second, newKey);
                }
                if (newKey != selectionIconKey) {
                    iconCarryTex     = selectionIconTex;     // let ensureSelectionIcon() reattach
                    iconCarryForPath = newKey;
                    selectionIconTex = nullptr;
                    selectionIconKey.clear();
                }
            }



            // Rebuild categoryNames to match (exclude "Uncategorized")
            categoryNames.clear();
            for (auto &kv : categories) {
                if (!strcasecmp(kv.first.c_str(), "Uncategorized")) continue;
                categoryNames.push_back(kv.first);
            }

            // Keep the on-screen order stable: if catsort is ON, sort numerically by XX (or CAT_XX),
            // otherwise alphabetical (your existing rule).
            if (gclCfg.catsort) {
                std::sort(categoryNames.begin(), categoryNames.end(), [](const std::string& a, const std::string& b){
                    auto parseXX = [](const std::string& s)->int{
                        const char* p = s.c_str();
                        if (startsWithCAT(p)) p += 4;
                        if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')
                            return (p[0]-'0')*10 + (p[1]-'0');
                        return 0;
                    };
                    int ax = parseXX(a), bx = parseXX(b);
                    if (ax > 0 || bx > 0){
                        if (ax != bx) return ax < bx;
                        return strcasecmp(a.c_str(), b.c_str()) < 0;
                    }
                    return strcasecmp(a.c_str(), b.c_str()) < 0;
                });
            } else {
                sortCategoryNamesByMtime(categoryNames, currentDevice);
            }

            // Mirror into the device snapshot so cache-based rebuilds reflect the new order
            {
                std::string key = rootPrefix(currentDevice);
                auto &snap = deviceCache[key].snap;
                snap.categories    = categories;
                snap.categoryNames = categoryNames;
                snap.hasCategories = hasCategories;
            }
        }

        // 6) Update on-screen entry names in-place (keeps current order/selection)
        if (!showRoots && view == View_Categories) {
            for (int i = 0; i < (int)entries.size(); ++i) {
                const char* nm = entries[i].d_name;
                if (!strcasecmp(nm, kCatSettingsLabel) || !strcasecmp(nm, "Uncategorized")) continue;
                std::string base = stripCategoryPrefixes(nm);
                auto it = baseToWant.find(base);
                if (it != baseToWant.end()) {
                    strncpy(entries[i].d_name, it->second.c_str(), sizeof(entries[i].d_name) - 1);
                    entries[i].d_name[sizeof(entries[i].d_name) - 1] = '\0';
                }
            }
        }

        refreshGclFilterFile();

        delete msgBox; msgBox = nullptr;
    }



    // -----------------------------
    // New: Operation (Move/Copy) state
    // -----------------------------
    enum ActionMode { AM_None, AM_Move, AM_Copy };
    enum OpPhase    { OP_None, OP_SelectDevice, OP_SelectCategory, OP_Confirm };
    ActionMode actionMode = AM_None;
    OpPhase    opPhase    = OP_None;

    // Snapshot of source selection (paths + kinds)
    std::vector<std::string> opSrcPaths;
    std::vector<GameItem::Kind> opSrcKinds;
    int         opSrcCount = 0;
    uint64_t    opSrcTotalBytes = 0;

    // Destination selections
    std::string opDestDevice;     // "ms0:/" or "ef0:/"
    std::string opDestCategory;   // "" for Uncategorized, or "CAT_*"

    // NEW: set of category names disabled during category selection (e.g., source category on same device)
    std::unordered_set<std::string> opDisabledCategories;

    // Pre-op UI state to restore after operation/cancel
    std::string preOpDevice;
    View        preOpView = View_AllFlat;
    std::string preOpCategory;
    int         preOpSel = 0;
    int         preOpScroll = 0;

    // --- scan snapshot so we can instantly reuse current device contents ---
    // --- scan snapshot so we can instantly reuse current device contents ---
    struct ScanSnapshot {
        std::map<std::string, std::vector<GameItem>> categories;
        std::vector<GameItem> uncategorized;
        std::vector<GameItem> flatAll;
        std::vector<std::string> categoryNames;
        bool hasCategories = false;
    };

    // Used during a single Move/Copy UI flow
    ScanSnapshot preOpScan{};
    bool hasPreOpScan = false;

    // --- New: cache per device so switching devices is instant ---
    struct DeviceCacheEntry {
        ScanSnapshot snap;
        bool dirty = true;            // true ⇒ must rescan before reuse
        uint32_t lastTick = 0;        // optional (age, if you want to expire)
    };
    std::map<std::string, DeviceCacheEntry> deviceCache;  // key: "ms0:/" or "ef0:/"
   
    // Marks a device cache line as dirty so the next entry triggers one fresh scan.
    void markDeviceDirty(const std::string& devOrPath) {
        std::string key = rootPrefix(devOrPath);  // accepts "ms0:/", "ef0:/", or any full path
        if (!key.empty()) deviceCache[key].dirty = true;
    }

    // Marks all device cache lines dirty so next device entry forces a rescan.
    void markAllDevicesDirty() {
        for (auto &kv : deviceCache) kv.second.dirty = true;
        for (const auto &r : roots) markDeviceDirty(r);
    }

    void setCategorySortMode(bool enable, bool saveOnExit = false) {
        if (!gclCfg.catsort) return;
        if (enable == catSortMode) return;

        if (enable) {
            catSortMode   = true;
            catPickActive = false;
            catPickIndex  = -1;
            return;
        }

        std::string keepBase;
        if (selectedIndex >= 0 && selectedIndex < (int)entries.size()) {
            const char* disp = entries[selectedIndex].d_name;
            if (strcasecmp(disp, kCatSettingsLabel) != 0 && strcasecmp(disp, "Uncategorized") != 0) {
                keepBase = stripCategoryPrefixes(disp);
            }
        }

        catSortMode   = false;
        catPickActive = false;
        catPickIndex  = -1;

        if (saveOnExit) {
            applyCategoryOrderAndPersist();
        }
        buildCategoryRows();

        int idx = -1;
        if (!keepBase.empty()) {
            for (int i = 0; i < (int)entries.size(); ++i) {
                if (!strcasecmp(stripCategoryPrefixes(entries[i].d_name).c_str(), keepBase.c_str())) {
                    idx = i; break;
                }
            }
        }
        if (idx < 0) {
            idx = (selectedIndex >= 0 && selectedIndex < (int)entries.size())
                ? selectedIndex
                : (int)entries.size() - 1;
            if (idx < 0) idx = 0;
        }

        selectedIndex = idx;
        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
        const int visible = categoryVisibleRows();
        const int lastVisible = scrollOffset + visible - 1;
        if (selectedIndex > lastVisible) scrollOffset = selectedIndex - (visible - 1);
    }


    void exitOpMode() {
        actionMode = AM_None;
        opPhase    = OP_None;
        opSrcPaths.clear();
        opSrcKinds.clear();
        opSrcCount = 0;
        opSrcTotalBytes = 0;
        opDestDevice.clear();
        opDestCategory.clear();
        moving = false;
    }

    // Restore pre-op UI and state after cancel/fail.
    // NOTE: MUST be called *after* exitOpMode().
    void cancelMoveRestore() {
        // Go back to where the user started
        currentDevice = preOpDevice;

        if (hasPreOpScan) {
            restoreScan(preOpScan);    // instant lists from the snapshot
        } else {
            // Fallback if no snapshot was taken (shouldn't happen in your flow)
            scanDevicePreferCache(currentDevice);
        }

        moving = false;

        // Return to the exact pre-op view
        if (preOpView == View_Categories) {
            openCategory(preOpCategory);
        } else if (preOpView == View_CategoryContents) {
            currentCategory = preOpCategory;
            openCategory(preOpCategory);
        } else { // View_AllFlat (or anything else)
            rebuildFlatFromCache();
        }

        // Restore cursor/scroll
        selectedIndex = preOpSel;
        scrollOffset  = preOpScroll;

        // Clear transient UI
        freeSelectionIcon();
        fileMenu = nullptr;
        msgBox   = nullptr;
    }

    // Show the destination device/category immediately from cache (no re-scan)
    void showDestinationCategoryNow(const std::string& dstDev, const std::string& dstCat /* "" => Uncategorized */) {
        currentDevice = dstDev;

        // Use cached snapshot if present, otherwise scan once to seed it
        auto &dstEntry = deviceCache[rootPrefix(dstDev)];
        bool haveSnap = !dstEntry.snap.flatAll.empty() ||
                        !dstEntry.snap.uncategorized.empty() ||
                        !dstEntry.snap.categories.empty() ||
                        !dstEntry.snap.categoryNames.empty() ||
                        dstEntry.snap.hasCategories;

        if (!haveSnap || dstEntry.dirty) {
            scanDevice(dstDev);
            snapshotCurrentScan(dstEntry.snap);
            dstEntry.dirty = false;
        } else {
            restoreScan(dstEntry.snap);
        }

        // Normalize cat name ("Uncategorized" vs "")
        std::string cat = dstCat.empty() ? std::string("Uncategorized") : dstCat;

        if (hasCategories) {
            // Open the real destination category with ALL entries (not just the moved ones)
            openCategory(cat);
        } else {
            // No categories on this device → show flat list that now includes the moved/copied items
            rebuildFlatFromCache();
        }

        // We are back to normal browsing now
        showRoots   = false;
        actionMode  = AM_None;
        opPhase     = OP_None;
        moving      = false;
        freeSelectionIcon();
    }

    void snapshotCurrentScan(ScanSnapshot& out) const {
        out.categories     = categories;
        out.uncategorized  = uncategorized;
        out.flatAll        = flatAll;
        out.categoryNames  = categoryNames;
        out.hasCategories  = hasCategories;
    }
    void restoreScan(const ScanSnapshot& in) {
        categories     = in.categories;
        uncategorized  = in.uncategorized;
        flatAll        = in.flatAll;
        categoryNames  = in.categoryNames;
        hasCategories  = in.hasCategories;
    }

    // Fetch prefix like "ms0:/" from any path or device string you use elsewhere
    // Fetch prefix like "ms0:/" or "ef0:/" from any path or device string you use elsewhere
    static std::string rootPrefix(const std::string& p) {
        // We expect "ms0:/..." or "ef0:/..." (colon at index 3, slash at index 4)
        // Also accept "ms0:" / "ef0:" (without trailing slash).
        if (p.size() >= 5 && p[3] == ':' && p[4] == '/')
            return p.substr(0, 5);
        if (p.size() >= 4 && p[3] == ':')
            return p.substr(0, 4) + "/";  // normalize to have a trailing slash
        // Fallback: try to locate a colon at pos 3 even for longer/odd inputs
        size_t colon = p.find(':');
        if (colon == 3) {
            if (colon + 1 < p.size() && p[colon + 1] == '/')
                return p.substr(0, colon + 2);
            return p.substr(0, colon + 1) + "/";
        }
        return "";
    }


    // Replace raw scan calls during navigation with this:
    void scanDevicePreferCache(const std::string& dev) {
        std::string key = rootPrefix(dev);
        auto &entry = deviceCache[key];  // creates if missing
        if (!entry.dirty) {
            // Reuse cached lists instantly
            restoreScan(entry.snap);
        } else {
            // Do the real scan and then cache it
            scanDevice(dev);
            snapshotCurrentScan(entry.snap);
            entry.dirty = false;
        }
    }

    // Remove a GameItem by exact path from all vectors in a snapshot.
    static void snapErasePath(ScanSnapshot& s, const std::string& path) {
        auto rmPath = [&](std::vector<GameItem>& v){
            v.erase(std::remove_if(v.begin(), v.end(),
                [&](const GameItem& gi){ return gi.path == path; }), v.end());
        };
        rmPath(s.flatAll);
        rmPath(s.uncategorized);
        for (auto &kv : s.categories) rmPath(kv.second);
    }

    static void snapInsertSorted(std::vector<GameItem>& v, const GameItem& gi) {
        // Replace in place if present; otherwise insert at the first position
        // where gi.sortKey <= existing.sortKey to keep DESC order.
        for (auto &x : v) {
            if (x.path == gi.path) { x = gi; return; }
        }
        auto it = std::lower_bound(
            v.begin(), v.end(), gi,
            [](const GameItem& a, const GameItem& b){ return a.sortKey > b.sortKey; } // DESC
        );
        v.insert(it, gi);
    }

    static void snapUpsertItem(ScanSnapshot& s, const GameItem& gi, const std::string& category /*may be ""*/) {
        snapInsertSorted(s.flatAll, gi);
        if (category.empty()) {
            snapInsertSorted(s.uncategorized, gi);
        } else {
            snapInsertSorted(s.categories[category], gi);
            if (std::find(s.categoryNames.begin(), s.categoryNames.end(), category) == s.categoryNames.end())
                s.categoryNames.push_back(category);
            s.hasCategories = true;
        }
    }

    // ---- Cache patch helpers (categories & items) ----
    void cachePatchAddCategory(const std::string& cat) {
        // Patch live members directly
        (void)categories[cat];
        if (std::find(categoryNames.begin(), categoryNames.end(), cat) == categoryNames.end())
            categoryNames.push_back(cat);
        hasCategories = true;

        // Patch device snapshot
        std::string key = rootPrefix(currentDevice);
        auto &dc = deviceCache[key];  // creates entry if missing
        (void)dc.snap.categories[cat];
        if (std::find(dc.snap.categoryNames.begin(), dc.snap.categoryNames.end(), cat) == dc.snap.categoryNames.end())
            dc.snap.categoryNames.push_back(cat);
        dc.snap.hasCategories = true;
    }


    void cachePatchDeleteCategory(const std::string& cat) {
        // Live members
        {
            auto it = categories.find(cat);
            if (it != categories.end()) {
                for (const auto& gi : it->second) {
                    // remove any of those paths from flat/uncat too
                    // build a small pseudo-snapshot over live refs
                    ScanSnapshot tmp;
                    tmp.categories    = categories;
                    tmp.uncategorized = uncategorized;
                    tmp.flatAll       = flatAll;
                    tmp.categoryNames = categoryNames;
                    tmp.hasCategories = hasCategories;

                    snapErasePath(tmp, gi.path);

                    // write back the erased sets
                    uncategorized = std::move(tmp.uncategorized);
                    flatAll       = std::move(tmp.flatAll);
                }
                categories.erase(it);
            }
            categoryNames.erase(std::remove(categoryNames.begin(), categoryNames.end(), cat), categoryNames.end());
            hasCategories = !categoryNames.empty();
        }

        // Device snapshot
        {
            std::string key = rootPrefix(currentDevice);
            auto &s = deviceCache[key].snap;
            auto it = s.categories.find(cat);
            if (it != s.categories.end()) {
                for (const auto& gi : it->second) snapErasePath(s, gi.path);
                s.categories.erase(it);
            }
            s.categoryNames.erase(std::remove(s.categoryNames.begin(), s.categoryNames.end(), cat), s.categoryNames.end());
            s.hasCategories = !s.categoryNames.empty();
        }
    }


    static void replaceCatSegmentInPath(const std::string& oldCat,
                                        const std::string& newCat,
                                        std::string& pathInOut) {
        if (oldCat.empty() || oldCat == newCat) return;

        // Helper: replace the *category* segment immediately following a given root
        auto replaceAfterRoot = [&](const char* root) -> bool {
            size_t rootPos = pathInOut.find(root);
            if (rootPos == std::string::npos) return false;

            // Start of the candidate segment (category folder name)
            size_t start = rootPos + strlen(root);
            size_t slash = pathInOut.find('/', start);
            if (slash == std::string::npos) return false;

            size_t segLen = slash - start;
            if (segLen != oldCat.size()) return false;
            if (pathInOut.compare(start, segLen, oldCat) != 0) return false;

            pathInOut.replace(start, segLen, newCat);
            return true;
        };

        // ISO categories: ms0:/ISO/<cat>/...
        const char* isoRoots[] = { "ISO/" };                  // drop ISO/PSP/ as a root
        for (int i = 0; i < 1; ++i) {
            if (replaceAfterRoot(isoRoots[i])) return;
        }

        // EBOOT folder categories: ms0:/PSP/GAME/<cat>/..., ms0:/PSP/GAME150/<cat>/...
        const char* gameRoots[] = {
            "PSP/GAME/",
            "PSP/GAME/PSX/",
            "PSP/GAME/Utility/",
            "PSP/GAME150/"
        };
        for (int i = 0; i < 4; ++i) {
            if (replaceAfterRoot(gameRoots[i])) return;
        }


        // Fallback for any unexpected layouts: keep old behavior
        size_t pos = pathInOut.find(oldCat);
        if (pos != std::string::npos) {
            pathInOut.replace(pos, oldCat.size(), newCat);
        }
    }


    void cachePatchRenameCategory(const std::string& oldCat, const std::string& newCat) {
        auto doOne = [&](std::map<std::string, std::vector<GameItem>>& cats,
                        std::vector<std::string>& names,
                        bool& hasCats) {
            auto it = cats.find(oldCat);
            if (it == cats.end()) return;

            std::vector<GameItem> moved = std::move(it->second);
            cats.erase(it);
            for (auto &gi : moved) replaceCatSegmentInPath(oldCat, newCat, gi.path);
            auto& dst = cats[newCat];
            dst.clear();
            for (auto &gi : moved) snapInsertSorted(dst, gi);

            for (auto &n : names) if (n == oldCat) { n = newCat; break; }
            std::sort(names.begin(), names.end(),
                    [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
            hasCats = !names.empty();
        };

        // Live
        doOne(categories, categoryNames, hasCategories);

        // Snapshot
        std::string key = rootPrefix(currentDevice);
        auto &snap = deviceCache[key].snap;
        doOne(snap.categories, snap.categoryNames, snap.hasCategories);

        // NEW: remap the "no icon" set
        if (!noIconPaths.empty()) {
            std::unordered_set<std::string> remapped;
            remapped.reserve(noIconPaths.size());
            for (const auto &p : noIconPaths) {
                std::string q = p;
                replaceCatSegmentInPath(oldCat, newCat, q);
                remapped.insert(std::move(q));
            }
            noIconPaths.swap(remapped);
        }

        // NEW: carry selected icon if it’s inside the renamed category
        if (selectionIconTex && selectionIconTex != placeholderIconTexture && !selectionIconKey.empty()) {
            std::string newKey = selectionIconKey;
            replaceCatSegmentInPath(oldCat, newCat, newKey);
            if (newKey != selectionIconKey) {
                iconCarryTex     = selectionIconTex;
                iconCarryForPath = newKey;
                selectionIconTex = nullptr;
                selectionIconKey.clear();
            }
        }
        }



        void cachePatchRenameItem(const std::string& oldPath, const std::string& newPath, GameItem::Kind k) {
            auto apply = [&](ScanSnapshot& s){
                snapErasePath(s, oldPath);
                const std::string dstCat = parseCategoryFromFullPath(newPath, k);
                GameItem gi = makeItemFor(newPath, k);
                snapUpsertItem(s, gi, dstCat);
            };

            // Live members
            {
                ScanSnapshot tmp;
                tmp.categories    = categories;
                tmp.uncategorized = uncategorized;
                tmp.flatAll       = flatAll;
                tmp.categoryNames = categoryNames;
                tmp.hasCategories = hasCategories;

                apply(tmp);

                categories    = std::move(tmp.categories);
                uncategorized = std::move(tmp.uncategorized);
                flatAll       = std::move(tmp.flatAll);
                categoryNames = std::move(tmp.categoryNames);
                hasCategories = tmp.hasCategories;
            }

            // Device snapshot
            {
                std::string key = rootPrefix(currentDevice);
                apply(deviceCache[key].snap);
            }
        }




    // Derive CAT_* (or "") for a PSP path you already normalize elsewhere.
    static std::string deriveCategoryFromPath(const std::string& p) {
        // Examples:
        //   ms0:/ISO/CAT_ARPG/Title.iso          -> CAT_ARPG
        //   ms0:/PSP/GAME/CAT_RPG/MyGame/        -> CAT_RPG
        //   ms0:/ISO/Title.iso                    -> ""
        //   ms0:/PSP/GAME/MyGame/                 -> ""
        auto pos = p.find("CAT_");
        if (pos == std::string::npos) return "";
        // Read token until next slash
        size_t end = p.find('/', pos);
        return (end == std::string::npos) ? p.substr(pos) : p.substr(pos, end - pos);
    }

    // Minimal “make” from known info (kind + new path). Metadata like mtime/icon are resolved lazily in your UI.
    // Fully-populated item factory used by cache patching after Move/Copy.
    // Reads filename/label, extracted title, mtime, size, and sortKey so
    // the destination gets correctly ordered immediately.
    static GameItem makeItemFor(const std::string& newPath, GameItem::Kind k) {
        GameItem gi{};
        gi.path = newPath;
        gi.kind = k;

        // Label from filename or folder name
        gi.label = basenameOf(newPath);

        // Stat for mtime / size, respecting file-or-folder semantics
        SceIoStat st{};
        if (k == GameItem::ISO_FILE) {
            if (sceIoGetstat(newPath.c_str(), &st) >= 0) {
                gi.time     = st.sce_st_mtime;
                gi.sizeBytes= (uint64_t)st.st_size;
            }
        } else {
            // EBOOT folder: stat the directory (no trailing slash)
            if (getStatDirNoSlash(newPath, st)) {
                gi.time     = st.sce_st_mtime;
                uint64_t folderBytes = 0;
                sumDirBytes(newPath, folderBytes);
                gi.sizeBytes = folderBytes;
            }
            gi.isUpdateDlc = isUpdateDlcFolder(newPath);
            fillEbootIconPaths(gi);
        }
        gi.sortKey = buildLegacySortKey(gi.time);

        // Title extraction (ISO/CSO/ZSO/DAX/JSO vs EBOOT folder)
        if (k == GameItem::ISO_FILE) {
            if      (endsWithNoCase(newPath, ".iso")) { std::string t; if (readIsoTitle(newPath, t))         gi.title = t; }
            else if (endsWithNoCase(newPath, ".cso") || endsWithNoCase(newPath, ".zso")) {
                std::string t; if (readCompressedIsoTitle(newPath, t)) gi.title = t;
            }
            else if (endsWithNoCase(newPath, ".dax")) { std::string t; if (readDaxTitle(newPath, t))         gi.title = t; }
            else if (endsWithNoCase(newPath, ".jso")) { std::string t; if (readJsoTitle(newPath, t))         gi.title = t; }
        } else {
            std::string t; if (getFolderTitle(newPath, t)) gi.title = t;
        }
        sanitizeTitleInPlace(gi.title);
        return gi;
    }


    // Patches both snapshots to reflect Move/Copy outcome.
    // When isMove==true: it erases from srcSnap and inserts into dstSnap.
    // When isMove==false: it leaves src as-is and inserts into dst.
    static void cacheApplyMoveOrCopy(ScanSnapshot& srcSnap, ScanSnapshot& dstSnap,
                                    const std::string& src, const std::string& dst,
                                    GameItem::Kind k, bool isMove) {
        // Remove from source lists first when moving
        if (isMove) snapErasePath(srcSnap, src);

        // Build fully refreshed metadata for the destination
        GameItem gi = makeItemFor(dst, k);

        // Insert into destination lists in correct category and sorted order
        const std::string dstCat = parseCategoryFromFullPath(dst, k);
        snapUpsertItem(dstSnap, gi, dstCat);
    }



    void rebuildFlatFromCache() {
        // Mirror openDevice() behavior when Categories Lite is Off
        workingList = (gclArkOn || gclProOn) ? flatAll : uncategorized;
        sortLikeLegacy(workingList);
        view = View_AllFlat;
        clearUI();
        for (const auto& gi : workingList){
            SceIoDirent e; memset(&e,0,sizeof(e));
            const char* name = (showTitles && !gi.title.empty()) ? gi.title.c_str() : gi.label.c_str();
            strncpy(e.d_name, name, sizeof(e.d_name)-1);
            entries.push_back(e);
            entryPaths.push_back(gi.path);
            entryKinds.push_back(gi.kind);
        }
        showRoots = false;
    }


    // -----------------------------
    // Drawing helpers (unchanged)
    // -----------------------------
    struct Vertex { unsigned int color; short x,y,z; };
    void drawRect(int x,int y,int w,int h,unsigned col) {
        Vertex* v = (Vertex*)sceGuGetMemory(2*sizeof(Vertex));
        v[0] = {col, (short)x,       (short)y,       0};
        v[1] = {col, (short)(x+w),   (short)(y+h),   0};
        sceGuDisable(GU_TEXTURE_2D);
        sceGuShadeModel(GU_FLAT);
        sceGuAmbientColor(0xFFFFFFFF);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 2, 0, v);
        sceGuEnable(GU_TEXTURE_2D);
    }
    void drawHFadeLine(int x, int y, int w, int h, uint8_t midAlpha, int fadePx, uint32_t rgb) {
        if (w <= 0 || h <= 0) return;
        if (fadePx * 2 > w) fadePx = w / 2;
        if (fadePx < 1) fadePx = 1;

        const int steps = 6;
        auto colWithAlpha = [&](uint8_t a)->unsigned { return (unsigned(a) << 24) | (rgb & 0x00FFFFFF); };

        // Left fade
        for (int i = 0; i < steps; ++i) {
            int x0 = x + (fadePx * i) / steps;
            int x1 = x + (fadePx * (i + 1)) / steps;
            int segW = x1 - x0;
            if (segW <= 0) continue;
            uint8_t a = (uint8_t)((midAlpha * (i + 1)) / steps);
            drawRect(x0, y, segW, h, colWithAlpha(a));
        }

        // Middle solid
        int midW = w - (fadePx * 2);
        if (midW > 0) drawRect(x + fadePx, y, midW, h, colWithAlpha(midAlpha));

        // Right fade
        for (int i = 0; i < steps; ++i) {
            int x0 = x + w - fadePx + (fadePx * i) / steps;
            int x1 = x + w - fadePx + (fadePx * (i + 1)) / steps;
            int segW = x1 - x0;
            if (segW <= 0) continue;
            uint8_t a = (uint8_t)((midAlpha * (steps - i)) / steps);
            drawRect(x0, y, segW, h, colWithAlpha(a));
        }
    }
    void drawPieSlice(float cx, float cy, float r, float startRad, float endRad, unsigned col) {
        if (r <= 0.0f || endRad <= startRad) return;
        const float span = endRad - startRad;
        const float full = 6.2831853f;
        int segs = (int)(span / full * 40.0f + 0.5f);
        if (segs < 2) segs = 2;

        struct V { unsigned color; float x,y,z; };
        V* v = (V*)sceGuGetMemory((segs + 2) * sizeof(V));
        v[0] = { col, cx, cy, 0.0f };
        for (int i = 0; i <= segs; ++i) {
            float t = (float)i / (float)segs;
            float a = startRad + span * t;
            v[i + 1] = { col, cx + cosf(a) * r, cy + sinf(a) * r, 0.0f };
        }
        sceGuDisable(GU_TEXTURE_2D);
        sceGuShadeModel(GU_FLAT);
        sceGuAmbientColor(0xFFFFFFFF);
        sceGuDrawArray(GU_TRIANGLE_FAN, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       segs + 2, 0, v);
        sceGuEnable(GU_TEXTURE_2D);
    }
    void drawPieRingSegment(float cx, float cy, float rOuter, float rInner,
                            float startRad, float endRad, unsigned col) {
        if (rOuter <= rInner || endRad <= startRad) return;
        const float span = endRad - startRad;
        const float full = 6.2831853f;
        int segs = (int)(span / full * 40.0f + 0.5f);
        if (segs < 2) segs = 2;

        struct V { unsigned color; float x,y,z; };
        V* v = (V*)sceGuGetMemory((segs + 1) * 2 * sizeof(V));
        for (int i = 0; i <= segs; ++i) {
            float t = (float)i / (float)segs;
            float a = startRad + span * t;
            float ca = cosf(a);
            float sa = sinf(a);
            v[i * 2 + 0] = { col, cx + ca * rOuter, cy + sa * rOuter, 0.0f };
            v[i * 2 + 1] = { col, cx + ca * rInner, cy + sa * rInner, 0.0f };
        }
        sceGuDisable(GU_TEXTURE_2D);
        sceGuShadeModel(GU_FLAT);
        sceGuAmbientColor(0xFFFFFFFF);
        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       (segs + 1) * 2, 0, v);
        sceGuEnable(GU_TEXTURE_2D);
    }
    static bool utf8Next(const char* s, size_t len, size_t& i, uint32_t& cp) {
        if (i >= len) return false;
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { cp = c; i += 1; return true; }
        if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            i += 2; return true;
        }
        if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            i += 3; return true;
        }
        if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
                 ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            i += 4; return true;
        }
        // invalid byte, skip
        cp = c;
        i += 1;
        return true;
    }
    static bool isJapaneseCp(uint32_t cp) {
        return (cp >= 0x3040 && cp <= 0x309F) || // Hiragana
               (cp >= 0x30A0 && cp <= 0x30FF) || // Katakana
               (cp >= 0x31F0 && cp <= 0x31FF) || // Katakana Phonetic Extensions
               (cp >= 0x3400 && cp <= 0x4DBF) || // CJK Ext A
               (cp >= 0x4E00 && cp <= 0x9FFF) || // CJK Unified
               (cp >= 0xFF66 && cp <= 0xFF9D);   // Halfwidth Katakana
    }
    static bool isKoreanCp(uint32_t cp) {
        return (cp >= 0x1100 && cp <= 0x11FF) || // Hangul Jamo
               (cp >= 0x3130 && cp <= 0x318F) || // Hangul Compatibility Jamo
               (cp >= 0xAC00 && cp <= 0xD7AF);   // Hangul Syllables
    }
    intraFont* pickFontForText(const char* s) {
        if (!font || !s || !*s) return font;
        if (!fontJpn && !fontKr) return font;
        bool wantKr = false, wantJpn = false;
        bool sawNonAscii = false;
        bool invalidUtf8 = false;
        size_t len = strlen(s);
        for (size_t i = 0; i < len; ) {
            if (((unsigned char)s[i]) & 0x80) sawNonAscii = true;
            uint32_t cp = 0;
            if (!utf8Next(s, len, i, cp)) { invalidUtf8 = true; break; }
            if (!wantKr && isKoreanCp(cp)) wantKr = true;
            if (!wantJpn && isJapaneseCp(cp)) wantJpn = true;
            if (wantKr || wantJpn) break;
        }
        if (wantKr && fontKr) return fontKr;
        if ((wantJpn || invalidUtf8 || sawNonAscii) && fontJpn) return fontJpn;
        return font;
    }
    void drawTextAligned(float x,float y,const char* s,unsigned col,int align) {
        if (font) {
            intraFont* f = pickFontForText(s);
            if (f) {
                intraFontActivate(f);
                intraFontSetStyle(f,0.5f,col,0,0.0f,align);
                intraFontPrint(f,x,y,s);
                return;
            }
        } else {
            int cx = int(x/8);
            if (align == INTRAFONT_ALIGN_CENTER) {
                cx -= (int)strlen(s) / 2;
            } else if (align == INTRAFONT_ALIGN_RIGHT) {
                cx -= (int)strlen(s);
            }
            if (cx < 0) cx = 0;
            pspDebugScreenSetXY(cx, int(y/8));
            pspDebugScreenSetTextColor(col);
            pspDebugScreenPrintf("%s", s);
        }
    }
    void drawText(float x,float y,const char* s,unsigned col) {
        drawTextAligned(x, y, s, col, INTRAFONT_ALIGN_LEFT);
    }
    float measureTextWidth(float size, const char* s) {
        if (!s) return 0.0f;
        if (!font) return (float)(strlen(s) * 8) * size;
        intraFont* f = pickFontForText(s);
        if (!f) return (float)(strlen(s) * 8) * size;
        intraFontActivate(f);
        intraFontSetStyle(f, size, COLOR_WHITE, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
        return intraFontMeasureText(f, s);
    }
    void drawTextStyled(float x, float y, const char* s, float size, unsigned col, unsigned shadow,
                        int align, bool bold) {
        if (!s) return;
        if (font) {
            intraFont* f = pickFontForText(s);
            if (f) {
                intraFontActivate(f);
                intraFontSetStyle(f, size, col, shadow, 0.0f, align);
                intraFontPrint(f, x, y, s);
                if (bold) intraFontPrint(f, x + 1.0f, y, s);
                return;
            }
        } else {
            drawTextAligned(x, y, s, col, align);
            if (bold) drawTextAligned(x + 1.0f, y, s, col, align);
        }
    }
    void drawTextureScaled(Texture* t, float x, float y, float targetH, unsigned color) {
        if (!t || !t->data || targetH <= 0.0f) return;
        const int w = t->width;
        const int h = t->height;
        const int tbw = t->stride;
        if (w <= 0 || h <= 0) return;
        int th = 1; while (th < h) th <<= 1;
        float s = targetH / (float)h;
        float dw = (float)w * s;
        float dh = targetH;

        sceKernelDcacheWritebackRange(t->data, tbw * h * 4);
        sceGuTexFlush();
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, th, tbw, t->data);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuEnable(GU_TEXTURE_2D);

        struct V { float u,v; unsigned color; float x,y,z; };
        V* vtx = (V*)sceGuGetMemory(2 * sizeof(V));
        vtx[0] = { 0.0f,            0.0f,            color, x,      y,      0.0f };
        vtx[1] = { (float)w, (float)h, color, x + dw, y + dh, 0.0f };
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 |
                                  GU_VERTEX_32BITF  | GU_TRANSFORM_2D, 2, nullptr, vtx);
        sceGuDisable(GU_TEXTURE_2D);
    }
    void drawTextureScaledTight(Texture* t, float x, float y, float targetH, unsigned color) {
        if (!t || !t->data || targetH <= 0.0f) return;
        const int w = t->width;
        const int h = t->height;
        const int tbw = t->stride;
        if (w <= 0 || h <= 0) return;
        int th = 1; while (th < h) th <<= 1;
        float s = targetH / (float)h;
        float dw = (float)w * s;
        float dh = targetH;

        sceKernelDcacheWritebackRange(t->data, tbw * h * 4);
        sceGuTexFlush();
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, th, tbw, t->data);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuEnable(GU_TEXTURE_2D);

        struct V { float u,v; unsigned color; float x,y,z; };
        V* vtx = (V*)sceGuGetMemory(2 * sizeof(V));
        vtx[0] = { 0.5f,           0.5f,           color, x,      y,      0.0f };
        vtx[1] = { (float)w - 0.5f, (float)h - 0.5f, color, x + dw, y + dh, 0.0f };
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 |
                                  GU_VERTEX_32BITF  | GU_TRANSFORM_2D, 2, nullptr, vtx);
        sceGuDisable(GU_TEXTURE_2D);
    }
    static std::string ellipsizeText(const std::string& s, size_t maxChars) {
        if (s.size() <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, maxChars);
        return s.substr(0, maxChars - 3) + "...";
    }
    std::string currentCategoryHeaderLabel() const {
        if (view == View_CategoryContents) {
            if (!currentCategory.empty()) return currentCategory;
            return "Uncategorized";
        }
        if (view == View_AllFlat) {
            if (gclArkOn || gclProOn) return "All content";
            return "Uncategorized";
        }
        return currentDeviceHeaderName();
    }
    void drawHeader() {
        const int bannerH = 15;
        const float textY = (float)(bannerH - 4);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        drawRect(0, 0, SCREEN_WIDTH, bannerH, COLOR_BANNER);

        std::string leftLabel = "Homebrew Sorter Ultimate";
        Texture* deviceIcon = nullptr;
        bool underlineLabel = false;
        const bool opHeader = (actionMode != AM_None &&
                               (opPhase == OP_SelectDevice || opPhase == OP_SelectCategory ||
                                opPhase == OP_Confirm));

        auto pickDeviceIcon = [&]() {
            if (!strncasecmp(currentDevice.c_str(), "ms0:", 4)) {
                deviceIcon = memcardSmallIcon;
            } else if (!strncasecmp(currentDevice.c_str(), "ef0:", 4)) {
                deviceIcon = internalSmallIcon;
            }
        };
        auto pickDeviceIconFor = [&](const std::string& dev) {
            if (!strncasecmp(dev.c_str(), "ms0:", 4)) {
                deviceIcon = memcardSmallIcon;
            } else if (!strncasecmp(dev.c_str(), "ef0:", 4)) {
                deviceIcon = internalSmallIcon;
            }
        };

        if (opHeader) {
            leftLabel = (actionMode == AM_Copy) ? "Copy Operation" : "Move Operation";
            deviceIcon = nullptr;
            if ((opPhase == OP_SelectCategory || opPhase == OP_Confirm) &&
                !opDestDevice.empty() && opDestDevice[0] != '_') {
                pickDeviceIconFor(opDestDevice);
            }
            underlineLabel = false;
        } else if (!showRoots && (view == View_Categories || view == View_GclSettings)) {
            leftLabel = currentDeviceHeaderName();
            pickDeviceIcon();
        } else if (!showRoots && (view == View_CategoryContents || view == View_AllFlat)) {
            leftLabel = ellipsizeText(currentCategoryHeaderLabel(), 23);
            underlineLabel = true;
            pickDeviceIcon();
        }

        float textX = 5.0f;
        // Draw device icon if applicable (11px tall, positioned 2px from top)
        if (deviceIcon && deviceIcon->data) {
            const float iconH = 11.0f;
            const float iconY = 2.0f;
            const float iconW = (float)deviceIcon->width * (iconH / (float)deviceIcon->height);
            drawTextureScaled(deviceIcon, textX, iconY, iconH, 0xFFFFFFFF);
            textX += iconW + 3.0f;  // Icon width + 3px gap
        }

        drawTextAligned(textX, textY, leftLabel.c_str(), COLOR_WHITE, INTRAFONT_ALIGN_LEFT);
        if (underlineLabel && leftLabel.find('_') != std::string::npos) {
            const float scale = 0.5f;
            const float underscoreW = measureTextWidth(scale, "_");
            const float underlineY = textY + 2.0f;
            std::string prefix;
            prefix.reserve(leftLabel.size());
            for (size_t ci = 0; ci < leftLabel.size(); ++ci) {
                if (leftLabel[ci] == '_') {
                    float prefixW = measureTextWidth(scale, prefix.c_str());
                    float ux = textX + prefixW;
                    int lineW = (int)(underscoreW + 1.0f);
                    if (lineW < 1) lineW = 1;
                    drawRect((int)(ux), (int)(underlineY - 1.0f), lineW, 1, COLOR_WHITE);
                }
                prefix.push_back(leftLabel[ci]);
            }
        }

        char mid[64];
        float midX = 195.0f;
        int midAlign = INTRAFONT_ALIGN_LEFT;
        if (opHeader) {
            const int count = (opSrcCount > 0) ? opSrcCount : (int)opSrcPaths.size();
            const std::string total = humanSize3(opSrcTotalBytes);
            const char* appWord = (count == 1) ? "App" : "Apps";
            snprintf(mid, sizeof(mid), "%d %s / %s", count, appWord, total.c_str());
            midX = SCREEN_WIDTH * 0.5f;
            midAlign = INTRAFONT_ALIGN_CENTER;
        } else if (showRoots) {
            snprintf(mid, sizeof(mid), "Main Menu");
            midX = 215.0f;
        } else if (view == View_Categories) {
            snprintf(mid, sizeof(mid), "Categories: %d", categoryDisplayCount());
            midX = SCREEN_WIDTH / 2.0f;
            midAlign = INTRAFONT_ALIGN_CENTER;
        } else if (view == View_GclSettings) {
            snprintf(mid, sizeof(mid), "Categories Settings");
            midX = SCREEN_WIDTH / 2.0f;
            midAlign = INTRAFONT_ALIGN_CENTER;
        } else if (view == View_CategoryContents || view == View_AllFlat) {
            int appCount = (int)workingList.size();
            int selectedCount = 0;
            uint64_t selectedBytes = 0;
            if (!checked.empty()) {
                for (const auto& gi : workingList) {
                    if (checked.find(gi.path) != checked.end()) {
                        ++selectedCount;
                        selectedBytes += gi.sizeBytes;
                    }
                }
            }
            if (selectedCount > 0) {
                const std::string total = humanSize3(selectedBytes);
                snprintf(mid, sizeof(mid), "%d Selected / %s", selectedCount, total.c_str());
            } else {
                snprintf(mid, sizeof(mid), "Apps Found: %d", appCount);
                midX += 4.0f;
            }
        }
        drawTextAligned(midX, textY, mid, COLOR_WHITE, midAlign);

        ScePspDateTime now{};
        sceRtcGetCurrentClockLocalTime(&now);
        char date[32];
        unsigned hour12 = (unsigned)now.hour % 12;
        if (hour12 == 0) hour12 = 12;
        const char* ampm = ((unsigned)now.hour < 12) ? "AM" : "PM";
        snprintf(date, sizeof(date), "%02u/%02u/%04u %02u:%02u %s",
                 (unsigned)now.month, (unsigned)now.day, (unsigned)now.year,
                 hour12, (unsigned)now.minute, ampm);
        drawTextAligned(SCREEN_WIDTH - 5.0f, textY, date, COLOR_WHITE, INTRAFONT_ALIGN_RIGHT);

        if (showRoots) {
            if (actionMode != AM_None && !opHeader) {
                const char* v = (actionMode == AM_Copy) ? "Copy" : "Move";
                std::string msg = std::string("Select Destination Storage (") + v + " Mode)";
                drawText(10, 25, msg.c_str(), COLOR_WHITE);
            }
        } else if (!(view == View_CategoryContents || view == View_AllFlat)) {
            char buf[256];
            const char* lbl = showTitles ? "App Title" : "File/Folder";
            if (view == View_Categories || view == View_GclSettings) {
                buf[0] = '\0'; // no secondary header text on Categories screen
            } else if (view == View_CategoryContents) {
                snprintf(buf, sizeof(buf), "Category: %s — %s  | Label: %s", currentCategory.c_str(), rootDisplayName(currentDevice.c_str()), lbl);
            } else {
                snprintf(buf, sizeof(buf), "%s — All content  | Label: %s", rootDisplayName(currentDevice.c_str()), lbl);
            }
            drawText(10,25,buf,COLOR_WHITE);
        }

    }

    void freeSelectionIcon() {
        if (selectionIconTex && selectionIconTex != placeholderIconTexture) {
            texFree(selectionIconTex);
        }
        selectionIconTex = nullptr;
        selectionIconKey.clear();
    }

    void freeCategoryIcon() {
        if (categoryIconTex) {
            texFree(categoryIconTex);
        }
        categoryIconTex = nullptr;
        categoryIconKey.clear();
        categoryIconMissing = false;
    }

    std::string findCategoryIconPath(const std::string& cat) const {
        if (cat.empty()) return {};
        if (!strcasecmp(cat.c_str(), kCatSettingsLabel)) return {};
        if (!strcasecmp(cat.c_str(), "Uncategorized")) return {};
        if (currentDevice.empty()) return {};

        const char* roots[] = {"ISO/","PSP/GAME/","PSP/GAME150/","PSP/GAME/PSX/","PSP/GAME/Utility/"};
        for (const char* r : roots) {
            std::string dir = currentDevice + std::string(r) + cat;
            if (!dirExists(dir)) continue;
            std::string iconPath = findFileCaseInsensitive(dir, "ICON0.PNG");
            if (!iconPath.empty()) return iconPath;
        }
        return {};
    }

    void ensureCategoryIcon() {
        if (msgBox || showRoots || view != View_Categories || selectedIndex < 0 || selectedIndex >= (int)entries.size()) {
            freeCategoryIcon();
            return;
        }
        const char* name = entries[selectedIndex].d_name;
        if (!name || !name[0]) { freeCategoryIcon(); return; }
        const std::string key(name);
        if (key == categoryIconKey && (categoryIconTex || categoryIconMissing)) return;

        freeCategoryIcon();
        std::string iconPath = findCategoryIconPath(key);
        if (iconPath.empty()) {
            categoryIconKey = key;
            categoryIconMissing = true;
            return;
        }
        Texture* t = texLoadPNG(iconPath.c_str());
        if (!t || !t->data) {
            if (t) texFree(t);
            categoryIconKey = key;
            categoryIconMissing = true;
            return;
        }
        categoryIconTex = t;
        categoryIconKey = key;
        categoryIconMissing = false;
    }

    void drawCategoryIconLowerRight() {
        if (!categoryIconTex || !categoryIconTex->data) return;

        const int boxW = 144, boxH = 80;
        const float ctrlX = 290.0f;
        const float ctrlW = 185.0f;
        const int controlsTop = SCREEN_HEIGHT - 30;

        const int w   = categoryIconTex->width;
        const int h   = categoryIconTex->height;
        const int tbw = categoryIconTex->stride;

        float sx = (float)boxW / (float)w;
        float sy = (float)boxH / (float)h;
        float s  = (sx < sy) ? sx : sy;
        if (s > 1.0f) s = 1.0f;
        int dw = (int)(w * s), dh = (int)(h * s);

        const int boxX = (int)(ctrlX + (ctrlW - boxW) * 0.5f);
        const int boxY = controlsTop - 6 - boxH + 12;
        int x = boxX + (boxW - dw) / 2 + 1;
        int y = boxY + (boxH - dh) / 2;

        sceKernelDcacheWritebackRange(categoryIconTex->data, tbw * h * 4);
        sceGuTexFlush();

        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, tbw, tbw, categoryIconTex->data);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuEnable(GU_TEXTURE_2D);

        const float u0 = 0.0f, v0 = 0.0f;
        const float u1 = (float)w - 0.5f;
        const float v1 = (float)h - 0.5f;

        struct V { float u,v; unsigned color; float x,y,z; };
        V* vtx = (V*)sceGuGetMemory(2 * sizeof(V));
        vtx[0] = { u0, v0, 0xFFFFFFFF, (float)x,      (float)y,      0.0f };
        vtx[1] = { u1, v1, 0xFFFFFFFF, (float)(x+dw), (float)(y+dh), 0.0f };
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 |
                                  GU_VERTEX_32BITF  | GU_TRANSFORM_2D, 2, nullptr, vtx);
        sceGuDisable(GU_TEXTURE_2D);
    }

    Texture* loadIconForGameItem(const GameItem& gi) {
        if (gi.kind == GameItem::EBOOT_FOLDER) {
            if (!gi.iconPath.empty()) {
                if (Texture* t = texLoadPNG(gi.iconPath.c_str())) return t;
            }
            if (!gi.pbpPath.empty()) {
                if (Texture* t = loadIconFromPBP(gi.pbpPath)) return t;
            }
            // Fallback: legacy scan if cached paths are empty/outdated
            std::string iconPath = findFileCaseInsensitive(gi.path, "ICON0.PNG");
            if (!iconPath.empty()) {
                if (Texture* t = texLoadPNG(iconPath.c_str())) return t;
            }
            std::string eboot = findEbootCaseInsensitive(gi.path);
            if (!eboot.empty()) {
                if (Texture* t = loadIconFromPBP(eboot)) return t;
            }
            return nullptr;
        }
        const std::string& p = gi.path;
        if (endsWithNoCase(p, ".iso")) {
            return loadIsoIconPNG(p);
        } else if (endsWithNoCase(p, ".cso") || endsWithNoCase(p, ".zso")) {
            return loadCompressedIsoIconPNG(p);
        } else if (endsWithNoCase(p, ".jso")) {
            std::vector<uint8_t> png;
            if (readJsoIconPNG(p, png) && !png.empty()) return texLoadPNGFromMemory(png.data(), (int)png.size());
            return nullptr;
        } else if (endsWithNoCase(p, ".dax")) {
            std::vector<uint8_t> png;
            if (readDaxIconPNG(p, png) && !png.empty()) return texLoadPNGFromMemory(png.data(), (int)png.size());
            return nullptr;
        }
        return nullptr;
    }

    bool ebootHasIconSource(const GameItem& gi) {
        if (gi.kind != GameItem::EBOOT_FOLDER) return false;
        if (!gi.iconPath.empty() || !gi.pbpPath.empty()) return true;
        if (!findFileCaseInsensitive(gi.path, "ICON0.PNG").empty()) return true;
        return !findEbootCaseInsensitive(gi.path).empty();
    }

    void ensureSelectionIcon() {
        if (msgBox || showRoots || !(view==View_AllFlat || view==View_CategoryContents)
            || selectedIndex < 0 || selectedIndex >= (int)entries.size()
            || FIO_S_ISDIR(entries[selectedIndex].d_stat.st_mode)) {
            freeSelectionIcon();
            return;
        }
        if (selectedIndex >= (int)workingList.size()) { freeSelectionIcon(); return; }

        const GameItem& gi = workingList[selectedIndex];
        const std::string key = gi.path;

        // If we just renamed this item, restore the previously displayed ICON0 immediately.
        if (iconCarryTex && key == iconCarryForPath) {
            selectionIconTex = iconCarryTex;
            selectionIconKey = iconCarryForPath;
            iconCarryTex = nullptr;
            iconCarryForPath.clear();
            return;
        }

        if (key == selectionIconKey && selectionIconTex) return;

        freeSelectionIcon();

        if (noIconPaths.find(key) != noIconPaths.end()) {
            selectionIconTex = placeholderIconTexture;
            selectionIconKey = key;
            return;
        }

        Texture* t = loadIconForGameItem(gi);
        if (t) {
            selectionIconTex = t;
        } else {
            selectionIconTex = placeholderIconTexture;
            const bool cacheFailure = (gi.kind == GameItem::EBOOT_FOLDER) && !ebootHasIconSource(gi);
            if (cacheFailure) noIconPaths.insert(key);
        }
        selectionIconKey = key;
    }


    void drawSelectedIconLowerRight() {
        if (!selectionIconTex || !selectionIconTex->data) return;

        const int boxW = 144, boxH = 80;
        const float ctrlX = 290.0f;
        const float ctrlW = 185.0f;
        const int controlsTop = SCREEN_HEIGHT - 30;

        const int w   = selectionIconTex->width;
        const int h   = selectionIconTex->height;
        const int tbw = selectionIconTex->stride;

        float sx = (float)boxW / (float)w;
        float sy = (float)boxH / (float)h;
        float s  = (sx < sy) ? sx : sy;
        if (s > 1.0f) s = 1.0f;
        int dw = (int)(w * s), dh = (int)(h * s);

        const int boxX = (int)(ctrlX + (ctrlW - boxW) * 0.5f);
        const int boxY = controlsTop - 6 - boxH + 12;
        int x = boxX + (boxW - dw) / 2 + 1;
        int y = boxY + (boxH - dh) / 2;

        sceKernelDcacheWritebackRange(selectionIconTex->data, tbw * h * 4);
        sceGuTexFlush();

        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, tbw, tbw, selectionIconTex->data);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuEnable(GU_TEXTURE_2D);

        const float u0 = 0.0f, v0 = 0.0f;
        const float u1 = (float)w - 0.5f;
        const float v1 = (float)h - 0.5f;

        struct V { float u,v; unsigned color; float x,y,z; };
        V* vtx = (V*)sceGuGetMemory(2 * sizeof(V));
        vtx[0] = { u0, v0, 0xFFFFFFFF, (float)x,      (float)y,      0.0f };
        vtx[1] = { u1, v1, 0xFFFFFFFF, (float)(x+dw), (float)(y+dh), 0.0f };
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 |
                                  GU_VERTEX_32BITF  | GU_TRANSFORM_2D, 2, nullptr, vtx);
        sceGuDisable(GU_TEXTURE_2D);
    }

    void drawRootMenu() {
        if (entries.empty()) return;

        intraFontActivate(font);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,0,0);

        const float panelX = 5.0f;
        const float panelY = 22.0f;
        const float panelW = 280.0f;
        const float panelH = 226.0f;
        drawRect((int)panelX, (int)(panelY - 1.0f), (int)panelW, (int)(panelH + 1.0f), COLOR_BANNER);

        const float ctrlX = 290.0f;
        const float ctrlY = 22.0f;
        const float ctrlW = 185.0f;
        const bool devicePicker = (opPhase == OP_SelectDevice);

        if (devicePicker) {
            const float ctrlH = 39.0f;
            drawRect((int)ctrlX, (int)(ctrlY - 1.0f), (int)ctrlW, (int)(ctrlH + 1.0f), COLOR_BANNER);
            const unsigned keyTextCol = 0xFFBBBBBB;
            auto drawKeyRowLeft = [&](float baseX, float& y, Texture* icon, const char* label,
                                      bool bumpRight, unsigned textCol){
                float iconH = 15.0f;
                float iconW = 0.0f;
                if (icon && icon->data && icon->height > 0) {
                    iconW = (float)icon->width * (iconH / (float)icon->height);
                }
                const float gap = (iconW > 0.0f) ? (6.0f + (bumpRight ? 4.0f : 0.0f)) : 0.0f;
                float x = baseX + (bumpRight ? 6.0f : 0.0f);
                if (bumpRight) x += 4.0f;
                if (icon && icon->data && icon->height > 0) {
                    drawTextureScaled(icon, x, y - 10.0f, iconH, 0xFFFFFFFF);
                    x += iconW + gap;
                }
                const float labelPad = bumpRight ? 6.0f : 0.0f;
                drawTextStyled(x + labelPad, y + 2.0f, label, 0.7f, textCol, 0, INTRAFONT_ALIGN_LEFT, false);
                y += 17.0f;
            };

            float keyY = ctrlY + 13.0f;
            const float keyX = ctrlX + 5.0f;
            drawKeyRowLeft(keyX, keyY, okIconTexture, "Select", true, keyTextCol);
            drawKeyRowLeft(keyX, keyY, circleIconTexture, "Back", true, keyTextCol);

            const int rowCount = (int)entries.size();
            const float top = panelY + 4.0f;
            const float bottom = panelY + panelH - 4.0f;
            const float rowH = (rowCount > 0) ? ((bottom - top) / (float)rowCount) : 0.0f;
            const float deviceShiftX = 12.0f;
            const float textCenterX = panelX + (panelW * 0.5f) + deviceShiftX;
            const float iconGap = 10.0f;
            const float iconYOffsetBase = -6.0f;

            const unsigned freeCol = 0xFFBBBBBB;
            const unsigned usedCol = 0xFF17D0FD; // same as "Save List Order"
            const unsigned needWarnCol = COLOR_RED;
            const unsigned needOkCol = 0xFFFFC88C; // baby blue (ABGR)
            const unsigned pieBgCol = 0x66BBBBBB; // 40% gray

            for (int i = 0; i < rowCount; ++i) {
                const char* name = entries[i].d_name;
                if (!name || (!strcmp(name, "__USB_MODE__") || !strcmp(name, "__GCL_TOGGLE__"))) continue;

                const bool sel = (i == selectedIndex);
                const bool disabled = (i < (int)rowFlags.size() && (rowFlags[i] & ROW_DISABLED));
                const bool present = (i < (int)rowPresent.size()) ? (rowPresent[i] != 0) : !disabled;
                const unsigned baseCol = disabled ? COLOR_GRAY : COLOR_WHITE;
                const unsigned textCol = sel ? COLOR_BLACK : baseCol;
                const unsigned shadowCol = sel ? COLOR_WHITE : 0x40000000;
                const unsigned iconCol = disabled ? 0x66FFFFFF : 0xFFFFFFFF;

                const int rowIndex = i;
                const float rowTop = top + rowH * rowIndex;
                const float centerY = rowTop + rowH * 0.5f;
                float rowShift = 0.0f;
                if (!strcmp(name, "ms0:/")) rowShift = -3.0f;
                else if (!strcmp(name, "ef0:/")) rowShift = 3.0f;
                const float nameCenterY = (centerY - 28.0f) + rowShift;
                const float pieCenterY = (centerY + 25.0f) + rowShift;

                const char* lines[2] = { nullptr, nullptr };
                int lineCount = 1;
                float scale = 0.75f;
                Texture* icon = nullptr;
                float iconH = 0.0f;

                if (!strcmp(name, "ms0:/")) {
                    lines[0] = "Memory"; lines[1] = "Stick"; lineCount = 2;
                    scale = 0.95f; icon = rootMemIcon; iconH = 40.0f;
                } else if (!strcmp(name, "ef0:/")) {
                    lines[0] = "Internal"; lines[1] = "Storage"; lineCount = 2;
                    scale = 0.95f; icon = rootInternalIcon; iconH = 40.0f;
                } else {
                    lines[0] = rootDisplayName(name);
                    scale = 0.6f; icon = nullptr; iconH = 0.0f;
                }

                float rowYOffset = 0.0f;
                if (!strcmp(name, "ms0:/")) rowYOffset = 4.0f;
                else if (!strcmp(name, "ef0:/")) rowYOffset = 5.0f;

                const float lineH = 16.0f * scale;
                const float blockH = lineH * (float)lineCount;
                const float firstBaseline = (nameCenterY + rowYOffset) - (blockH * 0.5f) + (lineH * 0.75f);

                float maxW = 0.0f;
                for (int l = 0; l < lineCount; ++l) {
                    float w = measureTextWidth(scale, lines[l]);
                    if (w > maxW) maxW = w;
                }
                const float textLeftX = textCenterX - (maxW * 0.5f);

                if (icon && iconH > 0.0f && icon->height > 0) {
                    float iconW = (float)icon->width * (iconH / (float)icon->height);
                    float iconX = textLeftX - iconGap - iconW;
                    if (!strcmp(name, "ms0:/")) iconX -= 6.0f;
                    float iconY = (nameCenterY + rowYOffset) - (iconH * 0.5f) + iconYOffsetBase + 5.0f;
                    drawTextureScaled(icon, iconX, iconY, iconH, iconCol);
                }

                const float lineGap = (!strcmp(name, "ms0:/") || !strcmp(name, "ef0:/")) ? 2.0f : 0.0f;
                for (int l = 0; l < lineCount; ++l) {
                    drawTextStyled(textCenterX, firstBaseline + (lineH * l) + (lineGap * l),
                                   lines[l], scale, textCol, shadowCol, INTRAFONT_ALIGN_CENTER, true);
                }

                const bool hideCopyMsInfo =
                    (actionMode == AM_Copy && runningFromEf0 && !strcmp(name, "ms0:/"));
                const bool showStorageInfo = present && !hideCopyMsInfo;
                if (showStorageInfo) {
                    const float pieR = 22.0f;
                    const float pieX = panelX + 60.0f + deviceShiftX;
                    const float infoX = pieX + pieR + 8.0f;
                    const float infoScale = 0.6f;
                    const float infoLineH = 16.0f * infoScale + 3.0f;

                    uint64_t freeB = (i < (int)rowFreeBytes.size()) ? rowFreeBytes[i] : 0;
                    uint64_t needB = (i < (int)rowNeedBytes.size()) ? rowNeedBytes[i] : 0;
                    uint64_t totalB = (i < (int)rowTotalBytes.size()) ? rowTotalBytes[i] : 0;
                    if (present && totalB == 0) {
                        getTotalBytesCMF(name, totalB);
                        if (i < (int)rowTotalBytes.size()) rowTotalBytes[i] = totalB;
                    }
                    uint64_t usedB = (totalB > freeB) ? (totalB - freeB) : 0;

                    const bool isSameMove = (actionMode == AM_Move && !preOpDevice.empty() && !strcasecmp(preOpDevice.c_str(), name));
                    if (isSameMove) needB = 0;
                    const bool noSpace = (i < (int)rowReason.size()) && (rowReason[i] == RD_NO_SPACE);
                    const unsigned needCol = noSpace ? needWarnCol : needOkCol;

                    float usedRatio = (totalB > 0) ? (float)usedB / (float)totalB : 0.0f;
                    float needRatio = (totalB > 0) ? (float)needB / (float)totalB : 0.0f;
                    if (usedRatio < 0.0f) usedRatio = 0.0f;
                    if (usedRatio > 1.0f) usedRatio = 1.0f;
                    if (needRatio < 0.0f) needRatio = 0.0f;
                    if (needRatio > (1.0f - usedRatio)) needRatio = 1.0f - usedRatio;

                    const float startA = -1.5707963f;
                    drawPieSlice(pieX, pieCenterY, pieR, 0.0f, 6.2831853f, pieBgCol);
                    if (usedRatio > 0.0f) {
                        drawPieSlice(pieX, pieCenterY, pieR, startA, startA + usedRatio * 6.2831853f, usedCol);
                    }
                    if (needRatio > 0.0f) {
                        float needStart = startA + usedRatio * 6.2831853f;
                        drawPieSlice(pieX, pieCenterY, pieR, needStart, needStart + needRatio * 6.2831853f, needCol);
                    }

                    // Subtle perimeter highlights/shadows for a simple 3D look.
                    if (pieR > 6.0f) {
                        const float ringOuter = pieR;
                        const float ringInner = pieR - 2.0f;
                        drawPieRingSegment(pieX, pieCenterY, ringOuter, ringInner, -2.4f, -0.8f, 0x55FFFFFF);
                        drawPieRingSegment(pieX, pieCenterY, ringOuter, ringInner, 1.2f, 2.6f, 0x33000000);
                        drawPieRingSegment(pieX, pieCenterY, pieR - 1.0f, pieR - 5.0f,
                                           -0.9f, -0.2f, 0x88FFFFFF);
                    }

                    float infoY = pieCenterY - infoLineH;
                    const std::string freeStr = (freeB > 0 || totalB > 0) ? humanSize3(freeB) : std::string("--");
                    const std::string usedStr = (totalB > 0) ? humanSize3(usedB) : std::string("--");
                    const bool showNeed = (needB > 0) || isSameMove;

                    std::string freeLine = std::string("Free Space: ") + freeStr;
                    drawTextStyled(infoX, infoY, freeLine.c_str(), infoScale, freeCol, 0, INTRAFONT_ALIGN_LEFT, false);
                    infoY += infoLineH;
                    if (showNeed) {
                        std::string needLine = isSameMove
                            ? std::string("Space Needed: N/A")
                            : std::string("Space Needed: ") + humanSize3(needB);
                        drawTextStyled(infoX, infoY, needLine.c_str(), infoScale, needCol, 0, INTRAFONT_ALIGN_LEFT, false);
                        infoY += infoLineH;
                    }
                    std::string usedLine = std::string("Used Space: ") + usedStr;
                    drawTextStyled(infoX, infoY, usedLine.c_str(), infoScale, usedCol, 0, INTRAFONT_ALIGN_LEFT, false);
                }
            }
            return;
        }
        const float keyH = 22.0f;
        drawRect((int)ctrlX, (int)(ctrlY - 1.0f), (int)ctrlW, (int)(keyH + 1.0f), COLOR_BANNER);
        const unsigned keyTextCol = 0xFFBBBBBB;
        float keyIconH = 15.0f;
        float keyIconW = 0.0f;
        if (okIconTexture && okIconTexture->data && okIconTexture->height > 0) {
            keyIconW = (float)okIconTexture->width * (keyIconH / (float)okIconTexture->height);
        }
        const float keyLabelScale = 0.7f;
        const float keyBaseX = ctrlX + 5.0f;
        const float keyIconX = keyBaseX + 10.0f;
        const float keyGap = (keyIconW > 0.0f) ? 10.0f : 0.0f;
        const float keyLabelPad = 6.0f;
        const float keyY = ctrlY + 13.0f;
        if (okIconTexture && okIconTexture->data && okIconTexture->height > 0) {
            drawTextureScaled(okIconTexture, keyIconX, keyY - 10.0f, keyIconH, 0xFFFFFFFF);
        }
        float keyTextX = (keyIconW > 0.0f)
            ? (keyIconX + keyIconW + keyGap + keyLabelPad)
            : (keyBaseX + keyLabelPad);
        float keyTextBaseline = keyY + 2.0f;
        drawTextStyled(keyTextX, keyTextBaseline, "Select", keyLabelScale, keyTextCol, 0, INTRAFONT_ALIGN_LEFT, false);

        const float creditGap = 6.0f;
        const float creditY = ctrlY + keyH + creditGap - 1.0f;
        const float creditH = 39.0f;
        drawRect((int)ctrlX, (int)(creditY - 1.0f), (int)ctrlW, (int)(creditH + 1.0f), COLOR_BANNER);
        const float creditTopLineY = creditY + 14.0f;
        const float creditBottomY = creditY + creditH - 6.0f;
        std::string credit = gHomeAnimEntries.empty() ? "No animations" : currentHomeAnimCredit();
        drawTextStyled(ctrlX + ctrlW * 0.5f, creditTopLineY, "Animation by:", 0.7f, keyTextCol, 0, INTRAFONT_ALIGN_CENTER, false);
        drawTextStyled(ctrlX + ctrlW * 0.5f, creditBottomY, credit.c_str(), 0.7f, COLOR_WHITE, 0, INTRAFONT_ALIGN_CENTER, false);

        const float switchGap = 5.0f;
        const float switchY = creditY + creditH + switchGap;
        const float switchH = 42.0f;
        drawRect((int)ctrlX, (int)(switchY - 1.0f), (int)ctrlW, (int)(switchH + 1.0f), COLOR_BANNER);

        const float switchPadY = switchY + 3.0f;
        const float switchIconH = 14.0f;
        const float switchMidX = ctrlX + ctrlW * 0.5f;
        if (lIconTexture && lIconTexture->data) {
            drawTextureScaled(lIconTexture, ctrlX + 5.0f, switchPadY, switchIconH, 0xFFFFFFFF);
        }
        if (rIconTexture && rIconTexture->data) {
            drawTextureScaled(rIconTexture, ctrlX + ctrlW - switchIconH - 5.0f, switchPadY, switchIconH, 0xFFFFFFFF);
        }
        drawTextStyled(switchMidX, switchPadY + 12.0f, "Change Animation", 0.7f, keyTextCol, 0, INTRAFONT_ALIGN_CENTER, false);
        char animBuf[32];
        int animCount = (int)gHomeAnimEntries.size();
        int animIdxOne = (gHomeAnimIndex >= 0 && gHomeAnimIndex < animCount) ? (gHomeAnimIndex + 1) : 0;
        if (animCount > 0) snprintf(animBuf, sizeof(animBuf), "%d/%d", animIdxOne, animCount);
        else snprintf(animBuf, sizeof(animBuf), "0/0");
        drawTextStyled(switchMidX, switchPadY + 32.0f, animBuf, 0.7f, COLOR_WHITE, 0, INTRAFONT_ALIGN_CENTER, false);

        const float animGap = 8.0f;
        const float animH = 108.0f;
        float animY = switchY + switchH + animGap - 3.0f;
        const float footerMargin = 18.0f + 4.0f;
        float maxAnimY = SCREEN_HEIGHT - footerMargin - animH;
        if (animY > maxAnimY) animY = maxAnimY;
        const float animX = ctrlX;
        const float animW = ctrlW;
        drawRect((int)animX, (int)(animY - 1.0f), (int)animW, (int)(animH + 1.0f), COLOR_BANNER);

        advanceHomeAnimationFrame();
        Texture* animTex = getCurrentHomeAnimTexture();
        if (animTex && animTex->data && animTex->width > 0 && animTex->height > 0) {
            const float pad = 8.0f;
            const float boxW = animW - pad * 2.0f;
            const float boxH = animH - pad * 2.0f;
            float sx = boxW / (float)animTex->width;
            float sy = boxH / (float)animTex->height;
            float s = (sx < sy) ? sx : sy;
            float drawH = (float)animTex->height * s;
            float drawW = (float)animTex->width * s;
            float drawX = animX + (animW - drawW) * 0.5f;
            float drawY = animY + (animH - drawH) * 0.5f;
            drawTextureScaledTight(animTex, drawX, drawY, drawH, 0xFFFFFFFF);
        } else {
            const char* noAnim = gHomeAnimEntries.empty() ? "No animations" : "Loading...";
            drawTextStyled(animX + animW * 0.5f, animY + animH * 0.5f, noAnim,
                           0.7f, COLOR_GRAY, 0, INTRAFONT_ALIGN_CENTER, false);
        }

        const int rowCount = (int)entries.size();
        const float top = panelY + 4.0f;
        const float bottom = panelY + panelH - 4.0f;
        const float rowH = (rowCount > 0) ? ((bottom - top) / (float)rowCount) : 0.0f;
        const float textCenterX = panelX + (panelW * 0.5f);
        const float iconGap = 10.0f;
        const float iconYOffsetBase = -6.0f;

        float internalCenterY = 0.0f;
        float usbCenterY = 0.0f;
        bool haveInternal = false;
        bool haveUsb = false;

        for (int i = 0; i < rowCount; ++i) {
            const char* name = entries[i].d_name;

            const char* lines[2] = { nullptr, nullptr };
            int lineCount = 1;
            float scale = 0.75f;
            Texture* icon = nullptr;
            float iconH = 0.0f;

            if (!strcmp(name, "ms0:/")) {
                lines[0] = "Memory"; lines[1] = "Stick"; lineCount = 2;
                scale = 0.95f; icon = rootMemIcon; iconH = 40.0f;
            } else if (!strcmp(name, "ef0:/")) {
                lines[0] = "Internal"; lines[1] = "Storage"; lineCount = 2;
                scale = 0.95f; icon = rootInternalIcon; iconH = 40.0f;
            } else if (!strcmp(name, "__USB_MODE__")) {
                lines[0] = "USB Mode";
                scale = 0.7f; icon = rootUsbIcon; iconH = 21.0f;
            } else if (!strcmp(name, "__GCL_TOGGLE__")) {
                lines[0] = "Game Categories:";
                scale = 0.7f; icon = rootCategoriesIcon; iconH = 25.0f;
            } else {
                lines[0] = rootDisplayName(name);
                scale = 0.6f; icon = nullptr; iconH = 0.0f;
            }

            const bool sel = (i == selectedIndex);
            const bool disabled = (i < (int)rowFlags.size() && (rowFlags[i] & ROW_DISABLED));
            const unsigned baseCol = disabled ? COLOR_GRAY : COLOR_WHITE;
            const unsigned textCol = sel ? COLOR_BLACK : baseCol;
            const unsigned shadowCol = sel ? COLOR_WHITE : 0x40000000;
            const unsigned iconCol = disabled ? 0x66FFFFFF : 0xFFFFFFFF;

            float rowYOffset = 0.0f;
            if (!strcmp(name, "ms0:/")) rowYOffset = 4.0f;
            else if (!strcmp(name, "ef0:/")) rowYOffset = 5.0f;
            else if (!strcmp(name, "__USB_MODE__")) rowYOffset = 7.0f;
            else if (!strcmp(name, "__GCL_TOGGLE__")) rowYOffset = -8.0f;
            float textYOffset = (!strcmp(name, "__GCL_TOGGLE__")) ? -2.0f : 0.0f;
            float iconYOffset = iconYOffsetBase;
            if (!strcmp(name, "ms0:/") || !strcmp(name, "ef0:/") || !strcmp(name, "__USB_MODE__")) {
                iconYOffset += 5.0f;
            }
            const float centerY = top + rowH * (i + 0.5f) + rowYOffset;
            const float lineH = 16.0f * scale;
            const float blockH = lineH * (float)lineCount;
            const float firstBaseline = centerY - (blockH * 0.5f) + (lineH * 0.75f);

            if (!strcmp(name, "ef0:/")) { internalCenterY = centerY; haveInternal = true; }
            if (!strcmp(name, "__USB_MODE__")) { usbCenterY = centerY; haveUsb = true; }

            float maxW = 0.0f;
            for (int l = 0; l < lineCount; ++l) {
                float w = measureTextWidth(scale, lines[l]);
                if (w > maxW) maxW = w;
            }
            const float textLeftX = textCenterX - (maxW * 0.5f);

            if (icon && iconH > 0.0f && icon->height > 0) {
                float iconW = (float)icon->width * (iconH / (float)icon->height);
                float iconX = textLeftX - iconGap - iconW;
                if (!strcmp(name, "ms0:/")) iconX -= 6.0f;
                float iconY = centerY - (iconH * 0.5f) + iconYOffset;
                drawTextureScaled(icon, iconX, iconY, iconH, iconCol);
            }

            const float lineGap = (!strcmp(name, "ms0:/") || !strcmp(name, "ef0:/")) ? 2.0f : 0.0f;
            for (int l = 0; l < lineCount; ++l) {
                drawTextStyled(textCenterX, firstBaseline + (lineH * l) + (lineGap * l) + textYOffset, lines[l],
                               scale, textCol, shadowCol, INTRAFONT_ALIGN_CENTER, true);
            }

            if (!strcmp(name, "__GCL_TOGGLE__")) {
                Texture* stateIcon = gclArkOn ? rootArk4Icon :
                                     (gclProOn ? rootProMeIcon : rootOffBulbIcon);
                const bool isOff = (!gclArkOn && !gclProOn);
                const float stateH = 18.0f;
                const float gap = 3.0f;
                const float stateYOffset = -2.0f;
                const float stateCenterY = firstBaseline + (lineH * lineCount) + gap + (stateH * 0.5f) + stateYOffset;

                float stateIconW = 0.0f;
                if (stateIcon && stateIcon->height > 0) {
                    stateIconW = (float)stateIcon->width * (stateH / (float)stateIcon->height);
                }

                if (isOff) {
                    const char* stateText = "Turned off";
                    const float stateScale = 0.6f;
                    const float stateLineH = 16.0f * stateScale;
                    const float stateBaseline = stateCenterY - (stateLineH * 0.5f) + (stateLineH * 0.75f) - 5.0f;
                    const unsigned stateCol = disabled ? COLOR_GRAY : COLOR_WHITE;

                    const float stateTextW = measureTextWidth(stateScale, stateText);
                    const float groupW = stateIconW + iconGap + stateTextW;
                    const float groupLeftX = textCenterX - (groupW * 0.5f);

                    if (stateIcon && stateH > 0.0f) {
                        drawTextureScaled(stateIcon, groupLeftX,
                                          stateCenterY - (stateH * 0.5f) + iconYOffset,
                                          stateH, iconCol);
                    }
                    drawTextStyled(groupLeftX + stateIconW + iconGap - 2.0f, stateBaseline + 1.0f, stateText,
                                   stateScale, stateCol, 0, INTRAFONT_ALIGN_LEFT, false);
                } else if (stateIcon && stateH > 0.0f) {
                    drawTextureScaled(stateIcon, textCenterX - (stateIconW * 0.5f),
                                      stateCenterY - (stateH * 0.5f) + iconYOffset,
                                      stateH, iconCol);
                }
            }
        }

        if (haveInternal && haveUsb) {
            const int lineY = (int)((internalCenterY + usbCenterY) * 0.5f);
            const int lineX = (int)(panelX + 24.0f);
            const int lineW = (int)(panelW - 24.0f);
            drawHFadeLine(lineX, lineY + 4, lineW - 30, 1, 0xA0, 20, 0x00C0C0C0);
        }
    }

    void drawCategoryMenu() {
        if (entries.empty()) return;

        const float panelX = 5.0f;
        const float panelY = 22.0f;
        const float panelW = 280.0f;
        const float panelH = 226.0f;
        drawRect((int)panelX, (int)(panelY - 1.0f), (int)panelW, (int)(panelH + 1.0f), COLOR_BANNER);

        const float ctrlX = 290.0f;
        const float ctrlY = 22.0f;
        const float ctrlW = 185.0f;
        const bool opCategoryMode =
            (actionMode != AM_None && (opPhase == OP_SelectCategory || opPhase == OP_Confirm));
        const float ctrlHFull = 94.0f;
        const float ctrlH = opCategoryMode ? 39.0f : (catSortMode ? (ctrlHFull - 17.0f - 19.0f) : (ctrlHFull - 19.0f));
        drawRect((int)ctrlX, (int)(ctrlY - 1.0f), (int)ctrlW, (int)(ctrlH + 1.0f), COLOR_BANNER);

        if (!opCategoryMode) {
            // Mode switcher block (L/R)
            const float modeY = ctrlY + ctrlHFull + 5.0f;
            const float modeH = 42.0f;
            drawRect((int)ctrlX, (int)(modeY - 1.0f), (int)ctrlW, (int)(modeH + 1.0f), COLOR_BANNER);

            // Mode switcher labels (L top-left, R top-right)
            const float modePadY = modeY + 3.0f;
            const float modeScale = 0.7f;
            const float modeIconH = 14.0f;
            unsigned stdCol  = catSortMode ? COLOR_GRAY : COLOR_WHITE;
            unsigned sortCol = catSortMode ? COLOR_WHITE : COLOR_GRAY;
            unsigned lIconCol = catSortMode ? 0xFFFFFFFF : 0x66FFFFFF; // dim L in Standard
            unsigned rIconCol = catSortMode ? 0x66FFFFFF : 0xFFFFFFFF; // dim R in Sort
            if (!gclCfg.catsort) {
                lIconCol = 0x66FFFFFF;
                rIconCol = 0x66FFFFFF;
            }
            const float lX = ctrlX + 5.0f;
            if (lIconTexture && lIconTexture->data) {
                drawTextureScaled(lIconTexture, lX, modePadY, modeIconH, lIconCol);
            }
            const float rX = ctrlX + ctrlW - modeIconH - 5.0f;
            if (rIconTexture && rIconTexture->data) {
                drawTextureScaled(rIconTexture, rX, modePadY, modeIconH, rIconCol);
            }
            drawTextStyled(ctrlX + ctrlW * 0.5f, modePadY + 12.0f, "Change Mode", modeScale, 0xFFBBBBBB, 0, INTRAFONT_ALIGN_CENTER, false);
            drawTextStyled(ctrlX + ctrlW * 0.5f, modePadY + 32.0f, catSortMode ? "Sort" : "Standard",
                           modeScale, catSortMode ? sortCol : stdCol, 0, INTRAFONT_ALIGN_CENTER, false);
        }

        const unsigned keyTextCol = 0xFFBBBBBB;
        const unsigned saveTextCol = 0xFF17D0FD;
        const unsigned pickedGlowCol = 0xFF8CE8FE;

        // Controls box
        auto drawKeyRowLeft = [&](float baseX, float& y, Texture* icon, const char* label,
                                  bool bumpRight, unsigned textCol, float rowStep){
            float iconH = 15.0f;
            if (!opCategoryMode && (icon == startIconTexture || icon == selectIconTexture)) iconH = 18.0f;
            float iconW = 0.0f;
            if (icon && icon->data && icon->height > 0) {
                iconW = (float)icon->width * (iconH / (float)icon->height);
            }
            const float gap = (iconW > 0.0f) ? (6.0f + (bumpRight ? 4.0f : 0.0f)) : 0.0f;
            float x = baseX + (bumpRight ? 6.0f : 0.0f);
            if (bumpRight) x += 4.0f;
            if (icon && icon->data && icon->height > 0) {
                drawTextureScaled(icon, x, y - 10.0f, iconH, 0xFFFFFFFF);
                x += iconW + gap;
            }
            const float labelPad = bumpRight ? 6.0f : 0.0f;
            drawTextStyled(x + labelPad, y + 2.0f, label, 0.7f, textCol, 0, INTRAFONT_ALIGN_LEFT, false);
            y += rowStep;
        };

        float keyY = ctrlY + 13.0f;
        const float keyX = ctrlX + 5.0f;
        if (opCategoryMode) {
            drawKeyRowLeft(keyX, keyY, okIconTexture, "Select", true, keyTextCol, 17.0f);
            drawKeyRowLeft(keyX, keyY, circleIconTexture, "Back", true, keyTextCol, 17.0f);
        } else if (!catSortMode) {
            drawKeyRowLeft(keyX, keyY, okIconTexture, "Select", true, keyTextCol, 18.0f);
            drawKeyRowLeft(keyX, keyY, selectIconTexture, "Rename", false, keyTextCol, 18.0f);
            drawKeyRowLeft(keyX, keyY, triangleIconTexture, "Category Ops.", true, keyTextCol, 18.0f);
            drawKeyRowLeft(keyX, keyY, circleIconTexture, "Back", true, keyTextCol, 18.0f);
        } else {
            drawKeyRowLeft(keyX, keyY, okIconTexture, "Pick Up/Drop", true, keyTextCol, 18.0f);
            drawKeyRowLeft(keyX, keyY, startIconTexture, "Save List", false, saveTextCol, 18.0f);
            drawKeyRowLeft(keyX, keyY, circleIconTexture, "Back", true, keyTextCol, 18.0f);
        }

        // Categories list
        const int rowCount = (int)entries.size();
        const float top = panelY + 4.0f + CAT_LIST_OFFSET_Y;
        const float rowH = CAT_ROW_H;
        const int visibleRows = categoryVisibleRows();
        int maxScroll = rowCount - visibleRows;
        if (maxScroll < 0) maxScroll = 0;
        if (scrollOffset > maxScroll) scrollOffset = maxScroll;
        if (scrollOffset < 0) scrollOffset = 0;
        const float iconGap = 10.0f;
        const float scrollTrackX = panelX + panelW - 6.0f;
        const float countRightX = scrollTrackX - 9.0f;
        const float countColLeftX = scrollTrackX - 50.0f;
        const float countCenterX = (countColLeftX + countRightX) * 0.5f;

        const bool hasSettingsRow = (rowCount > 0 && !strcasecmp(entries[0].d_name, kCatSettingsLabel));
        const bool inOpCategorySelect =
            (actionMode != AM_None && (opPhase == OP_SelectCategory || opPhase == OP_Confirm));
        const bool opHeader = (!hasSettingsRow && inOpCategorySelect);
        const bool headerShown = (hasSettingsRow || opHeader);
        const float listTop = headerShown ? (top + rowH + CAT_SETTINGS_GAP) : top;
        const int listCount = hasSettingsRow ? (rowCount - 1) : rowCount;
        const int listScrollOffset = hasSettingsRow ? std::max(0, scrollOffset - 1) : scrollOffset;
        const int listMaxScroll = (listCount > visibleRows) ? (listCount - visibleRows) : 0;
        const int startRow = hasSettingsRow ? std::max(1, scrollOffset) : scrollOffset;
        const int endRow = std::min(rowCount, startRow + visibleRows);
        const unsigned long long nowUs = (unsigned long long)sceKernelGetSystemTimeWide();
        if (catScrollIndex != selectedIndex) {
            catScrollIndex = selectedIndex;
            catScrollStartUs = nowUs;
        }
        auto clipTextToWidth = [&](const std::string& s, float maxW, float offsetPx, float scale)->std::string {
            if (s.empty() || maxW <= 0.0f) return std::string();
            float skip = (offsetPx > 0.0f) ? offsetPx : 0.0f;
            float used = 0.0f;
            std::string out;
            out.reserve(s.size());
            size_t ci = 0;
            while (ci < s.size()) {
                unsigned char lead = (unsigned char)s[ci];
                size_t charLen = (lead < 0x80) ? 1 : ((lead & 0xE0) == 0xC0) ? 2 : ((lead & 0xF0) == 0xE0) ? 3 : ((lead & 0xF8) == 0xF0) ? 4 : 1;
                if (ci + charLen > s.size()) break;
                std::string ch = s.substr(ci, charLen);
                float cw = measureTextWidth(scale, ch.c_str());
                if (skip > 0.0f) {
                    if (skip >= cw) { skip -= cw; ci += charLen; continue; }
                    skip = 0.0f;
                    ci += charLen;
                    continue;
                }
                if (used + cw > maxW) break;
                out += ch;
                used += cw;
                ci += charLen;
            }
            return out;
        };

        auto drawRow = [&](int i, float centerY, bool showCount, bool showAppsLabel) {
            const char* name = entries[i].d_name;
            const bool sel = (i == selectedIndex);
            const bool isUncategorizedRow = (strcasecmp(name, "Uncategorized") == 0);
            const bool isUncategorizedDisabled =
                (isUncategorizedRow && !inOpCategorySelect &&
                 !isUncategorizedEnabledForDevice(currentDevice));
            const bool disabled = (inOpCategorySelect &&
                                   opDisabledCategories.find(name) != opDisabledCategories.end())
                                   || isUncategorizedDisabled;

            const bool locked = isCategoryRowLocked(i);
            const bool isPicked = (catSortMode && catPickActive && i == catPickIndex && !locked);

            int appCount = -1;
            if (showCount || (!inOpCategorySelect && !isUncategorizedRow)) {
                if (isUncategorizedRow) {
                    appCount = (int)uncategorized.size();
                } else {
                    auto it = categories.find(name);
                    if (it != categories.end()) appCount = (int)it->second.size();
                    else {
                        for (const auto &kv : categories) {
                            if (!strcasecmp(kv.first.c_str(), name)) { appCount = (int)kv.second.size(); break; }
                        }
                    }
                }
            }

            const bool emptyFade = (!inOpCategorySelect && !isUncategorizedRow && appCount == 0);
            const bool dimmed = (disabled || emptyFade);

            unsigned baseCol = dimmed ? COLOR_GRAY : COLOR_WHITE;
            unsigned textCol = sel ? COLOR_BLACK : baseCol;
            unsigned shadowCol = sel ? COLOR_WHITE : 0x40000000;
            if (isPicked) shadowCol = pickedGlowCol;

            const float scale = 0.5f;
            const float appsScale = 0.6f;
            const float lineH = 16.0f * scale;
            const float baseline = centerY + (lineH * 0.25f) - 2.0f;

            const float textLeftX = panelX + 32.0f; // left aligned
            const float textRightX = countColLeftX; // leave room for count column
            const float textAvailW = textRightX - textLeftX;

            // Icon
            const float iconHCat = 15.0f;
            const bool isSettingsRow = (strcasecmp(name, kCatSettingsLabel) == 0);
            if (isSettingsRow) {
                if (catSettingsIcon && catSettingsIcon->data && catSettingsIcon->height > 0) {
                    float iconW = (float)catSettingsIcon->width * (iconHCat / (float)catSettingsIcon->height);
                    float iconX = textLeftX - iconGap - iconW + 3.0f;
                    float iconY = centerY - (iconHCat * 0.5f) - 4.0f;
                    drawTextureScaled(catSettingsIcon, iconX, iconY, iconHCat, 0xFFFFFFFF);
                }
            } else {
            const bool gclOn = (gclArkOn || gclProOn);
            const bool isUncRow = (strcasecmp(name, "Uncategorized") == 0);
            const bool isFiltered = gclOn && (!isUncRow && isFilteredBaseName(stripCategoryPrefixes(name)));
            Texture* folderIcon = (isFiltered && catFolderIconGray) ? catFolderIconGray : catFolderIcon;
                if (folderIcon && folderIcon->data && folderIcon->height > 0) {
                    float iconW = (float)folderIcon->width * (iconHCat / (float)folderIcon->height);
                    float iconX = textLeftX - iconGap - iconW + 5.0f;
                    float iconY = centerY - (iconHCat * 0.5f) - 4.0f;
                    unsigned iconCol = dimmed ? 0x66FFFFFF : 0xFFFFFFFF;
                    if (isFiltered) iconCol = (iconCol & 0x00FFFFFF) | 0xB3000000; // 70% opacity
                    drawTextureScaled(folderIcon, iconX, iconY, iconHCat, iconCol);
                    // Overlay C
                    const float cX = (float)(int)(iconX + (iconW * 0.25f) - 4.0f + 0.5f);
                    const float cY = (float)(int)(iconY + 13.0f + 0.5f);
                    drawTextStyled(cX, cY, "C",
                                   0.5f, dimmed ? COLOR_GRAY : COLOR_BLACK, 0, INTRAFONT_ALIGN_LEFT, false);
                }
            }

            float textOffsetX = 0.0f;
            float textOverflow = 0.0f;
            if (textAvailW > 0.0f) {
                const float textW = measureTextWidth(scale, name);
                textOverflow = textW - textAvailW;
            }
            if (sel && textOverflow > 2.0f) {
                const float speed = 120.0f; // px/sec
                const double elapsed = (double)(nowUs - catScrollStartUs) / 1000000.0;
                textOffsetX = (float)(elapsed * speed);
                if (textOffsetX > textOverflow) textOffsetX = textOverflow;
            }

            std::string drawLabel = name;
            if (textAvailW > 0.0f) {
                const float clipOffset = (textOffsetX > 0.0f) ? textOffsetX : 0.0f;
                drawLabel = clipTextToWidth(drawLabel, textAvailW, clipOffset, scale);
            }

            drawTextStyled(textLeftX, baseline, drawLabel.c_str(),
                           scale, textCol, shadowCol, INTRAFONT_ALIGN_LEFT, false);
            if (drawLabel.find('_') != std::string::npos) {
                const float underscoreW = measureTextWidth(scale, "_");
                const float underlineY = baseline + 2.0f;
                std::string prefix;
                prefix.reserve(drawLabel.size());
                for (size_t ci = 0; ci < drawLabel.size(); ++ci) {
                    if (drawLabel[ci] == '_') {
                        float prefixW = measureTextWidth(scale, prefix.c_str());
                        float ux = textLeftX + prefixW;
                        int lineW = (int)(underscoreW + 1.0f);
                        if (lineW < 1) lineW = 1;
                        drawRect((int)(ux), (int)(underlineY - 1.0f), lineW, 1, textCol);
                    }
                    prefix.push_back(drawLabel[ci]);
                }
            }

            if (showAppsLabel) {
                intraFontSetStyle(font, appsScale, COLOR_GRAY, 0, 0.0f, INTRAFONT_ALIGN_CENTER);
                intraFontPrint(font, countCenterX, baseline + 1.0f, "Apps");
            }

            if (showCount) {
                if (appCount < 0) appCount = 0;
                char cnt[16];
                snprintf(cnt, sizeof(cnt), "%d", appCount);
                intraFontSetStyle(font, scale, COLOR_GRAY, 0, 0.0f, INTRAFONT_ALIGN_CENTER);
                intraFontPrint(font, countCenterX + 1.0f, baseline + 1.0f, cnt);
            }
        };

        auto drawAppsHeader = [&](float centerY, const char* label) {
            const float appsScale = 0.6f;
            const float lineH = 16.0f * appsScale;
            const float baseline = centerY + (lineH * 0.25f) - 2.0f;
            if (label && label[0] != '\0') {
                const float textLeftX = panelX + 32.0f;
                drawTextStyled(textLeftX, baseline, label, appsScale, COLOR_WHITE, 0x40000000,
                               INTRAFONT_ALIGN_LEFT, false);
            }
            intraFontSetStyle(font, appsScale, COLOR_GRAY, 0, 0.0f, INTRAFONT_ALIGN_CENTER);
            intraFontPrint(font, countCenterX, baseline + 1.0f, "Apps");
        };

        if (hasSettingsRow) {
            const float centerY = top + rowH * 0.5f;
            drawRow(0, centerY, false, true);
            const int lineY = (int)(top + rowH + 0.0f);
            const int lineX = (int)(panelX + 13.0f);
            const int lineW = (int)(panelW - 70.0f);
            if (lineW > 0) {
                drawHFadeLine(lineX, lineY, lineW, 1, 0xA0, 20, 0x00C0C0C0);
            }
            const int solidW = 29;
            const int solidX = (int)(countCenterX - (solidW * 0.5f));
            drawRect(solidX, lineY, solidW, 1, COLOR_GRAY);
        } else if (opHeader) {
            const float centerY = top + rowH * 0.5f;
            drawAppsHeader(centerY, "Select Destination Category");
            const int lineY = (int)(top + rowH + 0.0f);
            const int lineX = (int)(panelX + 13.0f);
            const int lineW = (int)(panelW - 70.0f);
            if (lineW > 0) {
                drawHFadeLine(lineX, lineY, lineW, 1, 0xA0, 20, 0x00C0C0C0);
            }
            const int solidW = 29;
            const int solidX = (int)(countCenterX - (solidW * 0.5f));
            drawRect(solidX, lineY, solidW, 1, COLOR_GRAY);
        }

        for (int i = startRow; i < endRow; ++i) {
            const int rowIndex = i - startRow;
            const float centerY = listTop + rowH * (rowIndex + 0.5f);
            drawRow(i, centerY, true, false);
        }

        if (listCount > visibleRows) {
            const float trackX = scrollTrackX;
            const float trackY = listTop;
            const float trackH = rowH * visibleRows;
            drawRect((int)trackX, (int)trackY, 2, (int)trackH, 0x40000000);
            float thumbH = trackH * ((float)visibleRows / (float)listCount);
            if (thumbH < 6.0f) thumbH = 6.0f;
            const float t = (listMaxScroll > 0) ? ((float)listScrollOffset / (float)listMaxScroll) : 0.0f;
            const float thumbY = trackY + t * (trackH - thumbH);
            drawRect((int)trackX, (int)thumbY, 2, (int)thumbH, 0xFFBBBBBB);
        }
    }

    void drawGclSettingsMenu() {
        if (entries.empty()) return;

        const float panelX = 5.0f;
        const float panelY = 22.0f;
        const float panelW = 280.0f;
        const float panelH = 226.0f;
        drawRect((int)panelX, (int)(panelY - 1.0f), (int)panelW, (int)(panelH + 1.0f), COLOR_BANNER);

        const float ctrlX = 290.0f;
        const float ctrlY = 22.0f;
        const float ctrlW = 185.0f;
        const float ctrlH = 40.0f;
        drawRect((int)ctrlX, (int)(ctrlY - 1.0f), (int)ctrlW, (int)(ctrlH + 1.0f), COLOR_BANNER);

        const unsigned keyTextCol = 0xFFBBBBBB;
        auto drawKeyRowLeft = [&](float baseX, float& y, Texture* icon, const char* label,
                                  bool bumpRight, unsigned textCol){
            float iconH = 15.0f;
            if (icon == startIconTexture || icon == selectIconTexture) iconH = 18.0f;
            float iconW = 0.0f;
            if (icon && icon->data && icon->height > 0) {
                iconW = (float)icon->width * (iconH / (float)icon->height);
            }
            const float gap = (iconW > 0.0f) ? (6.0f + (bumpRight ? 4.0f : 0.0f)) : 0.0f;
            float x = baseX + (bumpRight ? 6.0f : 0.0f);
            if (bumpRight) x += 4.0f;
            if (icon && icon->data && icon->height > 0) {
                drawTextureScaled(icon, x, y - 10.0f, iconH, 0xFFFFFFFF);
                x += iconW + gap;
            }
            const float labelPad = bumpRight ? 6.0f : 0.0f;
            drawTextStyled(x + labelPad, y + 2.0f, label, 0.7f, textCol, 0, INTRAFONT_ALIGN_LEFT, false);
            y += 18.0f;
        };

        float keyY = ctrlY + 13.0f;
        const float keyX = ctrlX + 5.0f;
        drawKeyRowLeft(keyX, keyY, okIconTexture, "Select", true, keyTextCol);
        drawKeyRowLeft(keyX, keyY, circleIconTexture, "Back", true, keyTextCol);

        const int rowCount = (int)entries.size();
        const float top = panelY + 4.0f + CAT_LIST_OFFSET_Y;
        const float rowH = CAT_ROW_H + 3.0f;
        const int visibleRows = gclSettingsVisibleRows();
        int maxScroll = rowCount - visibleRows;
        if (maxScroll < 0) maxScroll = 0;
        if (scrollOffset > maxScroll) scrollOffset = maxScroll;
        if (scrollOffset < 0) scrollOffset = 0;
        const int startRow = scrollOffset;
        const int endRow = std::min(rowCount, startRow + visibleRows);
        const float iconGap = 10.0f;

        for (int i = startRow; i < endRow; ++i) {
            const char* name = entries[i].d_name;
            const bool sel = (i == selectedIndex);
            const bool disabled = (i < (int)rowFlags.size() && (rowFlags[i] & ROW_DISABLED));

            (void)disabled;
            unsigned shadowCol = sel ? COLOR_WHITE : 0x40000000;
            const unsigned labelCol = sel ? COLOR_BLACK : (disabled ? COLOR_GRAY : keyTextCol);

            const int rowIndex = i - startRow;
            const float centerY = top + rowH * (rowIndex + 0.5f);
            const float scale = 0.57f;
            const float lineH = 16.0f * scale;
            const float baseline = centerY + (lineH * 0.25f) - 2.0f;
            const float textLeftX = panelX + 32.0f;
            const float iconH = 15.0f;

            const bool isBlacklistRow = (!strncasecmp(name, "Folder Rename Blacklist:", 24));
            Texture* icon = catSettingsIcon;
            if (isBlacklistRow) {
                if (blacklistIcon) icon = blacklistIcon;
            }
            if (icon && icon->data && icon->height > 0) {
                float iconW = (float)icon->width * (iconH / (float)icon->height);
                float iconX = textLeftX - iconGap - iconW + 3.0f;
                if (isBlacklistRow) iconX -= 2.0f;
                float iconY = centerY - (iconH * 0.5f) - 4.0f;
                drawTextureScaled(icon, iconX, iconY, iconH, disabled ? 0x66FFFFFF : 0xFFFFFFFF);
            }

            const char* colon = strchr(name, ':');
            if (colon) {
                std::string left(name, (size_t)(colon - name + 1));
                std::string right(colon + 1);
                const unsigned valueCol = disabled ? COLOR_GRAY : COLOR_WHITE;
                drawTextStyled(textLeftX, baseline, left.c_str(),
                               scale, labelCol, shadowCol, INTRAFONT_ALIGN_LEFT, true);
                const float leftW = measureTextWidth(scale, left.c_str());
                drawTextStyled(textLeftX + leftW, baseline, right.c_str(),
                               scale, valueCol, 0, INTRAFONT_ALIGN_LEFT, false);
            } else {
                drawTextStyled(textLeftX, baseline, name, scale, labelCol, shadowCol, INTRAFONT_ALIGN_LEFT, false);
            }
        }

        if (rowCount > visibleRows) {
            const float trackX = panelX + panelW - 6.0f;
            const float trackY = top;
            const float trackH = rowH * visibleRows;
            drawRect((int)trackX, (int)trackY, 2, (int)trackH, 0x40000000);
            float thumbH = trackH * ((float)visibleRows / (float)rowCount);
            if (thumbH < 6.0f) thumbH = 6.0f;
            const float t = (maxScroll > 0) ? ((float)scrollOffset / (float)maxScroll) : 0.0f;
            const float thumbY = trackY + t * (trackH - thumbH);
            drawRect((int)trackX, (int)thumbY, 2, (int)thumbH, 0xFFBBBBBB);
        }
    }

    void drawGameListMenu() {
        const float panelX = 5.0f;
        const float panelY = 22.0f;
        const float panelW = 280.0f;
        const float panelH = 226.0f;
        drawRect((int)panelX, (int)(panelY - 1.0f), (int)panelW, (int)(panelH + 1.0f), COLOR_BANNER);

        const float ctrlX = 290.0f;
        const float ctrlY = 22.0f;
        const float ctrlW = 185.0f;
        const float ctrlH = 94.0f + 5.0f + 42.0f; // match Categories controls + L/R block height
        drawRect((int)ctrlX, (int)(ctrlY - 1.0f), (int)ctrlW, (int)(ctrlH + 1.0f), COLOR_BANNER);

        const unsigned keyTextCol = 0xFFBBBBBB;
        const unsigned saveTextCol = 0xFF17D0FD;
        const unsigned pickedGlowCol = 0xFF8CE8FE;
        auto drawKeyRowLeft = [&](float baseX, float& y, Texture* icon, const char* label,
                                  bool bumpRight, unsigned textCol){
            float iconH = 15.0f;
            if (icon == startIconTexture || icon == selectIconTexture) iconH = 18.0f;
            float iconW = 0.0f;
            if (icon && icon->data && icon->height > 0) {
                iconW = (float)icon->width * (iconH / (float)icon->height);
            }
            const float gap = (iconW > 0.0f) ? (6.0f + (bumpRight ? 4.0f : 0.0f)) : 0.0f;
            float x = baseX + (bumpRight ? 6.0f : 0.0f);
            if (bumpRight) x += 4.0f;
            if (icon && icon->data && icon->height > 0) {
                drawTextureScaled(icon, x, y - 10.0f, iconH, 0xFFFFFFFF);
                x += iconW + gap;
            }
            const float labelPad = bumpRight ? 6.0f : 0.0f;
            drawTextStyled(x + labelPad, y + 2.0f, label, 0.7f, textCol, 0, INTRAFONT_ALIGN_LEFT, false);
            y += 17.0f;
        };
        auto drawKeyRowSquarePlusUpDown = [&](float baseX, float& y, const char* label, unsigned textCol){
            float x = baseX; // align with start/select
            const float iconH = 15.0f;
            const float iconGap = 6.0f;
            const float plusGap = 3.0f;
            auto drawIcon = [&](Texture* icon){
                if (!icon || !icon->data || icon->height <= 0) return 0.0f;
                const float w = (float)icon->width * (iconH / (float)icon->height);
                drawTextureScaled(icon, x, y - 10.0f, iconH, 0xFFFFFFFF);
                x += w;
                return w;
            };
            if (drawIcon(squareIconTexture) > 0.0f) x += plusGap;
            const float plusScale = 0.6f;
            drawTextStyled(x, y + 1.0f, "+", plusScale, 0xFFFFFFFF, 0, INTRAFONT_ALIGN_LEFT, false);
            x += measureTextWidth(plusScale, "+") + plusGap;
            if (drawIcon(updownIconTexture) > 0.0f) x += iconGap;
            drawTextStyled(x, y + 2.0f, label, 0.7f, textCol, 0, INTRAFONT_ALIGN_LEFT, false);
            y += 17.0f;
        };

        float keyY = ctrlY + 13.0f;
        const float keyX = ctrlX + 5.0f;
        drawKeyRowLeft(keyX, keyY, okIconTexture, "Pick Up/Drop", true, keyTextCol);
        const char* toggleLabel = showTitles ? "Toggle Filenames" : "Toggle App Titles";
        drawKeyRowLeft(keyX, keyY, lIconTexture, toggleLabel, true, keyTextCol);
        drawKeyRowLeft(keyX, keyY, rIconTexture, "Alphabetize", true, keyTextCol);
        drawKeyRowLeft(keyX, keyY, startIconTexture, "Save List", false, saveTextCol);
        drawKeyRowLeft(keyX, keyY, selectIconTexture, "Rename", false, keyTextCol);
        drawKeyRowSquarePlusUpDown(keyX, keyY, "Mark for App Ops.", keyTextCol);
        drawKeyRowLeft(keyX, keyY, triangleIconTexture, "App Operations", true, keyTextCol);
        drawKeyRowLeft(keyX, keyY, circleIconTexture, "Back", true, keyTextCol);

        if (entries.empty()) return;

        const float listTop = panelY + 4.0f + GAME_LIST_OFFSET_Y;
        const float rowH = GAME_ROW_H;
        const int visibleRows = contentVisibleRows();
        int maxScroll = (int)entries.size() - visibleRows;
        if (maxScroll < 0) maxScroll = 0;
        if (scrollOffset > maxScroll) scrollOffset = maxScroll;
        if (scrollOffset < 0) scrollOffset = 0;

        const int startRow = scrollOffset;
        const int endRow = std::min((int)entries.size(), startRow + visibleRows);
        intraFontActivate(font);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,0,0);
        const float debugRightX = panelX + panelW - 12.0f;
        const float scale = 0.5f;
        const float lineH = 16.0f * scale;
        const float labelIconH = 15.0f;
        const float listShiftX = -15.0f;
        const float scrollTrackX = panelX + panelW - 6.0f;
        const float sizeRightX = scrollTrackX - 9.0f;
        const float iconSlotW = labelIconH;
        const float checkGap = 4.0f;
        const float checkX = panelX + 18.0f + listShiftX;
        const float iconSlotX = checkX + CHECKBOX_PX + checkGap;
        const float textLeftX = iconSlotX + iconSlotW + 4.0f;
        const float textRightX = scrollTrackX - 43.0f;
        const float textAvailW = textRightX - textLeftX;

        const unsigned long long nowUs = (unsigned long long)sceKernelGetSystemTimeWide();
        if (gameScrollIndex != selectedIndex) {
            gameScrollIndex = selectedIndex;
            gameScrollStartUs = nowUs;
        }

        auto clipTextToWidth = [&](const std::string& s, float maxW, float offsetPx)->std::string {
            if (s.empty() || maxW <= 0.0f) return std::string();
            float skip = (offsetPx > 0.0f) ? offsetPx : 0.0f;
            float used = 0.0f;
            std::string out;
            out.reserve(s.size());
            size_t ci = 0;
            while (ci < s.size()) {
                unsigned char lead = (unsigned char)s[ci];
                size_t charLen = (lead < 0x80) ? 1 : ((lead & 0xE0) == 0xC0) ? 2 : ((lead & 0xF0) == 0xE0) ? 3 : ((lead & 0xF8) == 0xF0) ? 4 : 1;
                if (ci + charLen > s.size()) break;
                std::string ch = s.substr(ci, charLen);
                float cw = measureTextWidth(scale, ch.c_str());
                if (skip > 0.0f) {
                    if (skip >= cw) { skip -= cw; ci += charLen; continue; }
                    skip = 0.0f;
                    ci += charLen;
                    continue;
                }
                if (used + cw > maxW) break;
                out += ch;
                used += cw;
                ci += charLen;
            }
            return out;
        };

        for(int i = startRow; i < endRow; i++){
            bool sel  = (i == selectedIndex);
            bool isDir= FIO_S_ISDIR(entries[i].d_stat.st_mode);
            const bool picked = moving && sel;
            const GameItem* giPtr = (view == View_AllFlat || view == View_CategoryContents) &&
                                    i >= 0 && i < (int)workingList.size()
                                    ? &workingList[i] : nullptr;
            const bool gclOn = (gclArkOn || gclProOn);
            const bool isHidden = gclOn && giPtr ? isGameFilteredPath(giPtr->path) : false;

            unsigned baseCol = isDir ? COLOR_CYAN : COLOR_WHITE;
            unsigned textCol = sel ? COLOR_BLACK : baseCol;
            unsigned shadowCol = sel ? COLOR_WHITE : 0x40000000;
            if (picked) shadowCol = pickedGlowCol;

            uint32_t labelColor = baseCol;
            const char* labelText = "[FILE]";  // default

            if (isDir) {
                labelText = "[DIR]";
            } else {
                // Use CATEGORY to determine PS1 vs Homebrew (with per-frame cache)
                if ((view == View_AllFlat || view == View_CategoryContents) &&
                    !isDir && i >= 0 && i < (int)workingList.size()) {
                    const GameItem& gi = workingList[i];
                    if (gi.kind == GameItem::EBOOT_FOLDER) {
                        if (gi.isUpdateDlc) {
                            labelText = "[UPD]";
                        } else {
                        std::string ebootPath = gi.path + "/EBOOT.PBP";
                        std::string category;

                        // Check cache first
                        auto it = categoryCache.find(ebootPath);
                        if (it != categoryCache.end()) {
                            category = it->second;
                        } else {
                            // Read and cache
                            category = readEbootCategory(ebootPath);
                            categoryCache[ebootPath] = category;
                        }

                        if (category == "ME") {
                            labelText = "[PS1]";
                        } else if (category == "MG") {
                            labelText = "[HB]";
                        } else {
                            labelText = "[FILE]";  // fallback for unknown CATEGORY
                        }
                        }
                    } else {
                        labelText = "[FILE]";  // ISO files
                    }
                } else {
                    labelText = "[FILE]";
                }
            }

            const int rowIndex = i - startRow;
            const float rowTop = listTop + rowH * rowIndex;
            const float centerY = rowTop + (rowH * 0.5f);
            const float baseline = centerY + (lineH * 0.25f) - 2.0f;

            Texture* labelIcon = nullptr;
            if (!strcmp(labelText, "[PS1]")) labelIcon = isHidden ? ps1IconTextureGray : ps1IconTexture;
            else if (!strcmp(labelText, "[HB]")) labelIcon = isHidden ? homebrewIconTextureGray : homebrewIconTexture;
            else if (!strcmp(labelText, "[UPD]")) labelIcon = isHidden ? updateIconTextureGray : updateIconTexture;
            else if (!strcmp(labelText, "[FILE]")) labelIcon = isHidden ? isoIconTextureGray : isoIconTexture;
            unsigned iconCol = labelColor;
            if (isHidden) iconCol = (iconCol & 0x00FFFFFF) | 0xB3000000; // 70% opacity

            if (labelIcon && labelIcon->data && labelIcon->height > 0) {
                float iconW = (float)labelIcon->width * (labelIconH / (float)labelIcon->height);
                float iconX = iconSlotX + (iconSlotW - iconW) * 0.5f;
                float iconY = centerY - (labelIconH * 0.5f) - 4.0f;
                // Snap to whole pixels to reduce shimmering/jagged edges.
                iconX = (float)((int)(iconX + 0.5f));
                iconY = (float)((int)(iconY + 0.5f));
                drawTextureScaled(labelIcon, iconX, iconY, labelIconH, iconCol);
            } else if (strcmp(labelText, "[UPD]") != 0) {
                const float labelTextW = measureTextWidth(scale, labelText);
                const float labelTextX = iconSlotX + (iconSlotW - labelTextW) * 0.5f;
                drawText(labelTextX, baseline, labelText, labelColor);
            }

            // --- filesize column (content views only) ---
            if (view == View_AllFlat || view == View_CategoryContents) {
                if (!isDir && i >= 0 && i < (int)workingList.size()) {
                    const GameItem& gi = workingList[i];
                    const std::string sz = (gi.sizeBytes > 0) ? humanSize3(gi.sizeBytes) : std::string("");
                    intraFontSetStyle(font, 0.5f, COLOR_GRAY, 0, 0.0f, INTRAFONT_ALIGN_RIGHT);
                    intraFontPrint(font, sizeRightX, baseline + 1.0f, sz.c_str());
                }
            }

            // checkbox left of filename (content views only)
            if (view == View_AllFlat || view == View_CategoryContents) {
                if (!isDir && i >= 0 && i < (int)workingList.size()) {
                    const std::string& p = workingList[i].path;
                    bool isChecked = (checked.find(p) != checked.end());
                    const float checkboxY = rowTop + (rowH - ITEM_HEIGHT) * 0.5f + 3.0f;
                    drawCheckboxAt((int)checkX, (int)checkboxY, isChecked);
                }
            }

            std::string label = entries[i].d_name;
            std::string drawLabel = label;
            float textOffsetX = 0.0f;
            float textOverflow = 0.0f;
            if (textAvailW > 0.0f) {
                const float textW = measureTextWidth(scale, label.c_str());
                textOverflow = textW - textAvailW;
            }
            if (sel && textOverflow > 2.0f) {
                const float speed = 120.0f; // px/sec
                const double elapsed = (double)(nowUs - gameScrollStartUs) / 1000000.0;
                textOffsetX = (float)(elapsed * speed);
                if (textOffsetX > textOverflow) textOffsetX = textOverflow;
            }
            if (textAvailW > 0.0f) {
                const float clipOffset = (textOffsetX > 0.0f) ? textOffsetX : 0.0f;
                drawLabel = clipTextToWidth(label, textAvailW, clipOffset);
            }

            drawTextStyled(textLeftX, baseline, drawLabel.c_str(),
                           scale, textCol, shadowCol, INTRAFONT_ALIGN_LEFT, false);


            if ((view==View_AllFlat || view==View_CategoryContents) && showDebugTimes && !isDir) {
                if (i >= 0 && i < (int)workingList.size()) {
                    const GameItem& gi = workingList[i];
                    char right[64], buf[32];
                    fmtDT(gi.time, buf, sizeof(buf));
                    snprintf(right, sizeof(right), "%s [F]", buf);
                    intraFontSetStyle(font,0.5f,COLOR_GRAY,0,0.0f,INTRAFONT_ALIGN_RIGHT);
                    intraFontPrint(font, debugRightX, baseline - 0.5f, right);
                }
            }
        }

        if ((int)entries.size() > visibleRows) {
            const float trackX = panelX + panelW - 6.0f;
            const float trackY = listTop - 3.0f;
            const float trackH = rowH * visibleRows;
            drawRect((int)trackX, (int)trackY, 2, (int)trackH, 0x40000000);
            float thumbH = trackH * ((float)visibleRows / (float)entries.size());
            if (thumbH < 6.0f) thumbH = 6.0f;
            const float t = (maxScroll > 0) ? ((float)scrollOffset / (float)maxScroll) : 0.0f;
            const float thumbY = trackY + t * (trackH - thumbH);
            drawRect((int)trackX, (int)thumbY, 2, (int)thumbH, 0xFFBBBBBB);
        }
    }

    void drawFileList() {
        if (!showRoots && view == View_GclSettings) {
            drawGclSettingsMenu();
            return;
        }
        if (!showRoots && view == View_Categories) {
            drawCategoryMenu();
            return;
        }
        if (showRoots) {
            drawRootMenu();
            return;
        }
        if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
            drawGameListMenu();
            return;
        }
        int y = LIST_START_Y;
        int end = std::min((int)entries.size(), scrollOffset+MAX_DISPLAY);
        intraFontActivate(font);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,0,0);
        for(int i=scrollOffset; i<end; i++){
            bool sel  = (i==selectedIndex);
            bool isDir= FIO_S_ISDIR(entries[i].d_stat.st_mode);

            bool disabled = (showRoots && i < (int)rowFlags.size() && (rowFlags[i] & ROW_DISABLED));

            bool isMoveRow = (!isDir && !showRoots &&
                              (view==View_AllFlat || view==View_CategoryContents) &&
                              moving && sel);

            unsigned labelCol;
            if (showRoots) {
                labelCol = disabled ? COLOR_GRAY : COLOR_CYAN;
            } else {
                labelCol = isDir ? COLOR_CYAN : (isMoveRow ? COLOR_YELLOW : COLOR_WHITE);
            }
            if (!showRoots && view == View_GclSettings &&
                i < (int)rowFlags.size() && (rowFlags[i] & ROW_DISABLED)) {
                labelCol = COLOR_GRAY;
            }

            // NEW: when picking a destination category, gray out categories marked as disabled
            if (!showRoots && view==View_Categories && actionMode!=AM_None && opPhase==OP_SelectCategory) {
                bool disabledCat = (opDisabledCategories.find(entries[i].d_name) != opDisabledCategories.end());
                if (disabledCat) labelCol = COLOR_GRAY;
            }

            // When sorting categories: highlight the picked row as [MOVE] in yellow
            const bool isCatMoveRow =
                (!showRoots && view == View_Categories && catSortMode &&
                catPickActive && i == catPickIndex && !isCategoryRowLocked(i));

            uint32_t labelColor = labelCol;
            const char* labelText = "[FILE]";  // default

            if (isCatMoveRow) {
                labelText = "[MOVE]";
            } else if (isDir) {
                labelText = "[DIR]";
            } else if (isMoveRow) {
                labelText = "[MOVE]";
            } else {
                // Use CATEGORY to determine PS1 vs Homebrew (with per-frame cache)
                if (!showRoots && (view == View_AllFlat || view == View_CategoryContents) &&
                    !isDir && i >= 0 && i < (int)workingList.size()) {
                    const GameItem& gi = workingList[i];
                    if (gi.kind == GameItem::EBOOT_FOLDER) {
                        if (gi.isUpdateDlc) {
                            labelText = "[UPD]";
                        } else {
                        std::string ebootPath = gi.path + "/EBOOT.PBP";
                        std::string category;

                        // Check cache first
                        auto it = categoryCache.find(ebootPath);
                        if (it != categoryCache.end()) {
                            category = it->second;
                        } else {
                            // Read and cache
                            category = readEbootCategory(ebootPath);
                            categoryCache[ebootPath] = category;
                        }

                        if (category == "ME") {
                            labelText = "[PS1]";
                        } else if (category == "MG") {
                            labelText = "[HB]";
                        } else {
                            labelText = "[FILE]";  // fallback for unknown CATEGORY
                        }
                        }
                    } else {
                        labelText = "[FILE]";  // ISO files
                    }
                } else {
                    labelText = "[FILE]";
                }
            }

            if (isCatMoveRow) labelColor = COLOR_YELLOW;

            drawText(10, y+2, labelText, labelColor);

            // checkbox left of filename (content views only)
            // --- filesize column (content views only, to the LEFT of the checkbox) ---
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                if (!isDir && i >= 0 && i < (int)workingList.size()) {
                    const GameItem& gi = workingList[i];
                    const std::string sz = (gi.sizeBytes > 0) ? humanSize3(gi.sizeBytes) : std::string("");
                    intraFontSetStyle(font, 0.5f, COLOR_GRAY, 0, 0.0f, INTRAFONT_ALIGN_RIGHT);
                    intraFontPrint(font, (float)SIZE_FIELD_RIGHT_X, (float)(y + 2.5f), sz.c_str());
                }
            }

            // checkbox left of filename (content views only)
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                if (!isDir && i >= 0 && i < (int)workingList.size()) {
                    const std::string& p = workingList[i].path;
                    bool isChecked = (checked.find(p) != checked.end());
                    drawCheckboxAt(CHECKBOX_X, y, isChecked);
                }
            }


            if(sel) intraFontSetStyle(font, 0.5f, COLOR_BLACK, COLOR_WHITE, 0.0f, INTRAFONT_ALIGN_LEFT);
            else    intraFontSetStyle(font, 0.5f, labelCol, 0x40000000, 0.0f, INTRAFONT_ALIGN_LEFT);

            if (showRoots) intraFontPrint(font, (float)NAME_TEXT_X, y + 2.5f, rootDisplayName(entries[i].d_name));
            else           intraFontPrint(font, (float)NAME_TEXT_X, y + 2.5f, entries[i].d_name);


            if (showRoots && opPhase == OP_SelectDevice) {
                // numbers if we have them
                uint64_t freeB = (i < (int)rowFreeBytes.size()) ? rowFreeBytes[i] : 0;
                uint64_t needB = (i < (int)rowNeedBytes.size()) ? rowNeedBytes[i] : 0;

                // pick message
                std::string msg;
                RowDisableReason why = (i < (int)rowReason.size()) ? rowReason[i] : RD_NONE;

                if (disabled && why == RD_RUNNING_FROM_EF0) {
                    msg = "- Run this app from the Memory Stick to access";
                } else if (needB > 0) {
                    // cross-device move: we know the NEED
                    if (disabled && why == RD_NO_SPACE) {
                        if (freeB == 0)
                            msg = " - Not enough space (need " + humanBytes(needB) + ", cannot determine free space)";
                        else
                            msg = " - Not enough space (need " + humanBytes(needB) + ", free " + humanBytes(freeB) + ")";
                    } else {
                        if (freeB == 0)
                            msg = " - Need " + humanBytes(needB) + ", probing free…";
                        else
                            msg = " - Need " + humanBytes(needB) + ", free " + humanBytes(freeB);
                    }
                } else {
                    // same-device or we couldn't compute need — still show free if known
                    if (freeB > 0)
                        msg = " - Free " + humanBytes(freeB);
                    else if (disabled)
                        msg = "- Not selectable";
                    else
                        msg.clear();
                }

                if (!msg.empty()) {
                    intraFontSetStyle(font, 0.5f, COLOR_YELLOW, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
                    intraFontPrint(font, 170.0f, y + 2, msg.c_str());
                }
            }


            if (!showRoots && (view==View_AllFlat || view==View_CategoryContents) && showDebugTimes && !isDir) {
                if (i >= 0 && i < (int)workingList.size()) {
                    const GameItem& gi = workingList[i];
                    char right[64], buf[32];
                    fmtDT(gi.time, buf, sizeof(buf));
                    snprintf(right, sizeof(right), "%s [F]", buf);
                    intraFontSetStyle(font,0.5f,COLOR_GRAY,0,0.0f,INTRAFONT_ALIGN_RIGHT);
                    intraFontPrint(font, SCREEN_WIDTH-20.0f, y+2, right);
                }
            }
            y += ITEM_HEIGHT;
        }

        if ((int)entries.size()>MAX_DISPLAY) {
            int h  = MAX_DISPLAY*180/entries.size();
            int yy = LIST_START_Y + scrollOffset*180/entries.size();
            drawRect(SCREEN_WIDTH-10, yy, 5, h, COLOR_WHITE);
        }
    }


    void drawControls() {
        if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
            return; // custom UI blocks handle controls in content views
        }
        int y = SCREEN_HEIGHT-30;
        const char* mode = showTitles ? "Title" : "Name";
        if (actionMode != AM_None) {
            if (showRoots || view == View_Categories) return;
            const char* v = (actionMode == AM_Copy) ? "Copy" : "Move";
            if (showRoots) {
                drawText(10,y, (std::string(v)+": X = Choose Destination Device   O: Cancel").c_str(), COLOR_WHITE);
            } else if (view == View_Categories) {
                drawText(10,y, (std::string(v)+": X = Choose Category   O: Cancel").c_str(), COLOR_WHITE);
            } else {
                drawText(10,y, (std::string(v)+" mode active…   O: Cancel").c_str(), COLOR_WHITE);
            }
            return;
        }


        if (view == View_Categories) {
            return; // custom UI block handles controls in Categories view
        }

        if (showRoots) {
            return;
        } else if (view == View_Categories) {
            if (!catSortMode) {
                std::string s = "X: Open Category | L: Rename CAT_ | ";
                if (gclCfg.catsort) s += "SELECT: Sort Mode | ";
                    s += "O: Back to Devices | △: Label Title/Name";
                    drawText(10, y, s.c_str(), COLOR_WHITE);
                } else {
                    drawText(10, y, "SORT MODE — X: Pick/Drop | ↑/↓: Move | SELECT: Done | O: Cancel (exit sort mode)", COLOR_WHITE);
                }
            } else if (view == View_CategoryContents || view == View_AllFlat) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "%s | START: Save | L: Rename | O: Back | □: Debug | △: Label (%s) | SELECT: A→Z (%s) | Hold ↑/↓: Fast",
                moving ? "X: Drop | ↑/↓: Swap" : "X: Pick | ↑/↓: Swap",
                mode, mode);
            drawText(10,y, buf, COLOR_WHITE);
        }
    }

    void drawFooter() {
        const int bannerH = 18;
        const int bannerY = SCREEN_HEIGHT - bannerH;
        const float textY = (float)(bannerY + bannerH - 5);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        drawRect(0, bannerY, SCREEN_WIDTH, bannerH, COLOR_BANNER);
        drawTextAligned(SCREEN_WIDTH / 2.0f, textY,
                        "Original by Sakya, Valentin, & Suloku. Reimagined by wad11656. Thanks joel16.",
                        COLOR_WHITE, INTRAFONT_ALIGN_CENTER);
    }

    void drawMessage(const char* m,unsigned c) {
        int x=80,y=SCREEN_HEIGHT/2-20;
        drawRect(x-10,y-5,SCREEN_WIDTH-2*x+20,30,0xFF404040);
        drawRect(x-11,y-6,SCREEN_WIDTH-2*x+22,32,COLOR_WHITE);
        drawText(x,y,m,c);
    }

    void drawCheckboxAt(int x, int y, bool isChecked) {
        Texture* t = isChecked ? checkTexChecked : checkTexUnchecked;
        if (!t || !t->data) return;
        int w=t->width, h=t->height, tbw=t->stride;
        float sx = (float)CHECKBOX_PX / (float)w;
        float sy = (float)CHECKBOX_PX / (float)h;
        float s  = (sx < sy) ? sx : sy;
        int dw = (int)(w * s), dh = (int)(h * s);
        int xx = x;
        int yy = y + ((ITEM_HEIGHT - dh) / 2) + CHECKBOX_Y_NUDGE;

        sceKernelDcacheWritebackRange(t->data, tbw * h * 4);
        sceGuTexFlush();
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, tbw, tbw, t->data);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuEnable(GU_TEXTURE_2D);

        struct V { float u,v; unsigned color; float x,y,z; };
        V* vtx = (V*)sceGuGetMemory(2 * sizeof(V));
        vtx[0] = { 0.0f,            0.0f,            0xFFFFFFFF, (float)xx,        (float)yy,        0.0f };
        vtx[1] = { (float)w - 0.5f, (float)h - 0.5f, 0xFFFFFFFF, (float)(xx + dw), (float)(yy + dh), 0.0f };
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, vtx);
        sceGuDisable(GU_TEXTURE_2D);
    }

    void drawBackdropOnlyForOSK() {
        sceGuStart(GU_DIRECT, list);
        #if OSK_MINIMAL_BACKDROP
        sceGuClearColor(gOskBgColorABGR);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        #else
        if (backgroundTexture && backgroundTexture->data) {
            sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            sceGuTexImage(0, backgroundTexture->stride, backgroundTexture->stride, backgroundTexture->stride, backgroundTexture->data);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
            sceGuEnable(GU_TEXTURE_2D);
            struct { float u,v; uint32_t color; float x,y,z; } *vtx = (decltype(vtx))sceGuGetMemory(2 * sizeof(*vtx));
            vtx[0] = { 0.0f, 0.0f, 0xFFFFFFFF, 0.0f,             0.0f,              0.0f };
            vtx[1] = { (float)backgroundTexture->width, (float)backgroundTexture->height, 0xFFFFFFFF, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT, 0.0f };
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_COLOR_8888 | GU_TRANSFORM_2D, 2, nullptr, vtx);
            sceGuDisable(GU_TEXTURE_2D);
        } else {
            sceGuClearColor(COLOR_BG);
            sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        }
        #endif
        sceGuFinish();
        sceGuSync(0,0);
    }

    void restoreGuAfterUtility() {
        sceGuStart(GU_DIRECT, list);
        sceGuDrawBuffer(GU_PSM_8888, (void*)0x00000000, 512);
        sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void*)0x00088000, 512);
        sceGuDepthBuffer((void*)0x00110000, 512);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_TRUE);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuDisable(GU_STENCIL_TEST);
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_LIGHTING);
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
        sceGuOffset(2048 - (SCREEN_WIDTH/2), 2048 - (SCREEN_HEIGHT/2));
        sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
        sceGuFinish();
        sceGuSync(0,0);
        sceDisplaySetMode(0, SCREEN_WIDTH, SCREEN_HEIGHT);
        sceDisplaySetFrameBuf((void*)0x00088000, 512, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
        sceGuDisplay(GU_TRUE);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
