    // Root detection & scanning
    // -----------------------------------------------------------
    void detectRoots() {
        roots.clear();
        runningFromEf0 = (gExecPath && strncmp(gExecPath, "ef0:", 4) == 0);

        { SceUID d = kfeIoOpenDir("ms0:/"); if (d >= 0) { kfeIoCloseDir(d); roots.push_back("ms0:/"); } }
        if (isPspGo()) {
            SceUID d = kfeIoOpenDir("ef0:/"); if (d >= 0) { kfeIoCloseDir(d); roots.push_back("ef0:/"); }
        }

        if (runningFromEf0) {
            bool alreadyListed = false;
            for (auto &r : roots) if (r == "ms0:/") { alreadyListed = true; break; }
            if (!alreadyListed) roots.insert(roots.begin(), "ms0:/");
        }
        if (roots.empty()) roots.push_back("ms0:/");

    }


    void resetLists(){
        categories.clear(); uncategorized.clear(); flatAll.clear();
        categoryNames.clear(); hasCategories = false; workingList.clear();
        moving = false;
    }

    static bool getStat(const std::string& path, SceIoStat& out){
        memset(&out,0,sizeof(out));
        return sceIoGetstat(path.c_str(), &out) >= 0;
    }
    static bool getStatDirNoSlash(const std::string& dir, SceIoStat& out){
        std::string p = dir;
        if (!p.empty() && p[p.size()-1]=='/') p.erase(p.size()-1);
        memset(&out,0,sizeof(out));
        return sceIoGetstat(p.c_str(), &out) >= 0;
    }

    void maybeRenderPopulating() {
        if (!scanAnimActive || !msgBox || gPopAnimFrames.empty()) return;
        unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
        if (scanAnimNextUs == 0 || now >= scanAnimNextUs) {
            renderOneFrame();
            unsigned long long delay = gPopAnimMinDelayUs ? gPopAnimMinDelayUs : 100000ULL;
            scanAnimNextUs = now + delay;
        }
    }

    void scanIsoRootDir(const std::string& base){
        if (!dirExists(base)) return;
        forEachEntry(base, [&](const SceIoDirent &e){
            maybeRenderPopulating();
            std::string name = e.d_name;
            if (FIO_S_ISDIR(e.d_stat.st_mode)) {
                if (isBlacklistedCategoryFolder("ISO/", name, base)) return;
                hasCategories = true;

                // Ensure the category is created/listed even if empty or contains no ISO-like files
                categories[name];  // creates empty vector if not present

                std::string catDir = base + name;
                forEachEntry(catDir, [&](const SceIoDirent &ee){
                    maybeRenderPopulating();
                    if (!FIO_S_ISDIR(ee.d_stat.st_mode)){
                        std::string fn = ee.d_name;
                        if (isIsoLike(fn)){
                            GameItem gi; gi.kind = GameItem::ISO_FILE;
                            gi.label = fn;
                            gi.path  = joinDirFile(catDir, fn.c_str());
                            SceIoStat st;
                            if (getStat(gi.path, st)){
                                gi.time     = st.sce_st_mtime;
                                gi.sortKey  = buildLegacySortKey(gi.time);
                                gi.sizeBytes= (uint64_t)st.st_size;
                            }

                            if (endsWithNoCase(fn, ".iso")) {
                                std::string t; if (readIsoTitle(gi.path, t)) gi.title = t;
                            } else if (endsWithNoCase(fn, ".cso") || endsWithNoCase(fn, ".zso")) {
                                std::string t; if (readCompressedIsoTitle(gi.path, t)) gi.title = t;
                            } else if (endsWithNoCase(fn, ".dax")) {
                                std::string t; if (readDaxTitle(gi.path, t)) gi.title = t;
                            } else if (endsWithNoCase(fn, ".jso")) {
                                std::string t; if (readJsoTitle(gi.path, t)) gi.title = t;
                            }

                            categories[name].push_back(gi);
                            snapInsertSorted(flatAll, gi);
                        }
                    }
                });
                std::sort(categories[name].begin(), categories[name].end(),
                          [](const GameItem& a, const GameItem& b){
                              return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
                          });
            } else {
                if (isIsoLike(name)){
                    GameItem gi; gi.kind = GameItem::ISO_FILE;
                    gi.label = name;
                    gi.path  = joinDirFile(base, name.c_str());
                    SceIoStat st;
                    if (getStat(gi.path, st)){
                        gi.time     = st.sce_st_mtime;
                        gi.sortKey  = buildLegacySortKey(gi.time);
                        gi.sizeBytes= (uint64_t)st.st_size;
                    }

                    if (endsWithNoCase(name, ".iso")) {
                        std::string t; if (readIsoTitle(gi.path, t)) gi.title = t;
                    } else if (endsWithNoCase(name, ".cso") || endsWithNoCase(name, ".zso")) {
                        std::string t; if (readCompressedIsoTitle(gi.path, t)) gi.title = t;
                    } else if (endsWithNoCase(name, ".dax")) {
                        std::string t; if (readDaxTitle(gi.path, t)) gi.title = t;
                    } else if (endsWithNoCase(name, ".jso")) {
                        std::string t; if (readJsoTitle(gi.path, t)) gi.title = t;
                    }

                    uncategorized.push_back(gi);
                    snapInsertSorted(flatAll, gi);
                }
            }
        });
    }


    void scanGameRootDir(const std::string& base){
        if (!dirExists(base)) return;
        forEachEntry(base, [&](const SceIoDirent &e){
            maybeRenderPopulating();
            if (!FIO_S_ISDIR(e.d_stat.st_mode)) return;
            std::string name = e.d_name;
            std::string baseName = stripCategoryPrefixes(name);
            bool isBlacklisted = isBlacklistedBaseNameFor(base, baseName);
            if (isBlacklisted && strcasecmp(name.c_str(), baseName.c_str()) != 0) {
                renameIfExists(base, name, baseName);
                name = baseName;
            }

            // If the folder itself contains an EBOOT.PBP, treat it as a stand-alone game (UNCATEGORIZED).
            std::string folderNoSlashRoot = joinDirFile(base, name.c_str());
            if (dirExists(folderNoSlashRoot) && !findEbootCaseInsensitive(folderNoSlashRoot).empty()){
                GameItem gi; gi.kind = GameItem::EBOOT_FOLDER;
                gi.label = name;
                gi.path  = folderNoSlashRoot;
                SceIoStat stF{};
                if (getStatDirNoSlash(gi.path, stF)) {
                    gi.time     = stF.sce_st_mtime;
                    gi.sortKey  = buildLegacySortKey(gi.time);
                }
                // Optional: compute folder size
                uint64_t folderBytes = 0;
                sumDirBytes(gi.path, folderBytes);
                gi.sizeBytes = folderBytes;

                std::string t; if (getFolderTitle(gi.path, t)) gi.title = t;
                uncategorized.push_back(gi);
                snapInsertSorted(flatAll, gi);
                return;
            }

            // Otherwise, treat it as a CATEGORY folder (regardless of CAT_ prefix).
            if (isBlacklisted) return;
            hasCategories = true;
            categories[name];  // creates empty vector if not present

            std::string catDir = base + name;
            forEachEntry(catDir, [&](const SceIoDirent &sub){
                maybeRenderPopulating();
                if (FIO_S_ISDIR(sub.d_stat.st_mode)){
                    std::string title = sub.d_name;
                    std::string folderNoSlash = joinDirFile(catDir, title.c_str());
                    if (dirExists(folderNoSlash)){
                        if (!findEbootCaseInsensitive(folderNoSlash).empty()){
                            GameItem gi; gi.kind = GameItem::EBOOT_FOLDER;
                            gi.label = title;
                            gi.path  = folderNoSlash;
                            SceIoStat stF{};
                            if (getStatDirNoSlash(gi.path, stF)) {
                                gi.time     = stF.sce_st_mtime;
                                gi.sortKey  = buildLegacySortKey(gi.time);
                            }
                            // Optional: compute folder size for display (can be O(total files))
                            uint64_t folderBytes = 0;
                            sumDirBytes(gi.path, folderBytes);
                            gi.sizeBytes = folderBytes;

                            std::string t; if (getFolderTitle(gi.path, t)) gi.title = t;
                            categories[name].push_back(gi);
                            snapInsertSorted(flatAll, gi);
                        }
                    }
                }
            });
            std::sort(categories[name].begin(), categories[name].end(),
                      [](const GameItem& a, const GameItem& b){
                          return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
                      });
        });
    }


    void scanDevice(const std::string& dev){
            resetLists();
            gclLoadBlacklistFor(dev);

        const char* isoRoots[]  = {"ISO/"};
            const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};

            for (size_t i=0;i<sizeof(isoRoots)/sizeof(isoRoots[0]);++i)  scanIsoRootDir(dev + std::string(isoRoots[i]));
            for (size_t i=0;i<sizeof(gameRoots)/sizeof(gameRoots[0]);++i) scanGameRootDir(dev + std::string(gameRoots[i]));


            if (!categories.empty()) hasCategories = true;

            if (!hasCategories){
                flatAll = uncategorized;
            } else {
                for (auto &kv : categories) categoryNames.push_back(kv.first);

                if (gclCfg.catsort) {
                    // When Sort Categories is ON, sort by leading XX (after optional CAT_)
                    std::sort(categoryNames.begin(), categoryNames.end(),
                            [](const std::string& a, const std::string& b){
                                auto parseXX = [](const std::string& s)->int{
                                    const char* p = s.c_str();
                                    if (startsWithCAT(p)) p += 4;
                                    if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')
                                        return (p[0]-'0')*10 + (p[1]-'0');
                                    return 0;
                                };
                                int ax = parseXX(a), bx = parseXX(b);
                                if (ax > 0 || bx > 0) {
                                    if (ax != bx) return ax < bx;
                                    // tie-breaker: alpha
                                    return strcasecmp(a.c_str(), b.c_str()) < 0;
                                }
                                // no numbers: fallback to alpha
                                return strcasecmp(a.c_str(), b.c_str()) < 0;
                            });
                } else {
                    // Sorting disabled: match Game Categories Lite (mtime desc)
                    sortCategoryNamesByMtime(categoryNames, dev);
                }

                if (!uncategorized.empty()) categories["Uncategorized"]; // flag presence
            }
            // With eager loading we no longer track per-category load flags.
        }


    // Add categories that exist on disk but are missing from the current cache (e.g., after removing blacklist entries).
    void refreshNewlyAllowedCategories(const std::string& dev) {
        if (dev.empty()) return;
        gclLoadBlacklistFor(dev);

        // Build a set of BASE names already present in the cache
        std::set<std::string, bool(*)(const std::string&, const std::string&)> existing(
            [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; }
        );
        std::unordered_map<std::string, bool> baseHasItems;
        for (const auto& kv : categories) {
            if (!strcasecmp(kv.first.c_str(), "Uncategorized")) continue;
            std::string base = stripCategoryPrefixes(kv.first);
            existing.insert(base);
            baseHasItems[base] = baseHasItems[base] || !kv.second.empty();
        }

        auto dropEmptyForBase = [&](const std::string& base){
            for (auto it = categories.begin(); it != categories.end(); ) {
                if (!strcasecmp(stripCategoryPrefixes(it->first).c_str(), base.c_str()) && it->second.empty()) {
                    it = categories.erase(it);
                } else ++it;
            }
        };

        auto addIsoCategory = [&](const std::string& rootLabel, const std::string& absRoot, const std::string& sub){
            if (isBlacklistedCategoryFolder(rootLabel, sub, absRoot)) return;
            std::string base = stripCategoryPrefixes(sub);
            if (existing.find(base) != existing.end() && baseHasItems[base]) return;

            std::string subAbs = joinDirFile(absRoot, sub.c_str());
            if (!findEbootCaseInsensitive(subAbs).empty()) return; // skip real games

            std::vector<GameItem> items;
            forEachEntry(subAbs, [&](const SceIoDirent &ee){
                if (!FIO_S_ISDIR(ee.d_stat.st_mode)){
                    std::string fn = ee.d_name;
                    if (isIsoLike(fn)){
                        GameItem gi; gi.kind = GameItem::ISO_FILE;
                        gi.label = fn;
                        gi.path  = joinDirFile(subAbs, fn.c_str());
                        SceIoStat st;
                        if (getStat(gi.path, st)){
                            gi.time     = st.sce_st_mtime;
                            gi.sortKey  = buildLegacySortKey(gi.time);
                            gi.sizeBytes= (uint64_t)st.st_size;
                        }

                        if (endsWithNoCase(fn, ".iso")) {
                            std::string t; if (readIsoTitle(gi.path, t)) gi.title = t;
                        } else if (endsWithNoCase(fn, ".cso") || endsWithNoCase(fn, ".zso")) {
                            std::string t; if (readCompressedIsoTitle(gi.path, t)) gi.title = t;
                        } else if (endsWithNoCase(fn, ".dax")) {
                            std::string t; if (readDaxTitle(gi.path, t)) gi.title = t;
                        } else if (endsWithNoCase(fn, ".jso")) {
                            std::string t; if (readJsoTitle(gi.path, t)) gi.title = t;
                        }

                        items.push_back(gi);
                    }
                }
            });

            categories[sub] = std::move(items);
            dropEmptyForBase(base);
            existing.insert(base);
            baseHasItems[base] = true;
            hasCategories = true;
        };

        const char* isoRoots[]  = {"ISO/"};                // drop ISO/PSP/ as a root
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME150/"}; // drop PSX/ and Utility/ as roots

        auto addGameCategory = [&](const std::string& rootLabel, const std::string& absRoot, const std::string& sub){
            if (isBlacklistedCategoryFolder(rootLabel, sub, absRoot)) return;
            std::string base = stripCategoryPrefixes(sub);
            if (existing.find(base) != existing.end() && baseHasItems[base]) return;

            std::string subAbs = joinDirFile(absRoot, sub.c_str());
            if (!findEbootCaseInsensitive(subAbs).empty()) return; // sub itself is a game folder

            std::vector<GameItem> items;
            forEachEntry(subAbs, [&](const SceIoDirent &e){
                if (FIO_S_ISDIR(e.d_stat.st_mode)) {
                    std::string title = e.d_name;
                    std::string folderNoSlash = joinDirFile(subAbs, title.c_str());
                    if (!dirExists(folderNoSlash)) return;
                    if (!findEbootCaseInsensitive(folderNoSlash).empty()){
                        GameItem gi; gi.kind = GameItem::EBOOT_FOLDER;
                        gi.label = title;
                        gi.path  = folderNoSlash;
                        SceIoStat stF{};
                        if (getStatDirNoSlash(gi.path, stF)) {
                            gi.time     = stF.sce_st_mtime;
                            gi.sortKey  = buildLegacySortKey(gi.time);
                        }
                        uint64_t folderBytes = 0;
                        sumDirBytes(gi.path, folderBytes);
                        gi.sizeBytes = folderBytes;

                        std::string t; if (getFolderTitle(gi.path, t)) gi.title = t;
                        items.push_back(gi);
                    }
                }
            });

            categories[sub] = std::move(items);
            dropEmptyForBase(base);
            existing.insert(base);
            baseHasItems[base] = true;
            hasCategories = true;
        };

        for (auto r : isoRoots) {
            std::string abs = dev + std::string(r);
            std::vector<std::string> subs; listSubdirs(abs, subs);
            for (auto &s : subs) addIsoCategory(r, abs, s);
        }
        for (auto r : gameRoots) {
            std::string abs = dev + std::string(r);
            std::vector<std::string> subs; listSubdirs(abs, subs);
            for (auto &s : subs) addGameCategory(r, abs, s);
        }

        // Do NOT inject categories from the opposite root; just mark it dirty so it refreshes on visit.
        if (isPspGo()) {
            std::string other = oppositeRootOf(dev);
            if (!other.empty()) markDeviceDirty(other);
        }
    }

    // Targeted refresh for a set of BASE names that were just un-blacklisted.
    void refreshCategoriesForBases(const std::string& dev, const std::vector<std::string>& bases) {
        if (dev.empty() || bases.empty()) return;
        gclLoadBlacklistFor(dev);

        auto lower = [](std::string s){ for (char& c : s) c = toLowerC(c); return s; };
        std::unordered_set<std::string> targets;
        for (auto b : bases) {
            b = stripCategoryPrefixes(sanitizeFilename(b));
            if (b.empty()) continue;
            targets.insert(lower(b));
        }
        if (targets.empty()) return;

        auto isTargetBase = [&](const std::string& name)->bool{
            std::string base = stripCategoryPrefixes(name);
            return targets.find(lower(base)) != targets.end();
        };

        auto refreshIso = [&](const std::string& rootLabel, const std::string& absRoot){
            std::vector<std::string> subs; listSubdirs(absRoot, subs);
            for (auto &sub : subs) {
                if (!isTargetBase(sub)) continue;
                if (isBlacklistedCategoryFolder(rootLabel, sub, absRoot)) continue;
                std::string subAbs = joinDirFile(absRoot, sub.c_str());
                if (!findEbootCaseInsensitive(subAbs).empty()) continue; // skip real games

                std::vector<GameItem> items;
                forEachEntry(subAbs, [&](const SceIoDirent &ee){
                    if (!FIO_S_ISDIR(ee.d_stat.st_mode)){
                        std::string fn = ee.d_name;
                        if (isIsoLike(fn)){
                            GameItem gi; gi.kind = GameItem::ISO_FILE;
                            gi.label = fn;
                            gi.path  = joinDirFile(subAbs, fn.c_str());
                            SceIoStat st;
                            if (getStat(gi.path, st)){
                                gi.time     = st.sce_st_mtime;
                                gi.sortKey  = buildLegacySortKey(gi.time);
                                gi.sizeBytes= (uint64_t)st.st_size;
                            }

                            if (endsWithNoCase(fn, ".iso")) {
                                std::string t; if (readIsoTitle(gi.path, t)) gi.title = t;
                            } else if (endsWithNoCase(fn, ".cso") || endsWithNoCase(fn, ".zso")) {
                                std::string t; if (readCompressedIsoTitle(gi.path, t)) gi.title = t;
                            } else if (endsWithNoCase(fn, ".dax")) {
                                std::string t; if (readDaxTitle(gi.path, t)) gi.title = t;
                            } else if (endsWithNoCase(fn, ".jso")) {
                                std::string t; if (readJsoTitle(gi.path, t)) gi.title = t;
                            }

                            items.push_back(gi);
                        }
                    }
                });

                categories[sub] = std::move(items);
                hasCategories = true;
            }
        };

        auto refreshGame = [&](const std::string& rootLabel, const std::string& absRoot){
            std::vector<std::string> subs; listSubdirs(absRoot, subs);
            for (auto &sub : subs) {
                if (!isTargetBase(sub)) continue;
                if (isBlacklistedCategoryFolder(rootLabel, sub, absRoot)) continue;

                std::string subAbs = joinDirFile(absRoot, sub.c_str());
                if (!findEbootCaseInsensitive(subAbs).empty()) continue; // sub itself is a game folder

                std::vector<GameItem> items;
                forEachEntry(subAbs, [&](const SceIoDirent &e){
                    if (FIO_S_ISDIR(e.d_stat.st_mode)) {
                        std::string title = e.d_name;
                        std::string folderNoSlash = joinDirFile(subAbs, title.c_str());
                        if (!dirExists(folderNoSlash)) return;
                        if (!findEbootCaseInsensitive(folderNoSlash).empty()){
                            GameItem gi; gi.kind = GameItem::EBOOT_FOLDER;
                            gi.label = title;
                            gi.path  = folderNoSlash;
                            SceIoStat stF{};
                            if (getStatDirNoSlash(gi.path, stF)) {
                                gi.time     = stF.sce_st_mtime;
                                gi.sortKey  = buildLegacySortKey(gi.time);
                            }
                            uint64_t folderBytes = 0;
                            sumDirBytes(gi.path, folderBytes);
                            gi.sizeBytes = folderBytes;

                            std::string t; if (getFolderTitle(gi.path, t)) gi.title = t;
                            items.push_back(gi);
                        }
                    }
                });

                categories[sub] = std::move(items);
                hasCategories = true;
            }
        };

        const char* isoRoots[]  = {"ISO/"};                // drop ISO/PSP/ as a root
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME150/"}; // drop PSX/ and Utility/ as roots
        for (auto r : isoRoots)  refreshIso(r,   dev + std::string(r));
        for (auto r : gameRoots) refreshGame(r, dev + std::string(r));
    }



    void clearUI(){
        rowFreeBytes.clear();
        rowReason.clear();      // <--- add
        rowNeedBytes.clear();   // <--- add
        rowTotalBytes.clear();
        rowPresent.clear();
        entries.clear(); entryPaths.clear(); entryKinds.clear();
        rowFlags.clear(); rowFreeBytes.clear(); rowTotalBytes.clear(); rowPresent.clear();
        selectedIndex=0; scrollOffset=0;
        freeSelectionIcon();
    }


    void refillRowsFromWorkingPreserveSel(){
        int oldSel = selectedIndex, oldScroll = scrollOffset;
        entries.clear(); entryPaths.clear(); entryKinds.clear();
        for (const auto& gi : workingList){
            SceIoDirent e; memset(&e,0,sizeof(e));
            const char* name = (showTitles && !gi.title.empty()) ? gi.title.c_str() : gi.label.c_str();
            strncpy(e.d_name, name, sizeof(e.d_name)-1);
            entries.push_back(e);
            entryPaths.push_back(gi.path);
            entryKinds.push_back(gi.kind);
        }
        if (entries.empty()) { selectedIndex = 0; scrollOffset = 0; }
        else {
            if (oldSel >= (int)entries.size()) oldSel = (int)entries.size()-1;
            if (oldSel < 0) oldSel = 0;
            selectedIndex = oldSel;
            int maxScroll = (int)entries.size() - contentVisibleRows();
            if (maxScroll < 0) maxScroll = 0;
            if (oldScroll > maxScroll) oldScroll = maxScroll;
            if (oldScroll < 0) oldScroll = 0;
            scrollOffset = oldScroll;
        }
        showRoots = false;
        freeSelectionIcon();
    }

    // ---------------------------------------------------------------
    // Game Categories Lite - helpers & settings screen (class-scoped)
    // ---------------------------------------------------------------

    // Pick ef0:/ on PSP Go if present; otherwise ms0:/
    std::string gclPickDeviceRoot() {
        if (isPspGo() && DeviceExists("ef0:/")) return "ef0:/";
        return "ms0:/";
    }

    void gclCollectSepluginsDirs(const std::string& root, std::vector<std::string>& out) {
        out.clear();
        std::string upper = joinDirFile(root, "SEPLUGINS");
        std::string lower = joinDirFile(root, "seplugins");
        const bool hasUpper = dirExists(upper);
        const bool hasLower = dirExists(lower);

        if (hasLower) out.push_back(lower);
        if (hasUpper && (!hasLower || strcasecmp(upper.c_str(), lower.c_str()) != 0)) out.push_back(upper);
        if (!hasLower && !hasUpper) out.push_back(lower); // prefer lowercase for create
    }

    std::string gclSepluginsDirForRoot(const std::string& root) {
        std::vector<std::string> dirs;
        gclCollectSepluginsDirs(root, dirs);
        return dirs.empty() ? joinDirFile(root, "seplugins") : dirs.front();
    }

    // Recursive search under /SEPLUGINS for category_lite.prx
    std::string gclFindCategoryLitePrx(const std::string& sepluginsNoSlash) {
        SceUID d = kfeIoOpenDir(sepluginsNoSlash.c_str());
        if (d < 0) return {};
        std::vector<std::string> stack{sepluginsNoSlash};
        kfeIoCloseDir(d);

        while (!stack.empty()) {
            std::string dpath = stack.back(); stack.pop_back();
            SceUID dd = kfeIoOpenDir(dpath.c_str()); if (dd < 0) continue;
            SceIoDirent ent; memset(&ent, 0, sizeof(ent));
            while (kfeIoReadDir(dd, &ent) > 0) {
                trimTrailingSpaces(ent.d_name);
                if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
                std::string child = joinDirFile(dpath, ent.d_name);
                if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
                    stack.push_back(child);
                } else if (strcasecmp(ent.d_name, "category_lite.prx") == 0) {
                    kfeIoCloseDir(dd);
                    return child;
                }
                memset(&ent, 0, sizeof(ent));
            }
            kfeIoCloseDir(dd);
        }
        return {};
    }

    // Use the existing global helper to find VSH.txt / PLUGINS.txt (case-insensitive)
    std::string gclFindTxtInSeplugins(const std::string& seplugins, const char* wantUpperOrLower){
        return findFileCaseInsensitive(seplugins, wantUpperOrLower);
    }

    std::string gclFindCategoryLitePrxAny(const std::string& primaryRoot) {
        std::vector<std::string> dirs;
        gclCollectSepluginsDirs(primaryRoot, dirs);
        for (const auto& dir : dirs) {
            std::string found = gclFindCategoryLitePrx(dir);
            if (!found.empty()) return found;
        }

        return {};
    }

    std::string gclFindArkPluginsFile(std::string& outSeplugins) {
        std::string primaryRoot = gclPickDeviceRoot();
        std::vector<std::string> dirs;
        gclCollectSepluginsDirs(primaryRoot, dirs);

        for (const auto& dir : dirs) {
            std::string plugins = gclFindTxtInSeplugins(dir, "PLUGINS.TXT");
            if (!plugins.empty()) { outSeplugins = dir; return plugins; }
        }

        outSeplugins = dirs.empty() ? joinDirFile(primaryRoot, "seplugins") : dirs.front();
        return joinDirFile(outSeplugins, "PLUGINS.txt");
    }

    std::string gclFindProVshFile(std::string& outSeplugins) {
        std::string primaryRoot = gclPickDeviceRoot();
        std::vector<std::string> dirs;
        gclCollectSepluginsDirs(primaryRoot, dirs);

        for (const auto& dir : dirs) {
            std::string vsh = gclFindTxtInSeplugins(dir, "VSH.TXT");
            if (!vsh.empty()) { outSeplugins = dir; return vsh; }
        }

        outSeplugins = dirs.empty() ? joinDirFile(primaryRoot, "seplugins") : dirs.front();
        return joinDirFile(outSeplugins, "VSH.txt");
    }

    static bool gclReadWholeText(const std::string& path, std::string& out){
        SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
        if (fd < 0) return false;
        SceIoStat st{}; if (sceIoGetstat(path.c_str(), &st) < 0) { sceIoClose(fd); return false; }
        if (st.st_size <= 0 || st.st_size > 512*1024) { sceIoClose(fd); return false; }
        out.resize((size_t)st.st_size);
        int got = sceIoRead(fd, &out[0], (uint32_t)out.size());
        sceIoClose(fd);
        return got >= 0;
    }

    static bool gclWriteWholeText(const std::string& path, const std::string& data){
        SceUID fd = sceIoOpen(path.c_str(), PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC, 0777);
        if (fd < 0) return false;
        int wr = sceIoWrite(fd, data.data(), (uint32_t)data.size());
        sceIoClose(fd);
        return wr == (int)data.size();
    }

    // Accepts: "1" (both files), and for ARK PLUGINS.txt only: "on", "true", "enabled"
    // Accepts:
    //  - PRO/ME VSH.txt: "<path> 1"
    //  - ARK-4 PLUGINS.txt: "vsh, <path>, 1" (also accepts on/true/enabled)
    bool gclLineEnables(const std::string& line, bool arkPluginsTxt){
        auto toLower = [](std::string s){ for(char& c:s) if(c>='A'&&c<='Z') c=c-'A'+'a'; return s; };
        auto trim = [](std::string s){
            size_t a=0,b=s.size();
            while (a<b && (s[a]==' '||s[a]=='\t')) ++a;
            while (b>a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\r')) --b;
            return s.substr(a,b-a);
        };

        if (!arkPluginsTxt) {
            // PRO/ME (incl. LME): space-separated, but tolerate CSV-style lines too.
            std::string l = toLower(line);
            std::string trimmed = trim(l);
            if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') return false;
            if (l.find("category_lite.prx") == std::string::npos) return false;

            if (l.find(',') != std::string::npos) {
                // Treat as CSV: "vsh, <path>, 1"
                std::vector<std::string> cols;
                size_t start=0;
                while (start<=l.size()){
                    size_t pos = l.find(',', start);
                    if (pos == std::string::npos) pos = l.size();
                    cols.push_back(trim(l.substr(start, pos - start)));
                    start = pos + (pos < l.size() ? 1 : 0);
                    if (pos == l.size()) break;
                }
                if (cols.size() >= 3 && cols[1].find("category_lite.prx") != std::string::npos) {
                    const std::string& state = cols[2];
                    if (state == "0") return false;
                    if (state == "1" || state == "on" || state == "true" || state == "enabled") return true;
                    return true; // missing/unknown flag -> treat as enabled
                }
                return false;
            }

            std::vector<std::string> toks;
            size_t i=0; while (i<l.size()){
                while (i<l.size() && (l[i]==' '||l[i]=='\t'||l[i]=='\r'||l[i]=='\n')) ++i;
                size_t j=i; while (j<l.size() && !(l[j]==' '||l[j]=='\t'||l[j]=='\r'||l[j]=='\n')) ++j;
                if (j>i) toks.emplace_back(l.substr(i,j-i));
                i=j;
            }
            for (size_t k=0;k<toks.size();++k){
                if (toks[k].find("category_lite.prx") != std::string::npos){
                    bool sawZero = false;
                    for (size_t m=k+1; m<toks.size(); ++m){
                        const std::string& v = toks[m];
                        if (v == "1" || v == "on" || v == "true" || v == "enabled") return true;
                        if (v == "0") sawZero = true;
                    }
                    return !sawZero; // treat missing flag as enabled
                }
            }
            return false;
        } else {
            // ARK-4: CSV columns -> "vsh, <path>, 1"
            std::string l = toLower(line);
            std::vector<std::string> cols;
            size_t start=0;
            while (start<=l.size()){
                size_t pos = l.find(',', start);
                if (pos == std::string::npos) pos = l.size();
                cols.push_back(trim(l.substr(start, pos - start)));
                start = pos + (pos < l.size() ? 1 : 0);
                if (pos == l.size()) break;
            }
            if (cols.size() < 3) return false;
            if (cols[1].find("category_lite.prx") == std::string::npos) return false;
            const std::string& state = cols[2];
            return (state == "1" || state == "on" || state == "true" || state == "enabled");
        }
    }


    void gclComputeInitial() {
        std::string primaryRoot = gclPickDeviceRoot();
        {
            std::string found = gclFindCategoryLitePrxAny(primaryRoot);
            if (!found.empty()) gclPrxPath = found;
        }
        bool havePrx = !gclPrxPath.empty();

        // ARK-4: PLUGINS.txt (search both roots)
        bool arkEnabled = false;
        std::string arkSe;
        std::string plugins = gclFindArkPluginsFile(arkSe);
        if (!plugins.empty()){
            std::string txt; if (gclReadWholeText(plugins, txt)) {
                size_t pos=0, s=0;
                while (pos<=txt.size()){
                    if (pos==txt.size() || txt[pos]=='\n' || txt[pos]=='\r'){
                        std::string line = txt.substr(s, pos-s);
                        if (gclLineEnables(line, true)) { arkEnabled = true; break; }
                        if (pos+1<txt.size() && txt[pos]=='\r' && txt[pos+1]=='\n') ++pos;
                        s = pos + 1;
                    }
                    ++pos;
                }
            }
        }

        // PRO/ME (incl. LME): VSH.txt (search both roots)
        bool proEnabled = false;
        std::string proSe;
        std::string vsh = gclFindProVshFile(proSe);
        if (!vsh.empty()){
            std::string txt; if (gclReadWholeText(vsh, txt)) {
                size_t pos=0, s=0;
                while (pos<=txt.size()){
                    if (pos==txt.size() || txt[pos]=='\n' || txt[pos]=='\r'){
                        std::string line = txt.substr(s, pos-s);
                        if (gclLineEnables(line, false)) { proEnabled = true; break; }
                        if (pos+1<txt.size() && txt[pos]=='\r' && txt[pos+1]=='\n') ++pos;
                        s = pos + 1;
                    }
                    ++pos;
                }
            }
        }

        // Don’t gate enablement on PRX discovery; VSH/PLUGINS are the source of truth.
        gclArkOn = arkEnabled;
        gclProOn = proEnabled;

        if (havePrx) {
            gclDevice = rootPrefix(gclPrxPath);
        } else if (!proSe.empty()) {
            gclDevice = rootPrefix(proSe);
        } else if (!arkSe.empty()) {
            gclDevice = rootPrefix(arkSe);
        } else {
            gclDevice = primaryRoot;
        }
    }

    // Update/append a line enabling/disabling the PRX
    // Update/append a line enabling/disabling the PRX
    // Writes either:
    //  - PRO/ME: "<path> 1/0"
    //  - ARK-4:  "vsh, <path>, 1/0"
    bool gclWriteEnableToFile(const std::string& filePath, bool enable, bool arkPluginsTxt){
        auto toLower = [](std::string s){ for(char& c:s) if(c>='A'&&c<='Z') c=c-'A'+'a'; return s; };
        auto trim = [](std::string s){
            size_t a=0,b=s.size();
            while (a<b && (s[a]==' '||s[a]=='\t')) ++a;
            while (b>a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\r')) --b;
            return s.substr(a,b-a);
        };

        std::string txt;
        gclReadWholeText(filePath, txt); // ok if missing; we’ll create

        // split into lines (preserve CRLF)
        std::vector<std::string> lines;
        size_t i=0, s=0; 
        while (i<=txt.size()){
            if (i==txt.size() || txt[i]=='\n' || txt[i]=='\r'){
                lines.emplace_back(txt.substr(s, i-s));
                if (i+1<txt.size() && txt[i]=='\r' && txt[i+1]=='\n') ++i;
                s = i+1;
            }
            ++i;
        }
        if (txt.empty()) lines.clear();

        bool found=false;
        for (auto& ln : lines){
            std::string low = toLower(ln);
            if (low.find("category_lite.prx") != std::string::npos){
                if (arkPluginsTxt) {
                    if (enable) {
                        if (gclPrxPath.empty()) return false;
                        ln = std::string("vsh, ") + gclPrxPath + ", 1";
                    } else {
                        // keep existing path if present; fallback to normalized disable
                        std::string keepPath;
                        // parse CSV in lowercase copy to identify columns
                        std::vector<std::string> cols;
                        size_t start = 0;
                        while (start <= low.size()){
                            size_t pos = low.find(',', start);
                            if (pos == std::string::npos) pos = low.size();
                            cols.push_back(trim(low.substr(start, pos - start)));
                            start = pos + (pos < low.size() ? 1 : 0);
                            if (pos == low.size()) break;
                        }
                        if (cols.size() >= 2 && cols[1].find("category_lite.prx") != std::string::npos) {
                            // recover original cased path from the source line
                            std::vector<std::string> colsRaw;
                            start = 0;
                            while (start <= ln.size()){
                                size_t pos = ln.find(',', start);
                                if (pos == std::string::npos) pos = ln.size();
                                colsRaw.push_back(trim(ln.substr(start, pos - start)));
                                start = pos + (pos < ln.size() ? 1 : 0);
                                if (pos == ln.size()) break;
                            }
                            if (colsRaw.size() >= 2) keepPath = colsRaw[1];
                        }
                        if (keepPath.empty()) keepPath = gclPrxPath;
                        if (keepPath.empty()) keepPath = "ms0:/SEPLUGINS/category_lite.prx";
                        ln = std::string("vsh, ") + keepPath + ", 0";
                    }
                } else {
                    // PRO/ME space-separated
                    if (enable) {
                        if (gclPrxPath.empty()) return false;
                        ln = gclPrxPath + " 1";
                    } else {
                        while (!ln.empty() && (ln.back()==' '||ln.back()=='\t')) ln.pop_back();
                        size_t sp = ln.find_last_of(" \t");
                        if (sp != std::string::npos) ln.erase(sp);
                        ln += " 0";
                    }
                }
                found = true; break;
            }
        }

        if (!found && enable){
            if (gclPrxPath.empty()) return false;
            if (!lines.empty() && !lines.back().empty()) lines.push_back(std::string());
            if (arkPluginsTxt) lines.push_back(std::string("vsh, ") + gclPrxPath + ", 1");
            else               lines.push_back(gclPrxPath + " 1");
        }

        std::string out;
        for (size_t k=0;k<lines.size();++k){ out += lines[k]; if (k+1<lines.size()) out += "\r\n"; }
        return gclWriteWholeText(filePath, out);
    }


    // NEW: load/save gclite.bin (CategoryConfig)
    static std::string defaultBlacklistRoot() {
        if (DeviceExists("ef0:/")) return "ef0:/";
        return "ms0:/";
    }

    static std::string blacklistRootKey(const std::string& dev) {
        std::string key = rootPrefix(dev);
        if (key.empty()) key = defaultBlacklistRoot();
        return key;
    }

    static std::string gclBlacklistPathFor(const std::string& root) {
        std::string key = root.empty() ? defaultBlacklistRoot() : root;
        std::string suffix = "ms0";
        if (!key.empty() && toLowerC(key[0]) == 'e') suffix = "ef0";
        return key + "seplugins/gclite_blacklist_" + suffix + ".txt";
    }

    static std::string trimSpaces(const std::string& in) {
        size_t a = 0, b = in.size();
        while (a < b && (in[a] == ' ' || in[a] == '\t')) ++a;
        while (b > a && (in[b-1] == ' ' || in[b-1] == '\t' || in[b-1] == '\r' || in[b-1] == '\n')) --b;
        return in.substr(a, b - a);
    }

    static std::string gclFilterRootKeyFor(const std::string& dev) {
        std::string key = rootPrefix(dev);
        if (!key.empty()) return key;
        if (DeviceExists("ms0:/")) return "ms0:/";
        if (DeviceExists("ef0:/")) return "ef0:/";
        return "ms0:/";
    }

    static std::string gclFilterPathFor(const std::string& root) {
        return root + "seplugins/gclite_filter.txt";
    }

    static void gclLoadFilterFor(const std::string& dev) {
        std::string root = gclFilterRootKeyFor(dev);
        if (root.empty()) return;
        if (gclFilterLoadedMap[root]) return;

        gclFilterLoadedMap[root] = true;
        std::vector<std::string> items;

        std::string txt;
        if (gclReadWholeText(gclFilterPathFor(root), txt)) {
            size_t pos = 0, start = 0;
            while (pos <= txt.size()) {
                if (pos == txt.size() || txt[pos] == '\n' || txt[pos] == '\r') {
                    std::string line = trimSpaces(txt.substr(start, pos - start));
                    line = stripCategoryPrefixes(line);
                    if (!line.empty()) {
                        bool dup = false;
                        for (const auto& w : items) {
                            if (!strcasecmp(w.c_str(), line.c_str())) { dup = true; break; }
                        }
                        if (!dup) items.push_back(line);
                    }
                    if (pos + 1 < txt.size() && txt[pos] == '\r' && txt[pos+1] == '\n') ++pos;
                    start = pos + 1;
                }
                ++pos;
            }
        }
        gclFilterMap[root] = std::move(items);
    }

    static bool gclSaveFilterFor(const std::string& dev,
                                 const std::unordered_map<std::string, std::string>& baseToDisplay) {
        std::string root = gclFilterRootKeyFor(dev);
        if (root.empty()) return false;
        gclFilterLoadedMap[root] = true;
        std::vector<std::string>& fl = gclFilterMap[root];

        std::string dir = root + "seplugins";
        sceIoMkdir(dir.c_str(), 0777);

        std::string out;
        for (size_t i = 0; i < fl.size(); ++i) {
            const std::string& base = fl[i];
            auto it = baseToDisplay.find(base);
            const std::string& line = (it != baseToDisplay.end()) ? it->second : base;
            out += line;
            if (i + 1 < fl.size()) out += "\r\n";
        }
        if (!out.empty()) out += "\r\n";
        return gclWriteWholeText(gclFilterPathFor(root), out);
    }

    static void gclLoadBlacklistFor(const std::string& dev) {
        std::string root = blacklistRootKey(dev);
        if (root.empty()) return;
        if (gclBlacklistLoadedMap[root]) return;

        gclBlacklistLoadedMap[root] = true;
        std::vector<std::string> items;

        std::string txt;
        if (gclReadWholeText(gclBlacklistPathFor(root), txt)) {
            size_t pos = 0, start = 0;
            while (pos <= txt.size()) {
                if (pos == txt.size() || txt[pos] == '\n' || txt[pos] == '\r') {
                    std::string line = trimSpaces(txt.substr(start, pos - start));
                    line = stripCategoryPrefixes(sanitizeFilename(line));
                    if (!line.empty()) {
                        bool dup = false;
                        for (auto &w : items) {
                            if (!strcasecmp(w.c_str(), line.c_str())) { dup = true; break; }
                        }
                        if (!dup) items.push_back(line);
                    }
                    if (pos + 1 < txt.size() && txt[pos] == '\r' && txt[pos+1] == '\n') ++pos;
                    start = pos + 1;
                }
                ++pos;
            }
        }
        gclBlacklistMap[root] = std::move(items);
    }

    static bool gclSaveBlacklistFor(const std::string& dev) {
        std::string root = blacklistRootKey(dev);
        if (root.empty()) return false;
        gclBlacklistLoadedMap[root] = true;
        std::vector<std::string>& bl = gclBlacklistMap[root];

        std::string dir = root + "seplugins";
        sceIoMkdir(dir.c_str(), 0777);

        std::string out;
        for (size_t i = 0; i < bl.size(); ++i) {
            out += bl[i];
            if (i + 1 < bl.size()) out += "\r\n";
        }
        return gclWriteWholeText(gclBlacklistPathFor(root), out);
    }

    static bool isBlacklistedBaseNameFor(const std::string& root, const std::string& base) {
        if (base.empty() || !blacklistActive()) return false;
        gclLoadBlacklistFor(root);
        const auto& bl = gclBlacklistMap[blacklistRootKey(root)];
        for (const auto& w : bl) {
            if (w.empty()) continue;
            if (!strcasecmp(base.c_str(), w.c_str())) return true;
        }
        return false;
    }

    bool isBlacklistedBaseName(const std::string& base) {
        return isBlacklistedBaseNameFor(blacklistRootKey(currentDevice), base);
    }

    static std::string normalizeBlacklistInput(const std::string& raw) {
        std::string t = trimSpaces(raw);
        if (t.empty()) return {};
        t = stripCategoryPrefixes(sanitizeFilename(t));
        if (t.empty()) return {};
        return t;
    }

    std::unordered_map<std::string, std::string> buildCategoryBaseToDisplayMap() const {
        std::unordered_map<std::string, std::string> out;
        out.reserve(categories.size());
        for (const auto& kv : categories) {
            if (!strcasecmp(kv.first.c_str(), "Uncategorized")) continue;
            std::string base = stripCategoryPrefixes(kv.first);
            if (base.empty()) continue;
            if (out.find(base) == out.end()) out.emplace(base, kv.first);
        }
        return out;
    }

    bool isFilteredBaseName(const std::string& base) {
        if (base.empty()) return false;
        gclLoadFilterFor(currentDevice);
        const std::string root = gclFilterRootKeyFor(currentDevice);
        const auto& fl = gclFilterMap[root];
        for (const auto& w : fl) {
            if (!strcasecmp(w.c_str(), base.c_str())) return true;
        }
        return false;
    }

    void refreshGclFilterFile() {
        gclLoadFilterFor(currentDevice);
        gclSaveFilterFor(currentDevice, buildCategoryBaseToDisplayMap());
    }

    void toggleGclFilterForCategory(const std::string& displayName) {
        std::string base = stripCategoryPrefixes(displayName);
        if (base.empty()) return;
        gclLoadFilterFor(currentDevice);
        const std::string root = gclFilterRootKeyFor(currentDevice);
        auto& fl = gclFilterMap[root];
        for (auto it = fl.begin(); it != fl.end(); ++it) {
            if (!strcasecmp(it->c_str(), base.c_str())) {
                fl.erase(it);
                refreshGclFilterFile();
                return;
            }
        }
        fl.push_back(base);
        refreshGclFilterFile();
    }

    void updateFilterOnCategoryRename(const std::string& oldDisplay, const std::string& newDisplay) {
        std::string oldBase = stripCategoryPrefixes(oldDisplay);
        std::string newBase = stripCategoryPrefixes(newDisplay);
        if (oldBase.empty() || newBase.empty()) return;
        gclLoadFilterFor(currentDevice);
        const std::string root = gclFilterRootKeyFor(currentDevice);
        auto& fl = gclFilterMap[root];
        bool had = false;
        for (auto it = fl.begin(); it != fl.end(); ++it) {
            if (!strcasecmp(it->c_str(), oldBase.c_str())) {
                fl.erase(it);
                had = true;
                break;
            }
        }
        if (!had) return;
        bool dup = false;
        for (const auto& w : fl) {
            if (!strcasecmp(w.c_str(), newBase.c_str())) { dup = true; break; }
        }
        if (!dup) fl.push_back(newBase);
        refreshGclFilterFile();
    }

    void applyBlacklistChanges() {
        gclSaveBlacklistFor(currentDevice);

        if (!blacklistActive()) {
            gclPendingUnblacklistMap[blacklistRootKey(currentDevice)].clear();
            buildGclSettingsRowsFromState();
            return;
        }

        // Force re-enforcement and cache patching so renamed folders disappear from categories immediately
        gclSchemeApplied.clear();
        s_catNamingEnforced.clear();

        if (currentDevice.empty()) {
            buildGclSettingsRowsFromState();
            return;
        }

        enforceCategorySchemeForDevice(currentDevice);
        if (isPspGo()) {
            std::string other = oppositeRootOf(currentDevice);
            if (!other.empty()) enforceCategorySchemeForDevice(other);
        }

        // Lightweight refresh: add back categories that were un-blacklisted, otherwise add any missing
        auto &pending = gclPendingUnblacklistMap[blacklistRootKey(currentDevice)];
        if (blacklistActive() && !pending.empty()) {
            refreshCategoriesForBases(currentDevice, pending);
            if (isPspGo()) {
                std::string other = oppositeRootOf(currentDevice);
                if (!other.empty()) markDeviceDirty(other); // refresh on next switch
            }
            pending.clear();
        } else if (blacklistActive()) {
            refreshNewlyAllowedCategories(currentDevice);
        } else {
            pending.clear();
        }

        patchCategoryCacheFromSettings();
        buildGclSettingsRowsFromState();
    }

    static void gclLoadConfig() {
        const std::string path = gclConfigPath();
        SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0777);
        if (fd >= 0) {
            GclConfig tmp{};
            int rd = sceIoRead(fd, &tmp, sizeof(tmp));
            sceIoClose(fd);
            if (rd == (int)sizeof(tmp)) { gclCfg = tmp; gclCfgLoaded = true; }
        }
        // defaults if missing or size mismatch
        if (!gclCfgLoaded) {
            gclCfg = GclConfig{0,0,0,0,0};
            gclCfgLoaded = true;
        }
        gclLoadBlacklistFor(defaultBlacklistRoot());
    }

    static bool gclSaveConfig() {
        const std::string path = gclConfigPath();
        // Ensure seplugins directory exists
        sceIoMkdir((isPspGo()? "ef0:/seplugins" : "ms0:/seplugins"), 0777);
        SceUID fd = sceIoOpen(path.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd < 0) return false;
        int wr = sceIoWrite(fd, &gclCfg, sizeof(gclCfg));
        sceIoClose(fd);
        return wr == (int)sizeof(gclCfg);
    }

    // Helpers to stringify current values (labels from the plugin’s language table)
    static const char* gclModeLabel(uint32_t m) {
        switch (m) { case 0: return "Multi MS"; case 1: return "Contextual menu"; case 2: return "Folders"; default: return "?"; }
    }
    static const char* gclPrefixLabel(uint32_t p) {
        return (p==0) ? "None" : "Use CAT prefix";
    }
    static const char* gclUncatLabel(uint32_t u, bool go) {
        switch (u) { case 0: return "No";
                    case 1: return "Only Memory Stick\u2122";
                    case 2: return "Only Internal Storage";
                    case 3: return "Both";
                    default: return "?"; }
    }
    static const char* gclSortLabel(uint32_t s) {
        return (s==0) ? "No" : "Yes";
    }

    void buildGclSettingsRowsFromState() {
        const int prevSel = selectedIndex, prevOff = scrollOffset;

        entries.clear(); entryPaths.clear(); entryKinds.clear();
        rowFlags.clear(); rowFreeBytes.clear(); rowReason.clear(); rowNeedBytes.clear(); rowTotalBytes.clear(); rowPresent.clear();

        gclLoadBlacklistFor(currentDevice);

        auto add = [&](const std::string& label, bool disabled = false){
            SceIoDirent e{}; e.d_stat.st_mode = FIO_S_IFDIR;
            strncpy(e.d_name, label.c_str(), sizeof(e.d_name)-1);
            entries.push_back(e);
            entryPaths.emplace_back("");
            entryKinds.push_back(GameItem::ISO_FILE);
            rowFlags.push_back(disabled ? ROW_DISABLED : 0); rowFreeBytes.push_back(0);
            rowReason.push_back(RD_NONE); rowNeedBytes.push_back(0);
        };

        gclLoadBlacklistFor(currentDevice);
        const auto& blNow = gclBlacklistMap[blacklistRootKey(currentDevice)];

        add(std::string("Category Mode: ")      + gclModeLabel(gclCfg.mode));
        add(std::string("Category Prefix: ")    + gclPrefixLabel(gclCfg.prefix));
        add(std::string("Show Uncategorized: ") + gclUncatLabel(gclCfg.uncategorized, isPspGo()));
        add(std::string("Sort Categories: ")    + gclSortLabel(gclCfg.catsort));
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "Folder Rename Blacklist: %d item%s", (int)blNow.size(),
                     blNow.size() == 1 ? "" : "s");
            add(buf, gclCfg.prefix == 0);
        }

        selectedIndex = std::min(prevSel, (int)entries.size()-1);
        scrollOffset  = std::min(prevOff, std::max(0, (int)entries.size()-1));
        // Keep selection on a selectable row
        if (!entries.empty()) {
            if (selectedIndex < 0) selectedIndex = 0;
            int firstEnabled = 0;
            while (firstEnabled < (int)entries.size() && (rowFlags[firstEnabled] & ROW_DISABLED)) firstEnabled++;
            if (firstEnabled >= (int)entries.size()) firstEnabled = 0;
            if (selectedIndex >= (int)rowFlags.size() || (rowFlags[selectedIndex] & ROW_DISABLED)) {
                selectedIndex = firstEnabled;
            }
            if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
            const int visible = gclSettingsVisibleRows();
            if (selectedIndex >= scrollOffset + visible) scrollOffset = selectedIndex - visible + 1;
        }
        showRoots = false; view = View_GclSettings;
    }

    void openBlacklistModal(int selectIndex = 0) {
        optMenuOwnedLabels.clear();
        gclLoadBlacklistFor(currentDevice);
        const auto& blNow = gclBlacklistMap[blacklistRootKey(currentDevice)];
        optMenuOwnedLabels.reserve(blNow.size() + 3);
        std::vector<OptionItem> items;
        optMenuOwnedLabels.push_back("Add new...");
        items.push_back({optMenuOwnedLabels.back().c_str(), false});
        if (blNow.empty()) {
            optMenuOwnedLabels.push_back("(No entries)");
            items.push_back({optMenuOwnedLabels.back().c_str(), true});
        } else {
            for (const auto& word : blNow) {
                optMenuOwnedLabels.push_back(word);
                items.push_back({optMenuOwnedLabels.back().c_str(), false});
            }
        }
        optMenu = new OptionListMenu("Folder Rename Blacklist", "Block these folder names from auto-prefixing with \"CAT_\"/converting to categories.", items, SCREEN_WIDTH, SCREEN_HEIGHT);
        gclPending = GCL_SK_Blacklist;
        if (selectIndex < 0) selectIndex = 0;
        if (selectIndex >= (int)items.size()) selectIndex = (int)items.size() - 1;
        optMenu->setSelected(selectIndex);
        optMenu->setOptionsOffsetAdjust(-3);
        optMenu->setMinVisibleRows(4);
        optMenu->setAllowTriangleDelete(true);

        SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
        optMenu->primeButtons(now.Buttons);
        inputWaitRelease = true;
    }

    bool deleteBlacklistAtIndex(int idx) {
        auto& bl = gclBlacklistMap[blacklistRootKey(currentDevice)];
        gclLoadBlacklistFor(currentDevice);
        if (idx < 0 || idx >= (int)bl.size()) return false;
        std::string removed = bl[idx];
        gclPendingUnblacklistMap[blacklistRootKey(currentDevice)].push_back(removed);
        bl.erase(bl.begin() + idx);
        gclBlacklistDirty = true;
        return true;
    }

    void rescanCurrentDeviceAfterBlacklist() {
        gclLoadBlacklistFor(currentDevice);
        gclSaveBlacklistFor(currentDevice);
        gclPendingUnblacklistMap[blacklistRootKey(currentDevice)].clear();

        const std::string root = rootPrefix(currentDevice);
        if (!root.empty()) {
            gclSchemeApplied.erase(root);
            s_catNamingEnforced.erase(root);
        }

        const char* popText = "Populating...";
        const float popScale = 1.0f;
        const int popPadX = 10;
        const int popPadY = 24;
        const int popLineH = (int)(24.0f * popScale + 0.5f);
        const float popTextW = measureTextWidth(popScale, popText);
        const int popExtraW = 4;
        const int popPanelW = (int)(popTextW + popPadX * 2 + popExtraW + 0.5f);
        const int popBottom = 14;
        const int popPanelH = popPadY + popLineH + popBottom - 24;
        const int popWrapTweak = 32;
        const int popForcedPxPerChar = 8;
        msgBox = new MessageBox(popText, nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                popScale, 0, "", popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                popPanelW, popPanelH);
        renderOneFrame();
        renderOneFrame();  // Double render to clear both buffers and prevent text artifacting

        scanDevice(currentDevice);
        {
            std::string key = rootPrefix(currentDevice);
            snapshotCurrentScan(deviceCache[key].snap);
            deviceCache[key].dirty = false;
        }

        freeSelectionIcon();
        if (iconCarryTex) {
            texFree(iconCarryTex);
            iconCarryTex = nullptr;
            iconCarryForPath.clear();
        }
        noIconPaths.clear();

        delete msgBox; msgBox = nullptr;
    }




    void openGclSettingsScreen(){
            // Load the plugin’s current settings so the list reflects them
            gclLoadConfig();   // ← no args; your function is void gclLoadConfig()

            // Your existing detection of ARK/PRO enablement, etc.
            gclComputeInitial();

            // Build rows (now showing the values from gclite.bin)
            buildGclSettingsRowsFromState();
        }




    void handleGclToggleAt(int idx){
        if (idx < 0) return;

        if (idx < (int)rowFlags.size() && (rowFlags[idx] & ROW_DISABLED)) {
            drawMessage("Enable CAT prefix first", COLOR_RED);
            sceKernelDelayThread(600*1000);
            return;
        }

        // No master toggles here anymore – just open the pickers for 0..3
        const bool go = isPspGo();

        if (idx == 0) {
            // Category mode
            std::vector<OptionItem> items = { {"Multi MS", false}, {"Contextual menu", false}, {"Folders", false} };
            optMenu = new OptionListMenu("Category Mode", "Choose how you want your categories to be presented in the XMB.", items, SCREEN_WIDTH, SCREEN_HEIGHT);
            gclPending = GCL_SK_Mode;
            optMenu->setSelected((int)gclCfg.mode);

            // NEW: Prime & debounce so held X/O won't auto-activate the choice
            SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
            optMenu->primeButtons(now.Buttons);
            inputWaitRelease = true;
        } else if (idx == 1) {
            // Category prefix
            std::vector<OptionItem> items = {
                {"None", false}, {"Use CAT prefix", false}
            };
            optMenu = new OptionListMenu("Category Prefix", "Require the \"CAT_\" prefix on folders meant to act as categories.", items, SCREEN_WIDTH, SCREEN_HEIGHT);
            gclPending = GCL_SK_Prefix;
            optMenu->setSelected((int)gclCfg.prefix);

            // NEW: Prime & debounce so held X/O won't auto-activate the choice
            SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
            optMenu->primeButtons(now.Buttons);
            inputWaitRelease = true;
        } else if (idx == 2) {
            // Show uncategorized
            std::vector<OptionItem> items = {
                {"No", false}, {"Only Memory Stick", false}, {"Only Internal Storage", !go}, {"Both", !go}
            };
            optMenu = new OptionListMenu("Show Uncategorized", "Enable the \"Uncategorized\" category for games not placed in a category subfolder.", items, SCREEN_WIDTH, SCREEN_HEIGHT);
            gclPending = GCL_SK_Uncat;
            optMenu->setSelected((int)gclCfg.uncategorized);
            optMenu->setOptionsOffsetAdjust(-3);
            optMenu->setMinVisibleRows(4);

            // NEW: Prime & debounce so held X/O won't auto-activate the choice
            SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
            optMenu->primeButtons(now.Buttons);
            inputWaitRelease = true;
        } else if (idx == 3) {
            // Sort categories
            std::vector<OptionItem> items = { {"No", false}, {"Yes", false} };
            optMenu = new OptionListMenu("Sort Categories", "Sort categories by using a \"##\" prefix. E.g.: \"01MyCategory\", \"CAT_02MyCategory\"", items, SCREEN_WIDTH, SCREEN_HEIGHT);
            gclPending = GCL_SK_Sort;
            optMenu->setSelected((int)gclCfg.catsort);

            // NEW: Prime & debounce so held X/O won't auto-activate the choice
            SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
            optMenu->primeButtons(now.Buttons);
            inputWaitRelease = true;
        } else if (idx == 4) {
            gclBlacklistDirty = false;
            openBlacklistModal(0);
        }
    }






        // ---- GCL: install-on-demand helpers --------------------------------

    bool copyFileBuffered(const std::string& src, const std::string& dst) {
        SceUID in = sceIoOpen(src.c_str(), PSP_O_RDONLY, 0);
        if (in < 0) return false;
        SceUID out = sceIoOpen(dst.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (out < 0) { sceIoClose(in); return false; }

        char buf[16 * 1024];
        for (;;) {
            int rd = sceIoRead(in, buf, sizeof(buf));
            if (rd < 0) { sceIoClose(in); sceIoClose(out); return false; }
            if (rd == 0) break;
            int wr = sceIoWrite(out, buf, rd);
            if (wr != rd) { sceIoClose(in); sceIoClose(out); return false; }
        }
        sceIoClose(in); sceIoClose(out);
        return true;
    }

    // Ensure category_lite.prx exists under <root>/SEPLUGINS
    // If missing, copy it from "<app base>/resources/category_lite.prx"
    bool gclEnsurePrxPresent(const std::string& sepluginsDir) {
        std::string seplugins = sepluginsDir;
        if (seplugins.empty()) {
            gclDevice = gclPickDeviceRoot();
            seplugins = gclSepluginsDirForRoot(gclDevice);
        }

        const std::string targetRoot = rootPrefix(seplugins);
        if (!gclPrxPath.empty()) {
            const std::string prxRoot = rootPrefix(gclPrxPath);
            if (!targetRoot.empty() && !strcasecmp(prxRoot.c_str(), targetRoot.c_str()) &&
                pathExists(gclPrxPath)) {
                return true;
            }
        }

        if (!dirExists(seplugins)) {
            sceIoMkdir(seplugins.c_str(), 0777);
        }
        std::string dst = joinDirFile(seplugins, "category_lite.prx");

        // Where we’re copying from (next to your images)
        std::string baseDir = getBaseDir(gExecPath);
        std::string src = baseDir + "resources/category_lite.prx";

        if (!copyFileBuffered(src, dst)) return false;

        gclPrxPath = dst;   // remember the exact installed path
        if (!targetRoot.empty()) gclDevice = targetRoot;
        return true;
    }

    void gclMaybeUpdatePrx() {
        if (!gclArkOn && !gclProOn) return;
        if (gclPrxPath.empty()) return;

        SceIoStat srcSt{}, curSt{};
        std::string baseDir = getBaseDir(gExecPath);
        std::string src = baseDir + "resources/category_lite.prx";
        if (!pathExists(src, &srcSt) || !pathExists(gclPrxPath, &curSt)) return;
        if (srcSt.st_size == curSt.st_size) return;

        std::string dir = parentOf(gclPrxPath);
        if (dir.empty()) return;
        std::string tmp = joinDirFile(dir, "category_lite_new.prx");
        std::string old = joinDirFile(dir, "category_lite_old.prx");

        if (pathExists(tmp)) sceIoRemove(tmp.c_str());
        if (!copyFileBuffered(src, tmp)) { if (pathExists(tmp)) sceIoRemove(tmp.c_str()); return; }

        if (pathExists(old)) sceIoRemove(old.c_str());
        if (sceIoRename(gclPrxPath.c_str(), old.c_str()) < 0) {
            sceIoRemove(tmp.c_str());
            return;
        }

        if (sceIoRename(tmp.c_str(), gclPrxPath.c_str()) < 0) {
            copyFileBuffered(tmp, gclPrxPath);
            if (!pathExists(gclPrxPath)) sceIoRename(old.c_str(), gclPrxPath.c_str());
            if (pathExists(tmp)) sceIoRemove(tmp.c_str());
            return;
        }
    }

    void buildRootRows(){
        clearUI();
        int preselect = -1;
        const uint64_t HEADROOM = (4ull << 20); // keep ~4 MiB headroom

        bool hasMs = false, hasEf = false;
        for (auto &r : roots) {
            if (r == "ms0:/") hasMs = true;
            if (r == "ef0:/") hasEf = true;
        }

        if (opPhase != OP_SelectDevice) {
            gclLoadConfig();
            gclComputeInitial();
            gclMaybeUpdatePrx();
        }

        auto addSimpleRow = [&](const char* name){
            SceIoDirent e{}; strncpy(e.d_name, name, sizeof(e.d_name)-1);
            e.d_stat.st_mode = FIO_S_IFDIR;
            entries.push_back(e);
            entryPaths.emplace_back("");
            entryKinds.push_back(GameItem::ISO_FILE);
            rowFlags.push_back(0); rowFreeBytes.push_back(0); rowReason.push_back(RD_NONE); rowNeedBytes.push_back(0);
            rowTotalBytes.push_back(0);
            rowPresent.push_back(0);
        };

        auto addDeviceRow = [&](const std::string& r, bool present){
            SceIoDirent e{}; strncpy(e.d_name, r.c_str(), sizeof(e.d_name)-1);
            e.d_stat.st_mode = FIO_S_IFDIR;
            entries.push_back(e);
            entryPaths.emplace_back("");
            entryKinds.push_back(GameItem::ISO_FILE);

            uint8_t flags = 0;
            RowDisableReason reason = RD_NONE;
            uint64_t needB = 0, freeB = 0, totalB = 0;

            if (!present) {
                flags |= ROW_DISABLED;
            } else {
                // Existing rule: running from ef0 → ms0 disabled
                if (runningFromEf0 && r == "ms0:/") {
                    flags |= ROW_DISABLED;
                    reason = RD_RUNNING_FROM_EF0;
                }

                // NEW: don't probe or show yellow text for same-device MOVE row
                const bool sameDeviceMoveRow =
                    (opPhase == OP_SelectDevice) &&
                    (actionMode == AM_Move) &&
                    !preOpDevice.empty() &&
                    (r == preOpDevice);

                // For other rows (or Copy), compute free/need and possibly disable
                if (opPhase == OP_SelectDevice &&
                    (actionMode == AM_Move || actionMode == AM_Copy)) {
                    if (!sameDeviceMoveRow) {
                        needB = bytesNeededForOp(opSrcPaths, opSrcKinds, r, /*isCopy=*/(actionMode == AM_Copy));
                        if (getFreeBytesCMF(r.c_str(), freeB)) {
                            if (needB > 0 && freeB + HEADROOM < needB) {
                                flags |= ROW_DISABLED;
                                reason = RD_NO_SPACE;
                            }
                        } else {
                            freeB = 0; // unknown; UI will say "probing"
                        }
                    } else {
                        // Still show actual free space for same-device Move.
                        getFreeBytesCMF(r.c_str(), freeB);
                    }
                }
            }

            if (present) getTotalBytesCMF(r.c_str(), totalB);

            rowFlags.push_back(flags);
            rowFreeBytes.push_back(freeB);
            rowReason.push_back(reason);
            rowNeedBytes.push_back(needB);
            rowTotalBytes.push_back(totalB);
            rowPresent.push_back(present ? 1 : 0);

            if (present && r == currentDevice) preselect = (int)entries.size() - 1;
        };

        if (opPhase != OP_SelectDevice) {
            addDeviceRow("ms0:/", hasMs);
            addDeviceRow("ef0:/", hasEf);
            addSimpleRow("__USB_MODE__");
            addSimpleRow("__GCL_TOGGLE__");
        } else {
            addDeviceRow("ms0:/", hasMs);
            addDeviceRow("ef0:/", hasEf);
        }

        showRoots = true; moving = false;

        selectedIndex = 0; scrollOffset = 0;
        if (preselect >= 0 && (rowFlags[preselect] & ROW_DISABLED) == 0) {
            selectedIndex = preselect;
        } else {
            for (int i = 0; i < (int)entries.size(); ++i)
                if ((rowFlags[i] & ROW_DISABLED) == 0) { selectedIndex = i; break; }
        }

        if (opPhase == OP_SelectDevice && !opDestDevice.empty()) {
            for (int i = 0; i < (int)entries.size(); ++i) {
                if (strcasecmp(entries[i].d_name, opDestDevice.c_str()) == 0) {
                    if ((rowFlags[i] & ROW_DISABLED) == 0) {
                        selectedIndex = i;
                        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                        if (selectedIndex >= scrollOffset + MAX_DISPLAY)
                            scrollOffset = selectedIndex - MAX_DISPLAY + 1;
                    }
                    break;
                }
            }
        }

        if (rootKeepGclSelection) {
            for (int i = 0; i < (int)entries.size(); ++i) {
                if (!strcmp(entries[i].d_name, "__GCL_TOGGLE__")) {
                    selectedIndex = i;
                    if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                    if (selectedIndex >= scrollOffset + MAX_DISPLAY)
                        scrollOffset = selectedIndex - MAX_DISPLAY + 1;
                    break;
                }
            }
            rootKeepGclSelection = false;
        }
    }


    void buildRootRowsForDevicePicker() {
        // Keep this a thin alias so it always uses the full Move-mode logic.
        opPhase = OP_SelectDevice;  // ensure buildRootRows() sees we’re in device-pick phase
        buildRootRows();            // this computes need/free, sets reasons, disables rows, and prints yellow text
    }

    void buildCategoryRows(){
        clearUI(); moving = false;
        catSortMode = false;
        catPickActive = false;
        catPickIndex = -1;
        {
            const std::string filterRoot = gclFilterRootKeyFor(currentDevice);
            if (!filterRoot.empty()) gclFilterLoadedMap[filterRoot] = false;
            gclLoadFilterFor(currentDevice);
        }

        // Enforce on-disk names to match current settings only when needed (run-once per root until settings change).
        {
            // Only enforce on first time we hit this device root this session
            const std::string root = rootPrefix(currentDevice); // e.g. "ms0:/" or "ef0:/"
            if (s_catNamingEnforced.insert(root).second) {
                enforceCategorySchemeForDevice(root);
                if (isPspGo()) {
                    const std::string other = (strncasecmp(root.c_str(), "ms0:/", 5) == 0) ? "ef0:/" : "ms0:/";
                    enforceCategorySchemeForDevice(other);
                    s_catNamingEnforced.insert(other); // mark the sibling as enforced too
                }
            }

            // Always patch the *cache* to reflect the current mapping (cheap & safe)
            patchCategoryCacheFromSettings();
        }


        std::vector<std::string> catsSorted = categoryNames;
        bool hasUnc = (categories.find("Uncategorized") != categories.end());
        if (hasUnc && gclCfg.uncategorized) catsSorted.push_back("Uncategorized");


        // Add an entry to open the plugin settings here
        {
            SceIoDirent z{}; strncpy(z.d_name, kCatSettingsLabel, sizeof(z.d_name)-1);
            z.d_stat.st_mode = FIO_S_IFDIR;
            entries.push_back(z);
        }

        int preselect = -1;
        for (auto &name : catsSorted){
            SceIoDirent e{}; strncpy(e.d_name, name.c_str(), sizeof(e.d_name)-1);
            e.d_stat.st_mode = FIO_S_IFDIR;
            entries.push_back(e);
            entryPaths.emplace_back("");
            entryKinds.push_back(GameItem::ISO_FILE);
            if (!currentCategory.empty() && name == currentCategory)
                preselect = (int)entries.size() - 1;
        }
        showRoots = false; view = View_Categories;

        // Default to the settings row if nothing specific is selected
        selectedIndex = (preselect >= 0) ? preselect : 0;
        {
            const int visible = categoryVisibleRows();
            scrollOffset = (selectedIndex >= visible) ? (selectedIndex - visible + 1) : 0;
        }
    }




    static std::string parseCategoryFromFullPath(const std::string& full, GameItem::Kind kind) {
        std::string sub = KfeFileOps::subrootFor(full, kind);
        std::string tail = KfeFileOps::afterSubroot(full, sub);
        std::string cat, leaf;
        KfeFileOps::parseCategoryFromPath(tail, cat, leaf);
        return cat; // "" if Uncategorized
    }

    void buildCategoryRowsForOp(){
        clearUI(); moving = false;
        opDisabledCategories.clear();

        // If selecting within current device (or no explicit device yet), gray out each source category.
        const bool sameDevice =
            (opDestDevice.empty() || !strcasecmp(opDestDevice.c_str(), preOpDevice.c_str()));

        bool srcHasUncategorized = false;

        if (sameDevice) {
            for (size_t i = 0; i < opSrcPaths.size(); ++i) {
                std::string cat = parseCategoryFromFullPath(opSrcPaths[i], opSrcKinds[i]);
                if (!cat.empty()) {
                    opDisabledCategories.insert(cat);
                } else {
                    srcHasUncategorized = true;
                }
            }
            // NEW: also disable "Uncategorized" if any source item is from Uncategorized
            if (srcHasUncategorized) {
                opDisabledCategories.insert("Uncategorized");
            }
        }

        std::vector<std::string> catsSorted = categoryNames;
        if (gclCfg.catsort) {
            std::sort(catsSorted.begin(), catsSorted.end(),
                    [](const std::string& a, const std::string& b){
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
            sortCategoryNamesByMtime(catsSorted, currentDevice);
        }

        bool alreadyHasUnc = false;
        for (auto& n : catsSorted) if (!strcasecmp(n.c_str(),"Uncategorized")) { alreadyHasUnc = true; break; }
        if (!alreadyHasUnc) catsSorted.push_back("Uncategorized");

        for (auto &name : catsSorted){
            SceIoDirent e{}; strncpy(e.d_name, name.c_str(), sizeof(e.d_name)-1);
            e.d_stat.st_mode = FIO_S_IFDIR;
            entries.push_back(e);
            entryPaths.emplace_back("");
            entryKinds.push_back(GameItem::ISO_FILE);
        }
        showRoots = false;
        // ... backing out to categories ...
        view = View_Categories;

        // (Removed call to buildCategoriesListForDevice(); not defined & not needed here)

        // Put the cursor back on the category we just left
        selectedIndex = findCategoryRowByName(currentCategory);
        if (selectedIndex < 0) selectedIndex = 0;
        if (selectedIndex >= (int)entries.size()) selectedIndex = (int)entries.size() - 1;

        // Clamp scroll to show the selected row
        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
        {
            const int visible = categoryVisibleRows();
            if (selectedIndex >= scrollOffset + visible)
                scrollOffset = selectedIndex - (visible - 1);
        }


        // Select the first NON-disabled category
        int sel = 0;
        while (sel < (int)entries.size() &&
            opDisabledCategories.find(entries[sel].d_name) != opDisabledCategories.end()) {
            ++sel;
        }
        if (sel >= (int)entries.size()) sel = 0;
        selectedIndex = sel;
        scrollOffset = 0;
    }

    void openDevice(const std::string& dev){
        currentDevice = dev;
        scanAnimActive = false;
        scanAnimNextUs = 0;

        // Decide based on cache dirtiness AND presence of a real snapshot
        std::string key = rootPrefix(currentDevice);
        auto &dc = deviceCache[key];                    // creates entry if missing

        // A snapshot is "present" only if it actually holds content/metadata we can render.
        const bool hasSnap =
            (!dc.snap.flatAll.empty()) ||
            (!dc.snap.uncategorized.empty()) ||
            (!dc.snap.categories.empty()) ||
            (!dc.snap.categoryNames.empty());

        const bool needsScan = dc.dirty || !hasSnap;   // first time OR explicitly dirtied

        if (needsScan) {
            const char* popText = "Populating...";
            const float popScale = 1.0f;
            const int popPadX = 10;
            const int popPadY = 24;
            const int popLineH = (int)(24.0f * popScale + 0.5f);
            const float popTextW = measureTextWidth(popScale, popText);
            const int popExtraW = 4;
            const int popPanelW = (int)(popTextW + popPadX * 2 + popExtraW + 0.5f);
            const int popBottom = 14;
            const int popPanelH = popPadY + popLineH + popBottom - 24;
            const int popWrapTweak = 32;
            const int popForcedPxPerChar = 8;
            msgBox = new MessageBox(popText, nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                    popScale, 0, "", popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                    popPanelW, popPanelH);
            if (gEnablePopAnimations && !gPopAnimDirs.empty()) {
                const std::string* animDir = nextPopAnimDir();
                if (animDir && ensurePopAnimLoaded(*animDir)) {
                    msgBox->setAnimation(gPopAnimFrames.data(), gPopAnimFrames.size(), POP_ANIM_TARGET_H);
                    scanAnimActive = true;
                }
            }
            renderOneFrame();
            renderOneFrame();  // Double render to clear both buffers and prevent text artifacting
            if (scanAnimActive) {
                unsigned long long delay = gPopAnimMinDelayUs ? gPopAnimMinDelayUs : 100000ULL;
                scanAnimNextUs = (unsigned long long)sceKernelGetSystemTimeWide() + delay;
            }

            scanDevice(currentDevice);                  // real, slow scan
            if (categories.empty() && uncategorized.empty()) {
                logEmptyScanOnce(currentDevice);
            }
            snapshotCurrentScan(dc.snap);               // cache the fresh results
            dc.dirty = false;

            moving = false;
            scanAnimActive = false;
            scanAnimNextUs = 0;
            delete msgBox; msgBox = nullptr;
        } else {
            // Instant reuse of cached snapshot (no message box)
            restoreScan(dc.snap);
            moving = false;
        }

        // Background-probe only the *opposite* device so UI stays snappy
        const bool canCrossDevices = dualDeviceAvailableFromMs0(); // PSP Go, running from ms0, both devices
        if (canCrossDevices) {
            bool hasMs = false, hasEf = false;
            for (auto &r : roots) { if (r=="ms0:/") hasMs = true; if (r=="ef0:/") hasEf = true; }

            const bool onMs = (strncasecmp(currentDevice.c_str(), "ms0:", 4) == 0);
            const bool probeMs = !onMs && hasMs;  // only probe ms0 if we're on ef0
            const bool probeEf =  onMs && hasEf;  // only probe ef0 if we're on ms0

            FreeSpaceInit();
            FreeSpaceSetPresence(probeMs, probeEf);     // <--- probe opposite only
            FreeSpaceRequestRefresh();
        }

        if (gclArkOn || gclProOn) {
            // RUN-ONCE per device root (ms0:/ or ef0:/). Avoids slow renames when backing out.
            {
                std::string rootKey = rootPrefix(currentDevice);  // "ms0:/" or "ef0:/"
                if (!rootKey.empty() && !gclSchemeApplied.count(rootKey)) {
                    enforceCategorySchemeForDevice(currentDevice);

                    // PSP Go: if we're on one root and the other exists, mirror the scheme once
                    if (isPspGo()) {
                        std::string other = oppositeRootOf(currentDevice);
                        if (!other.empty()) enforceCategorySchemeForDevice(other);
                    }

                    // Update the in-memory cache so ICON0s continue to resolve after renames
                    patchCategoryCacheFromSettings();

                    gclSchemeApplied.insert(rootKey);
                }
            }
            if (!hasCategories) {
                // No category folders present; fall back to a flat list so games are visible.
                rebuildFlatFromCache();
            } else {
                buildCategoryRows();
            }
        } else {
            // Categories Lite is Off → bypass categories and list only "Uncategorized"
            workingList = uncategorized;
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

    }

    void openCategory(const std::string& catName){
        currentCategory = catName;
        moving = false;
        if (catName == "Uncategorized") workingList = uncategorized;
        else {
            workingList.clear();
            auto it = categories.find(catName);
            if (it != categories.end()) workingList = it->second;
        }
        sortLikeLegacy(workingList);
        view = View_CategoryContents;
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

    // Quick overlay used when backing out of screens to avoid abrupt jumps.
    MessageBox* pushReturningModal(const char* text = "Returning...") {
        if (msgBox) { delete msgBox; msgBox = nullptr; }
        const float popScale = 1.0f;
        const int popPadX = 10;
        const int popPadY = 24;
        const int popLineH = (int)(24.0f * popScale + 0.5f);
        const float popTextW = measureTextWidth(popScale, text);
        const int popExtraW = 4;
        int popPanelW = (int)(popTextW + popPadX * 2 + popExtraW + 0.5f);
        popPanelW -= 6;
        const int popBottom = 14;
        const int popPanelH = popPadY + popLineH + popBottom - 24;
        const int popWrapTweak = 32;
        const int popForcedPxPerChar = 8;
        msgBox = new MessageBox(text, nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                popScale, 0, "", popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                popPanelW, popPanelH);
        renderOneFrame();
        return msgBox;
    }
    void popModal(MessageBox* box) {
        if (!box) return;
        delete box;
        if (msgBox == box) msgBox = nullptr;
        renderOneFrame();
    }

    // -----------------------------------------------------------
    // Commit timestamps (unchanged)
    // -----------------------------------------------------------
    static void fillStatTimes(SceIoStat &st, const ScePspDateTime &dt){
        memset(&st, 0, sizeof(SceIoStat));
        st.sce_st_mtime = dt;
        st.sce_st_ctime = dt;
        st.sce_st_atime = dt;
    }
    static void applyTimesLikeLegacy(const std::string& target, const ScePspDateTime &dt){
        SceIoStat st; fillStatTimes(st, dt);
        sceIoChstat(target.c_str(), &st, 0x08 | 0x10 | 0x20);
    }
    // Build a canonical, in-order set of targets from what the user *sees*.
    // Ensures every row has a real path; if a row isn’t in workingList yet,
    // we materialize a GameItem on the fly (so new copies/moves are included).
    void syncWorkingListFromScreen() {
        if (!(view == View_AllFlat || view == View_CategoryContents)) return;
        std::vector<GameItem> synced;
        synced.reserve(entries.size());

        // Fast look-up of existing items by path
        std::map<std::string, GameItem> byPath;
        for (const auto& gi : workingList) byPath[gi.path] = gi;

        for (size_t i = 0; i < entries.size(); ++i) {
            if (i >= entryPaths.size()) continue;
            const std::string& p = entryPaths[i];
            if (p.empty()) continue;             // skip headers/categories/devices

            auto it = byPath.find(p);
            if (it != byPath.end()) {
                synced.push_back(it->second);
            } else {
                // New row that isn’t in the model yet → construct it now
                GameItem::Kind k = (i < entryKinds.size()) ? entryKinds[i] : GameItem::ISO_FILE;
                synced.push_back(makeItemFor(p, k));
            }
        }
        workingList.swap(synced);
    }
    // Patch a vector in-place from workingList times/sortKeys and keep DESC order
    void patchTimesAndResort(std::vector<GameItem>& v,
                            const std::vector<GameItem>& from /*workingList*/) {
        if (v.empty()) return;
        for (auto &x : v) {
            auto it = std::find_if(from.begin(), from.end(),
                [&](const GameItem& gi){ return gi.path == x.path; });
            if (it != from.end()) {
                x.time    = it->time;
                x.sortKey = it->sortKey;
            }
        }
        sortLikeLegacy(v); // uses sortKey DESC
    }
    void commitOrderTimestamps(){
        if (!(view == View_AllFlat || view == View_CategoryContents)) return;

        // Make sure model matches the screen order (critical for new copies/moves)
        syncWorkingListFromScreen();

        if (workingList.empty()) return;
        std::string keepPath = (selectedIndex >= 0 && selectedIndex < (int)workingList.size())
                            ? workingList[selectedIndex].path : std::string();

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
        msgBox = new MessageBox("Saving...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                popScale, 0, "", popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                popPanelW, popPanelH);
        renderOneFrame();

        ScePspDateTime startDT{}; sceRtcGetCurrentClockLocalTime(&startDT);
        unsigned long long baseTick=0; sceRtcGetTick(&startDT, &baseTick);
        const unsigned long long STEP = 10ULL * 1000000ULL;

        // Bottom row gets baseTick, then +10s per step up
        int n = (int)workingList.size();
        for (int i = n - 1; i >= 0; --i) {
            unsigned long long tick = baseTick + (unsigned long long)((n-1) - i) * STEP;
            ScePspDateTime dt{}; sceRtcSetTick(&dt, &tick);

            SceIoStat st;
            fillStatTimes(st, dt); // sets mtime/ctime/atime -> dt and zeroes the rest
            int rc = sceIoChstat(workingList[i].path.c_str(), &st, 0x08 | 0x10 | 0x20);

            if (rc < 0) {
                logInit();
                logf("START/mtime FAIL: %s rc=%d", workingList[i].path.c_str(), rc);
                logClose();
            } else {
                // also update in-memory time/sortKey so resorting is instant
                workingList[i].time    = dt;
                workingList[i].sortKey = buildLegacySortKey(dt);
            }
        }

        delete msgBox; msgBox = nullptr;

        sortLikeLegacy(workingList);
        refillRowsFromWorkingPreserveSel();
        selectByPath(keepPath);

        // --- NEW: also patch the live in-memory lists used by immediate navigation ---
        if (view == View_AllFlat) {
            // The on-screen list IS the canonical new flat order.
            flatAll = workingList;

            // Keep category lists consistent with the new times.
            patchTimesAndResort(uncategorized, workingList);
            for (auto &kv : categories) {
                patchTimesAndResort(kv.second, workingList);
            }
        } else if (view == View_CategoryContents) {
            // We saved from *within* a single category.
            if (!strcasecmp(currentCategory.c_str(), "Uncategorized")) {
                uncategorized = workingList;
            } else {
                categories[currentCategory] = workingList;
            }

            // And reflect the bumped times in the flat list too.
            patchTimesAndResort(flatAll, workingList);
        }

        // Update device cache so returning to the device keeps the saved order.
        {
            const std::string key = rootPrefix(currentDevice);
            if (!key.empty()) {
                auto &entry = deviceCache[key];
                snapshotCurrentScan(entry.snap);
                entry.dirty = false;
            }
        }

        drawMessage("Order saved", COLOR_GREEN);
        sceKernelDelayThread(700 * 1000);
    }
