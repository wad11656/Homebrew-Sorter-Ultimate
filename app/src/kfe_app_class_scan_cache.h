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
                                gi.time     = st.sce_st_ctime;
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
                        gi.time     = st.sce_st_ctime;
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

            // If the folder itself contains a PBP (EBOOT/PARAM/PBOOT), treat it as a stand-alone game (UNCATEGORIZED).
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
                gi.isUpdateDlc = isUpdateDlcFolder(gi.path);
                fillEbootIconPaths(gi);

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
                            gi.isUpdateDlc = isUpdateDlcFolder(gi.path);
                            fillEbootIconPaths(gi);

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


            // Show categories view when Game Categories is enabled OR when category folders exist
            if (!categories.empty() || gclArkOn || gclProOn) hasCategories = true;

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
                            gi.time     = st.sce_st_ctime;
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
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};

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
                        gi.isUpdateDlc = isUpdateDlcFolder(gi.path);
                        fillEbootIconPaths(gi);

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

        // Keep snapshot in sync when this refresh touches the current device
        if (!currentDevice.empty() &&
            !strcasecmp(rootPrefix(currentDevice).c_str(), rootPrefix(dev).c_str())) {
            auto &dc = deviceCache[rootPrefix(dev)];
            snapshotCurrentScan(dc.snap);
            dc.dirty = false;
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
                            gi.time     = st.sce_st_ctime;
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
                            gi.isUpdateDlc = isUpdateDlcFolder(gi.path);
                            fillEbootIconPaths(gi);

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
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};
        for (auto r : isoRoots)  refreshIso(r,   dev + std::string(r));
        for (auto r : gameRoots) refreshGame(r, dev + std::string(r));

        // Keep snapshot in sync when this refresh touches the current device
        if (!currentDevice.empty() &&
            !strcasecmp(rootPrefix(currentDevice).c_str(), rootPrefix(dev).c_str())) {
            auto &dc = deviceCache[rootPrefix(dev)];
            snapshotCurrentScan(dc.snap);
            dc.dirty = false;
        }
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

    // --- Legacy plugin support: sentinel file + PRX search helpers ---

    static std::string gclSentinelFilePath() {
        return currentExecBaseDir() + "use_legacy_categories_plugin";
    }

    static bool gclIsLegacyMode() {
        return pathExists(gclSentinelFilePath());
    }

    static void gclSetLegacyMode(bool on) {
        std::string path = gclSentinelFilePath();
        if (on) {
            SceUID fd = sceIoOpen(path.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd >= 0) sceIoClose(fd);
        } else {
            if (pathExists(path)) sceIoRemove(path.c_str());
        }
    }

    // --- Standalone blacklist file (app-level, independent of plugin mode) ---

    static std::string gclBlacklistFilePath() {
        return currentExecBaseDir() + "folder_rename_blacklist.txt";
    }

    static void gclLoadBlacklistFile() {
        gclBlacklistMap["ms0:/"].clear();
        gclBlacklistMap["ef0:/"].clear();

        std::string txt;
        if (!gclReadWholeText(gclBlacklistFilePath(), txt)) return;

        auto addUnique = [](std::vector<std::string>& list, const std::string& v) {
            for (const auto& w : list) {
                if (!strcasecmp(w.c_str(), v.c_str())) return;
            }
            list.push_back(v);
        };
        auto parseDevice = [](std::string tok, std::string& outRoot) -> bool {
            size_t a = 0, b = tok.size();
            while (a < b && (tok[a] == ' ' || tok[a] == '\t')) ++a;
            while (b > a && (tok[b-1] == ' ' || tok[b-1] == '\t')) --b;
            tok = tok.substr(a, b - a);
            if (tok.size() >= 4 && tok[3] == ':') tok = tok.substr(0, 3);
            for (char& c : tok) c = toLowerC(c);
            if (tok == "ms0") { outRoot = "ms0:/"; return true; }
            if (tok == "ef0") { outRoot = "ef0:/"; return true; }
            return false;
        };

        size_t pos = 0, start = 0;
        while (pos <= txt.size()) {
            if (pos == txt.size() || txt[pos] == '\n' || txt[pos] == '\r') {
                std::string raw = trimSpaces(txt.substr(start, pos - start));
                if (!raw.empty()) {
                    size_t comma = raw.find(',');
                    bool parsed = false;
                    if (comma != std::string::npos) {
                        std::string devTok = raw.substr(0, comma);
                        std::string val = trimSpaces(raw.substr(comma + 1));
                        std::string root;
                        if (parseDevice(devTok, root) && !val.empty()) {
                            val = normalizeBlacklistInput(val);
                            if (!val.empty()) addUnique(gclBlacklistMap[root], val);
                            parsed = true;
                        }
                    }
                    if (!parsed) {
                        std::string val = normalizeBlacklistInput(raw);
                        if (!val.empty()) {
                            addUnique(gclBlacklistMap["ms0:/"], val);
                            addUnique(gclBlacklistMap["ef0:/"], val);
                        }
                    }
                }
                if (pos < txt.size() && txt[pos] == '\r' && pos + 1 < txt.size() && txt[pos + 1] == '\n') ++pos;
                start = pos + 1;
            }
            ++pos;
        }
    }

    static bool gclSaveBlacklistFile() {
        std::string out;
        auto emit = [&](const char* dev, const std::vector<std::string>& list) {
            for (const auto& v : list) {
                if (v.empty()) continue;
                out += std::string(dev) + ", " + v + "\r\n";
            }
        };
        emit("ms0", gclBlacklistMap["ms0:/"]);
        emit("ef0", gclBlacklistMap["ef0:/"]);
        return gclWriteWholeText(gclBlacklistFilePath(), out);
    }

    // Recursive search for a PRX by exact filename (case-insensitive)
    std::string gclFindPrxByName(const std::string& sepluginsNoSlash, const char* filename) {
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
                } else if (strcasecmp(ent.d_name, filename) == 0) {
                    kfeIoCloseDir(dd);
                    return child;
                }
                memset(&ent, 0, sizeof(ent));
            }
            kfeIoCloseDir(dd);
        }
        return {};
    }

    std::string gclFindPrxByNameAny(const std::string& primaryRoot, const char* filename) {
        std::vector<std::string> dirs;
        gclCollectSepluginsDirs(primaryRoot, dirs);
        for (const auto& dir : dirs) {
            std::string found = gclFindPrxByName(dir, filename);
            if (!found.empty()) return found;
        }
        return {};
    }

    std::string gclFindCategoryLiteOldPrxAny(const std::string& primaryRoot) {
        return gclFindPrxByNameAny(primaryRoot, "category_lite_old.prx");
    }

    std::string gclFindCategoryLiteV18PrxAny(const std::string& primaryRoot) {
        return gclFindPrxByNameAny(primaryRoot, "category_lite_v1.8.prx");
    }

    static std::string gclBundledPrxPath() {
        return currentExecBaseDir() + "resources/category_lite.prx";
    }

    bool gclFilesEqual(const std::string& a, const std::string& b) {
        SceIoStat sa{}, sb{};
        if (!pathExists(a, &sa) || !pathExists(b, &sb)) return false;
        if (sa.st_size != sb.st_size) return false;
        if (sa.st_size <= 0) return true;

        SceUID fa = sceIoOpen(a.c_str(), PSP_O_RDONLY, 0);
        if (fa < 0) return false;
        SceUID fb = sceIoOpen(b.c_str(), PSP_O_RDONLY, 0);
        if (fb < 0) { sceIoClose(fa); return false; }

        char ba[16 * 1024];
        char bb[16 * 1024];
        bool same = true;
        while (same) {
            int ra = sceIoRead(fa, ba, sizeof(ba));
            int rb = sceIoRead(fb, bb, sizeof(bb));
            if (ra != rb) { same = false; break; }
            if (ra < 0) { same = false; break; }
            if (ra == 0) break;
            if (memcmp(ba, bb, (size_t)ra) != 0) { same = false; break; }
        }

        sceIoClose(fa);
        sceIoClose(fb);
        return same;
    }

    bool gclIsBundledPrxCopy(const std::string& prxPath) {
        std::string bundled = gclBundledPrxPath();
        if (!pathExists(prxPath) || !pathExists(bundled)) return false;
        return gclFilesEqual(prxPath, bundled);
    }

    std::string gclFindLegacyCategoryLitePrx(const std::string& sepluginsNoSlash) {
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
                    if (!gclIsBundledPrxCopy(child)) {
                        kfeIoCloseDir(dd);
                        return child;
                    }
                }
                memset(&ent, 0, sizeof(ent));
            }
            kfeIoCloseDir(dd);
        }
        return {};
    }

    std::string gclFindLegacyCategoryLitePrxAny(const std::string& primaryRoot) {
        std::vector<std::string> dirs;
        gclCollectSepluginsDirs(primaryRoot, dirs);
        for (const auto& dir : dirs) {
            std::string found = gclFindLegacyCategoryLitePrx(dir);
            if (!found.empty()) return found;
        }
        return {};
    }

    bool gclHasLegacyCandidatePrx(const std::string& primaryRoot) {
        if (!gclFindLegacyCategoryLitePrxAny(primaryRoot).empty()) return true;
        return !gclFindCategoryLiteOldPrxAny(primaryRoot).empty();
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
        size_t off = 0;
        while (off < out.size()) {
            int rd = sceIoRead(fd, &out[off], (uint32_t)(out.size() - off));
            if (rd < 0) { sceIoClose(fd); out.clear(); return false; }
            if (rd == 0) break;
            off += (size_t)rd;
        }
        sceIoClose(fd);
        out.resize(off);
        return !out.empty();
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

        // Don't gate enablement on PRX discovery; VSH/PLUGINS are the source of truth.
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

        // Legacy plugin mode
        gclLegacyMode = gclIsLegacyMode();
        if (!gclLegacyMode) {
            gclOldPrxPath.clear();
        } else {
            gclOldPrxPath = gclFindCategoryLiteOldPrxAny(primaryRoot);

            // Stale legacy flag check: if the only PRX is the bundled v1.8 one
            // (no old.prx, no v1.8.prx), the legacy plugin was removed externally.
            if (gclOldPrxPath.empty() && gclFindCategoryLiteV18PrxAny(primaryRoot).empty()) {
                if (havePrx) {
                    std::string baseDir = currentExecBaseDir();
                    std::string src = baseDir + "resources/category_lite.prx";
                    SceIoStat srcSt{}, curSt{};
                    if (pathExists(src, &srcSt) && pathExists(gclPrxPath, &curSt) &&
                        srcSt.st_size == curSt.st_size) {
                        // Only bundled v1.8 PRX remains — clear stale legacy flag
                        gclLegacyMode = false;
                        gclSetLegacyMode(false);
                    }
                }
            }
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

    bool gclFileHasEnabledEntry(const std::string& filePath, bool arkPluginsTxt){
        if (filePath.empty()) return false;
        std::string txt;
        if (!gclReadWholeText(filePath, txt)) return false;

        size_t pos=0, s=0;
        while (pos<=txt.size()){
            if (pos==txt.size() || txt[pos]=='\n' || txt[pos]=='\r'){
                std::string line = txt.substr(s, pos-s);
                if (gclLineEnables(line, arkPluginsTxt)) return true;
                if (pos+1<txt.size() && txt[pos]=='\r' && txt[pos+1]=='\n') ++pos;
                s = pos + 1;
            }
            ++pos;
        }
        return false;
    }

    bool gclWriteEnableToFileWithVerify(const std::string& filePath, bool enable, bool arkPluginsTxt, int maxAttempts=2){
        if (filePath.empty()) return false;
        if (maxAttempts < 1) maxAttempts = 1;

        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            if (!gclWriteEnableToFile(filePath, enable, arkPluginsTxt)) {
                sceKernelDelayThread(8 * 1000);
                continue;
            }

            const bool isEnabled = gclFileHasEnabledEntry(filePath, arkPluginsTxt);
            if (isEnabled == enable) return true;
            sceKernelDelayThread(8 * 1000);
        }
        return false;
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

    static std::string gclFiltersRoot() {
        if (isPspGo() && DeviceExists("ef0:/")) return "ef0:/";
        if (DeviceExists("ms0:/")) return "ms0:/";
        if (DeviceExists("ef0:/")) return "ef0:/";
        return "ms0:/";
    }

    static std::string gclFiltersPath() {
        return gclFiltersRoot() + "seplugins/gclite_filter.txt";
    }

    void scrubHiddenAppFiltersOnStartup() {
        if (gclFiltersScrubbed) return;
        gclFiltersScrubbed = true;

        gclLoadUnifiedFilters();
        bool changed = false;

        auto scrubForRoot = [&](const std::string& dev){
            if (dev.empty() || !DeviceExists(dev.c_str())) return;
            const std::string root = gclFilterRootKeyFor(dev);
            auto& fl = gclGameFilterMap[root];
            for (auto it = fl.begin(); it != fl.end(); ) {
                if (it->empty()) { it = fl.erase(it); changed = true; continue; }
                std::string full = dev.substr(0, 4) + *it; // "ms0:" + "/path"
                if (!pathExists(full)) { it = fl.erase(it); changed = true; }
                else ++it;
            }
        };

        scrubForRoot("ms0:/");
        scrubForRoot("ef0:/");

        if (changed) gclSaveUnifiedFilters();
    }

    static std::string normalizeGameFilterPath(const std::string& raw) {
        std::string t = trimSpaces(raw);
        if (t.empty()) return {};
        for (char& c : t) if (c == '\\') c = '/';
        if (t.size() >= 4 && t[3] == ':') t = t.substr(4);
        if (!t.empty() && t[0] != '/') t = std::string("/") + t;
        return t;
    }

    static bool updateGameFilterEntryExact(std::vector<std::string>& list,
                                           const std::string& oldNorm,
                                           const std::string& newNorm) {
        if (oldNorm.empty() || newNorm.empty() || oldNorm == newNorm) return false;
        bool changed = false;
        for (auto& w : list) {
            if (!strcasecmp(w.c_str(), oldNorm.c_str())) {
                w = newNorm;
                changed = true;
            }
        }
        if (changed) {
            std::vector<std::string> dedup;
            dedup.reserve(list.size());
            for (const auto& w : list) {
                bool dup = false;
                for (const auto& d : dedup) { if (!strcasecmp(d.c_str(), w.c_str())) { dup = true; break; } }
                if (!dup) dedup.push_back(w);
            }
            list.swap(dedup);
        }
        return changed;
    }

    static bool updateGameFilterEntryPrefix(std::vector<std::string>& list,
                                            const std::string& oldPrefix,
                                            const std::string& newPrefix) {
        if (oldPrefix.empty() || newPrefix.empty() || oldPrefix == newPrefix) return false;
        bool changed = false;
        for (auto& w : list) {
            if (w.size() < oldPrefix.size()) continue;
            if (strncasecmp(w.c_str(), oldPrefix.c_str(), oldPrefix.size()) != 0) continue;
            if (w.size() > oldPrefix.size() && w[oldPrefix.size()] != '/') continue;
            w = newPrefix + w.substr(oldPrefix.size());
            changed = true;
        }
        if (changed) {
            std::vector<std::string> dedup;
            dedup.reserve(list.size());
            for (const auto& w : list) {
                bool dup = false;
                for (const auto& d : dedup) { if (!strcasecmp(d.c_str(), w.c_str())) { dup = true; break; } }
                if (!dup) dedup.push_back(w);
            }
            list.swap(dedup);
        }
        return changed;
    }

    static bool removeGameFilterEntryExact(std::vector<std::string>& list,
                                           const std::string& key) {
        if (key.empty()) return false;
        bool changed = false;
        for (auto it = list.begin(); it != list.end(); ) {
            if (!strcasecmp(it->c_str(), key.c_str())) { it = list.erase(it); changed = true; }
            else ++it;
        }
        return changed;
    }

    static bool removeGameFilterEntriesByPrefix(std::vector<std::string>& list,
                                                const std::string& prefix) {
        if (prefix.empty()) return false;
        bool changed = false;
        for (auto it = list.begin(); it != list.end(); ) {
            if (it->size() < prefix.size()) { ++it; continue; }
            if (strncasecmp(it->c_str(), prefix.c_str(), prefix.size()) != 0) { ++it; continue; }
            if (it->size() > prefix.size() && (*it)[prefix.size()] != '/') { ++it; continue; }
            it = list.erase(it);
            changed = true;
        }
        return changed;
    }

    static bool updateListEntryCaseInsensitive(std::vector<std::string>& list,
                                               const std::string& oldValue,
                                               const std::string& newValue) {
        if (oldValue.empty() || newValue.empty()) return false;
        if (oldValue == newValue) return false;
        bool had = false;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (!strcasecmp(it->c_str(), oldValue.c_str())) {
                list.erase(it);
                had = true;
                break;
            }
        }
        if (!had) return false;
        for (const auto& w : list) {
            if (!strcasecmp(w.c_str(), newValue.c_str())) return true;
        }
        list.push_back(newValue);
        return true;
    }

    static bool replaceCaseInsensitiveEntryWithExact(std::vector<std::string>& list,
                                                     const std::string& value) {
        if (value.empty()) return false;
        int matches = 0;
        bool hasExact = false;
        for (const auto& w : list) {
            if (!strcasecmp(w.c_str(), value.c_str())) {
                ++matches;
                if (w == value) hasExact = true;
            }
        }
        if (matches == 0) return false;
        if (matches == 1 && hasExact) return false;

        for (auto it = list.begin(); it != list.end(); ) {
            if (!strcasecmp(it->c_str(), value.c_str())) it = list.erase(it);
            else ++it;
        }
        list.push_back(value);
        return true;
    }

    static bool removeListEntryCaseInsensitive(std::vector<std::string>& list,
                                               const std::string& value) {
        if (value.empty()) return false;
        bool changed = false;
        for (auto it = list.begin(); it != list.end(); ) {
            if (!strcasecmp(it->c_str(), value.c_str())) { it = list.erase(it); changed = true; }
            else ++it;
        }
        return changed;
    }

    static bool updateHiddenAppPathsForFolderRename(const std::string& absRoot,
                                                    const std::string& fromName,
                                                    const std::string& toName) {
        if (fromName.empty() || toName.empty()) return false;
        if (fromName == toName) return false;
        const std::string devRoot = rootPrefix(absRoot);
        if (devRoot.empty()) return false;

        gclLoadUnifiedFilters();
        const std::string filterRoot = gclFilterRootKeyFor(devRoot);
        auto& fl = gclGameFilterMap[filterRoot];

        std::string oldPrefix = normalizeGameFilterPath(joinDirFile(absRoot, fromName.c_str()));
        std::string newPrefix = normalizeGameFilterPath(joinDirFile(absRoot, toName.c_str()));
        if (oldPrefix.empty() || newPrefix.empty()) return false;

        bool changed = updateGameFilterEntryPrefix(fl, oldPrefix, newPrefix);
        if (changed) gclSaveUnifiedFilters();
        return changed;
    }

    bool updateGameFilterOnItemRename(const std::string& oldPath, const std::string& newPath) {
        if (gclLegacyMode) {
            const std::string oldName = extractGameFolderName(oldPath);
            const std::string newName = extractGameFolderName(newPath);
            if (oldName.empty() || newName.empty()) return false;
            if (oldName == newName) return false;

            gclLoadLegacyFilterCache();
            std::vector<std::string> names = gclLegacyFilterCache;
            bool changed = false;
            for (size_t i = 0; i < names.size(); ++i) {
                if (names[i] == oldName) {
                    names[i] = newName;
                    changed = true;
                }
            }
            if (!changed) return false;

            // Keep case-sensitive uniqueness in legacy list.
            std::vector<std::string> dedup;
            dedup.reserve(names.size());
            for (const auto& n : names) {
                bool dup = false;
                for (const auto& d : dedup) {
                    if (d == n) { dup = true; break; }
                }
                if (!dup) dedup.push_back(n);
            }

            gclWriteLegacyFilter(dedup);
            gclLegacyFilterCache = dedup;
            gclLegacyFilterLoaded = true;
            return true;
        }

        std::string oldNorm = normalizeGameFilterPath(oldPath);
        std::string newNorm = normalizeGameFilterPath(newPath);
        if (oldNorm.empty() || newNorm.empty()) return false;
        if (oldNorm == newNorm) return false;

        const std::string oldRoot = rootPrefix(oldPath);
        const std::string newRoot = rootPrefix(newPath);
        if (oldRoot.empty()) return false;

        gclLoadUnifiedFilters();
        const std::string oldFilterRoot = gclFilterRootKeyFor(oldRoot);
        auto& oldList = gclGameFilterMap[oldFilterRoot];

        bool changed = false;
        if (newRoot.empty() || !strcasecmp(oldRoot.c_str(), newRoot.c_str())) {
            changed = updateGameFilterEntryExact(oldList, oldNorm, newNorm);
            changed |= replaceCaseInsensitiveEntryWithExact(oldList, newNorm);
        } else {
            bool found = removeGameFilterEntryExact(oldList, oldNorm);
            if (found) {
                const std::string newFilterRoot = gclFilterRootKeyFor(newRoot);
                auto& newList = gclGameFilterMap[newFilterRoot];
                bool dup = false;
                for (const auto& w : newList) { if (!strcasecmp(w.c_str(), newNorm.c_str())) { dup = true; break; } }
                if (!dup) newList.push_back(newNorm);
                else changed |= replaceCaseInsensitiveEntryWithExact(newList, newNorm);
                changed = true;
            }
        }
        if (changed) gclSaveUnifiedFilters();
        return changed;
    }

    void removeHiddenAppFiltersForPaths(const std::vector<std::string>& paths) {
        if (paths.empty()) return;
        gclLoadUnifiedFilters();
        bool changed = false;
        for (const auto& p : paths) {
            std::string key = normalizeGameFilterPath(p);
            if (key.empty()) continue;
            std::string dev = rootPrefix(p);
            if (dev.empty()) dev = currentDevice;
            const std::string root = gclFilterRootKeyFor(dev);
            changed |= removeGameFilterEntryExact(gclGameFilterMap[root], key);
        }
        if (changed) gclSaveUnifiedFilters();
    }

    void removeHiddenFiltersForDeletedCategory(const std::string& displayName) {
        if (displayName.empty()) return;
        std::string base = stripCategoryPrefixes(displayName);
        if (base.empty()) return;
        gclLoadUnifiedFilters();

        const std::string root = gclFilterRootKeyFor(currentDevice);
        bool changed = false;

        changed |= removeListEntryCaseInsensitive(gclCategoryFilterMap[root], base);

        const char* roots[] = {"ISO/","PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};
        for (auto r : roots) {
            std::string abs = currentDevice + std::string(r);
            std::string prefix = normalizeGameFilterPath(joinDirFile(abs, displayName.c_str()));
            if (!prefix.empty()) {
                changed |= removeGameFilterEntriesByPrefix(gclGameFilterMap[root], prefix);
            }
        }

        if (changed) gclSaveUnifiedFilters();
    }

    static void gclLoadUnifiedFilters() {
        if (gclFiltersLoaded) return;
        gclFiltersLoaded = true;

        gclBlacklistMap["ms0:/"].clear();
        gclBlacklistMap["ef0:/"].clear();
        gclCategoryFilterMap["ms0:/"].clear();
        gclCategoryFilterMap["ef0:/"].clear();
        gclGameFilterMap["ms0:/"].clear();
        gclGameFilterMap["ef0:/"].clear();

        std::string txt;
        if (!gclReadWholeText(gclFiltersPath(), txt)) return;

        enum Section { SEC_None, SEC_Blacklist, SEC_HiddenCats, SEC_HiddenApps };
        Section sec = SEC_None;

        auto parseDeviceToken = [&](std::string tok, std::string& outRoot)->bool {
            tok = trimSpaces(tok);
            if (tok.size() >= 4 && tok[3] == ':') tok = tok.substr(0, 3);
            for (char& c : tok) c = toLowerC(c);
            if (tok == "ms0") { outRoot = "ms0:/"; return true; }
            if (tok == "ef0") { outRoot = "ef0:/"; return true; }
            return false;
        };

        auto addUniqueCaseInsensitive = [&](std::vector<std::string>& list, const std::string& v){
            for (const auto& w : list) {
                if (!strcasecmp(w.c_str(), v.c_str())) return;
            }
            list.push_back(v);
        };

        auto addUniqueExact = [&](std::vector<std::string>& list, const std::string& v){
            for (const auto& w : list) {
                if (!strcasecmp(w.c_str(), v.c_str())) return;
            }
            list.push_back(v);
        };

        size_t pos = 0, start = 0;
        while (pos <= txt.size()) {
            if (pos == txt.size() || txt[pos] == '\n' || txt[pos] == '\r') {
                std::string raw = trimSpaces(txt.substr(start, pos - start));
                if (!raw.empty()) {
                    std::string low = raw;
                    for (char& c : low) c = toLowerC(c);
                    if (low.rfind("===", 0) == 0 && low.size() >= 6 && low.find("===") != std::string::npos) {
                        if (low == "===categories rename blacklist===") sec = SEC_Blacklist;
                        else if (low == "===hidden categories===") sec = SEC_HiddenCats;
                        else if (low == "===hidden apps===") sec = SEC_HiddenApps;
                        else sec = SEC_None;
                    } else if (sec == SEC_Blacklist || sec == SEC_HiddenCats) {
                        size_t comma = raw.find(',');
                        bool parsed = false;
                        if (comma != std::string::npos) {
                            std::string devTok = raw.substr(0, comma);
                            std::string val = trimSpaces(raw.substr(comma + 1));
                            std::string root;
                            if (parseDeviceToken(devTok, root) && !val.empty()) {
                                parsed = true;
                                if (sec == SEC_Blacklist) {
                                    val = normalizeBlacklistInput(val);
                                    if (!val.empty()) addUniqueCaseInsensitive(gclBlacklistMap[root], val);
                                } else {
                                    val = stripCategoryPrefixes(val);
                                    if (!val.empty()) addUniqueCaseInsensitive(gclCategoryFilterMap[root], val);
                                }
                            }
                        } else if (raw.size() >= 4 && (raw[3] == ':' || raw[3] == ' ' || raw[3] == '\t')) {
                            std::string devTok = raw.substr(0, 3);
                            std::string val = trimSpaces(raw.substr(4));
                            std::string root;
                            if (parseDeviceToken(devTok, root) && !val.empty()) {
                                parsed = true;
                                if (sec == SEC_Blacklist) {
                                    val = normalizeBlacklistInput(val);
                                    if (!val.empty()) addUniqueCaseInsensitive(gclBlacklistMap[root], val);
                                } else {
                                    val = stripCategoryPrefixes(val);
                                    if (!val.empty()) addUniqueCaseInsensitive(gclCategoryFilterMap[root], val);
                                }
                            }
                        }
                        if (!parsed) {
                            std::string val = raw;
                            if (sec == SEC_Blacklist) {
                                val = normalizeBlacklistInput(val);
                                if (!val.empty()) {
                                    addUniqueCaseInsensitive(gclBlacklistMap["ms0:/"], val);
                                    addUniqueCaseInsensitive(gclBlacklistMap["ef0:/"], val);
                                }
                            } else {
                                val = stripCategoryPrefixes(val);
                                if (!val.empty()) {
                                    addUniqueCaseInsensitive(gclCategoryFilterMap["ms0:/"], val);
                                    addUniqueCaseInsensitive(gclCategoryFilterMap["ef0:/"], val);
                                }
                            }
                        }
                    } else if (sec == SEC_HiddenApps) {
                        std::string devTok, pathPart;
                        size_t comma = raw.find(',');
                        if (comma != std::string::npos) {
                            devTok = raw.substr(0, comma);
                            pathPart = trimSpaces(raw.substr(comma + 1));
                        } else if (raw.size() >= 4 && raw[3] == ':') {
                            devTok = raw.substr(0, 3);
                            pathPart = raw;
                        }
                        std::string root;
                        if (!devTok.empty() && parseDeviceToken(devTok, root)) {
                            std::string norm = normalizeGameFilterPath(pathPart);
                            if (!norm.empty()) addUniqueExact(gclGameFilterMap[root], norm);
                        } else if (devTok.empty()) {
                            std::string norm = normalizeGameFilterPath(raw);
                            if (!norm.empty()) {
                                addUniqueExact(gclGameFilterMap["ms0:/"], norm);
                                addUniqueExact(gclGameFilterMap["ef0:/"], norm);
                            }
                        }
                    }
                }
                if (pos + 1 < txt.size() && txt[pos] == '\r' && txt[pos+1] == '\n') ++pos;
                start = pos + 1;
            }
            ++pos;
        }

        // Blacklist lives in its own file in the EBOOT folder.
        // If the standalone file exists, it's authoritative; overwrite anything
        // parsed from gclite_filter.txt above.  If not, any entries we just
        // parsed from gclite_filter.txt serve as a one-time migration and we
        // write them out to the new file.
        if (pathExists(gclBlacklistFilePath())) {
            gclLoadBlacklistFile();
        } else if (!gclBlacklistMap["ms0:/"].empty() || !gclBlacklistMap["ef0:/"].empty()) {
            gclSaveBlacklistFile();  // one-time migration
        }
    }

    static bool gclSaveUnifiedFilters() {
        if (!gclFiltersLoaded) gclLoadUnifiedFilters();

        // Blacklist always goes to its own file in the EBOOT folder
        gclSaveBlacklistFile();

        // Legacy mode: gclite_filter.txt is in legacy format, don't overwrite
        if (gclIsLegacyMode()) return true;

        std::string root = gclFiltersRoot();
        std::string dir = root + "seplugins";
        sceIoMkdir(dir.c_str(), 0777);
        std::string path = gclFiltersPath();

        std::string existing;
        if (gclReadWholeText(path, existing)) {
            if (existing.find("===") == std::string::npos) {
                std::string oldPath = root + "seplugins/gclite_filter_old.txt";
                if (!pathExists(oldPath)) {
                    sceIoRename(path.c_str(), oldPath.c_str());
                }
            }
        }

        auto emitDeviceList = [&](const char* label,
                                  const std::vector<std::string>& ms,
                                  const std::vector<std::string>& ef,
                                  bool isPath){
            std::string out;
            out += std::string("===") + label + "===\r\n";
            auto emit = [&](const char* dev, const std::vector<std::string>& list){
                for (const auto& v : list) {
                    if (v.empty()) continue;
                    if (isPath) out += std::string(dev) + ":" + v;
                    else out += std::string(dev) + ", " + v;
                    out += "\r\n";
                }
            };
            emit("ms0", ms);
            emit("ef0", ef);
            return out;
        };

        const auto& catMs = gclCategoryFilterMap["ms0:/"];
        const auto& catEf = gclCategoryFilterMap["ef0:/"];
        const auto& appMs = gclGameFilterMap["ms0:/"];
        const auto& appEf = gclGameFilterMap["ef0:/"];

        std::string out;
        out += emitDeviceList("HIDDEN CATEGORIES", catMs, catEf, false);
        out += emitDeviceList("HIDDEN APPS", appMs, appEf, true);

        return gclWriteWholeText(path, out);
    }

    static void gclLoadCategoryFilterFor(const std::string& dev) {
        (void)dev;
        gclLoadUnifiedFilters();
    }

    static bool gclSaveCategoryFilterFor(const std::string& dev,
                                         const std::unordered_map<std::string, std::string>& baseToDisplay) {
        (void)dev;
        (void)baseToDisplay;
        return gclSaveUnifiedFilters();
    }

    static void gclLoadGameFilterFor(const std::string& dev) {
        (void)dev;
        gclLoadUnifiedFilters();
    }

    static bool gclSaveGameFilterFor(const std::string& dev) {
        (void)dev;
        return gclSaveUnifiedFilters();
    }

    static void gclLoadBlacklistFor(const std::string& dev) {
        (void)dev;
        gclLoadUnifiedFilters();
    }

    static bool gclSaveBlacklistFor(const std::string& dev) {
        (void)dev;
        return gclSaveUnifiedFilters();
    }

    // ---------------------------------------------------------------
    // Legacy v1.6/v1.7 filter support
    // ---------------------------------------------------------------

    // Extract the game-folder name from a full PSP path.
    // e.g. "ms0:/PSP/GAME/MyApp/EBOOT.PBP" → "MyApp"
    //      "/psp/game/cat_action/myapp"      → "myapp"
    //      "ms0:/PSP/GAME/MyApp"             → "MyApp"
    static std::string extractGameFolderName(const std::string& rawPath) {
        std::string p = rawPath;
        // Strip device prefix
        if (p.size() >= 4 && p[3] == ':') p = p.substr(4);
        for (char& c : p) if (c == '\\') c = '/';
        // Remove trailing slash
        while (!p.empty() && p.back() == '/') p.pop_back();
        if (p.empty()) return {};
        // If it ends with a known file (EBOOT.PBP, PBOOT.PBP, etc.), go up one level
        std::string last = p;
        size_t sl = p.rfind('/');
        if (sl != std::string::npos) last = p.substr(sl + 1);
        auto endsWithCI = [](const std::string& s, const char* suffix) {
            size_t slen = strlen(suffix);
            if (s.size() < slen) return false;
            return strcasecmp(s.c_str() + s.size() - slen, suffix) == 0;
        };
        if (endsWithCI(last, ".pbp") || endsWithCI(last, ".prx") || endsWithCI(last, ".rif")) {
            if (sl != std::string::npos) p = p.substr(0, sl);
        }
        // Now get the last component
        sl = p.rfind('/');
        if (sl != std::string::npos) return p.substr(sl + 1);
        return p;
    }

    // Read a legacy (v1.6/v1.7) gclite_filter.txt: one folder name per line
    // Extract a bare folder name from a line that may be a simple name,
    // a full path, or a device-prefixed path.  Returns empty for === headers.
    static bool hasBinaryLikeBytes(const std::string& s) {
        for (unsigned char ch : s) {
            if (ch == '\0') return true;
            if (ch < 0x20 && ch != '\t' && ch != '\r' && ch != '\n') return true;
        }
        return false;
    }

    static std::string extractLegacyFolderName(const std::string& line) {
        if (line.empty()) return {};
        if (hasBinaryLikeBytes(line)) return {};
        if (line.rfind("===", 0) == 0) return {};  // skip section headers

        // Already a bare folder name?
        if (line.find('/') == std::string::npos &&
            line.find('\\') == std::string::npos &&
            line.find(':') == std::string::npos) return line;

        // Has path separators or device prefix — extract last component
        std::string name = extractGameFolderName(line);
        // Reject if extractGameFolderName returned something that still looks like a header
        if (!name.empty() && name.rfind("===", 0) == 0) return {};
        return name;
    }

    static std::vector<std::string> gclReadLegacyFilterFile(const std::string& filePath) {
        std::vector<std::string> names;
        std::string txt;
        if (!gclReadWholeText(filePath, txt)) return names;
        size_t pos = 0, start = 0;
        while (pos <= txt.size()) {
            if (pos == txt.size() || txt[pos] == '\n' || txt[pos] == '\r') {
                std::string raw = trimSpaces(txt.substr(start, pos - start));
                std::string name = extractLegacyFolderName(raw);
                if (!name.empty()) {
                    // Deduplicate (case-sensitive)
                    bool dup = false;
                    for (const auto& existing : names) {
                        if (existing == name) { dup = true; break; }
                    }
                    if (!dup) names.push_back(name);
                }
                if (pos + 1 < txt.size() && txt[pos] == '\r' && txt[pos + 1] == '\n') ++pos;
                start = pos + 1;
            }
            ++pos;
        }
        return names;
    }

    static std::vector<std::string> gclReadLegacyFilter() {
        return gclReadLegacyFilterFile(gclFiltersPath());
    }

    // Write a legacy gclite_filter.txt: one folder name per line
    static bool gclWriteLegacyFilter(const std::vector<std::string>& names) {
        std::string out;
        for (const auto& n : names) {
            std::string clean = extractLegacyFolderName(n);
            if (clean.empty()) continue;
            out += clean + "\r\n";
        }
        std::string root = gclFiltersRoot();
        std::string dir = root + "seplugins";
        sceIoMkdir(dir.c_str(), 0777);
        return gclWriteWholeText(gclFiltersPath(), out);
    }

    // Ensure legacy filter cache is loaded
    void gclLoadLegacyFilterCache() {
        if (gclLegacyFilterLoaded) return;
        gclLegacyFilterCache = gclReadLegacyFilter();
        gclLegacyFilterLoaded = true;
    }

    // Check if a folder name is in the legacy filter (uses cache)
    bool isGameFilteredLegacy(const std::string& path) {
        std::string folderName = extractGameFolderName(path);
        if (folderName.empty()) return false;
        gclLoadLegacyFilterCache();
        for (const auto& n : gclLegacyFilterCache) {
            if (n == folderName) return true;
        }
        return false;
    }

    // Hide/unhide in legacy mode: add/remove the folder name
    void setGameHiddenLegacy(const std::vector<std::string>& paths, bool hide) {
        if (paths.empty()) return;
        gclLoadLegacyFilterCache();
        std::vector<std::string> names = gclLegacyFilterCache;
        for (const auto& p : paths) {
            std::string folderName = extractGameFolderName(p);
            if (folderName.empty()) continue;
            if (hide) {
                bool exists = false;
                for (const auto& n : names) {
                    if (n == folderName) { exists = true; break; }
                }
                if (!exists) names.push_back(folderName);
            } else {
                for (auto it = names.begin(); it != names.end(); ) {
                    if (*it == folderName) it = names.erase(it);
                    else ++it;
                }
            }
        }
        gclWriteLegacyFilter(names);
        gclLegacyFilterCache = names;  // update cache in place
    }

    // Search for game folders matching a name across both devices.
    // Scope is intentionally bounded to PSP layout depth:
    //   - /PSP/GAME/<App>
    //   - /PSP/GAME/<Category>/<App>
    // Matching is case-insensitive, and only folders that look like game folders
    // (contain EBOOT/PBOOT/PARAM) are returned.
    // Returns full paths (e.g. "ms0:/PSP/GAME/MyApp", "ef0:/PSP/GAME/CAT_Action/MyApp").
    std::vector<std::string> gclFindGameFoldersByName(const std::string& folderName) {
        std::vector<std::string> results;
        if (folderName.empty()) return results;

        auto pushUnique = [&](const std::string& p) {
            for (const auto& e : results) {
                if (!strcasecmp(e.c_str(), p.c_str())) return;
            }
            results.push_back(p);
        };

        auto searchDevice = [&](const char* root) {
            if (!DeviceExists(root)) return;
            std::string gameDir = std::string(root) + "PSP/GAME";
            if (!dirExists(gameDir)) return;
            SceUID d = kfeIoOpenDir(gameDir.c_str());
            if (d < 0) return;

            SceIoDirent ent; memset(&ent, 0, sizeof(ent));
            while (kfeIoReadDir(d, &ent) > 0) {
                trimTrailingSpaces(ent.d_name);
                if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent, 0, sizeof(ent)); continue; }
                if (!FIO_S_ISDIR(ent.d_stat.st_mode)) { memset(&ent, 0, sizeof(ent)); continue; }

                std::string child = joinDirFile(gameDir, ent.d_name);
                const bool childIsGameFolder = !findEbootCaseInsensitive(child).empty();

                // /PSP/GAME/<App>
                if (!strcasecmp(ent.d_name, folderName.c_str()) && childIsGameFolder) {
                    pushUnique(child);
                }

                // /PSP/GAME/<Category>/<App> (one level only)
                if (!childIsGameFolder) {
                    SceUID d2 = kfeIoOpenDir(child.c_str());
                    if (d2 >= 0) {
                        SceIoDirent ent2; memset(&ent2, 0, sizeof(ent2));
                        while (kfeIoReadDir(d2, &ent2) > 0) {
                            trimTrailingSpaces(ent2.d_name);
                            if (!strcmp(ent2.d_name, ".") || !strcmp(ent2.d_name, "..")) { memset(&ent2, 0, sizeof(ent2)); continue; }
                            if (!FIO_S_ISDIR(ent2.d_stat.st_mode)) { memset(&ent2, 0, sizeof(ent2)); continue; }

                            if (!strcasecmp(ent2.d_name, folderName.c_str())) {
                                std::string appPath = joinDirFile(child, ent2.d_name);
                                if (!findEbootCaseInsensitive(appPath).empty()) {
                                    pushUnique(appPath);
                                }
                            }
                            memset(&ent2, 0, sizeof(ent2));
                        }
                        kfeIoCloseDir(d2);
                    }
                }
                memset(&ent, 0, sizeof(ent));
            }
            kfeIoCloseDir(d);
        };

        searchDevice("ms0:/");
        searchDevice("ef0:/");
        return results;
    }

    // Parse ===HIDDEN APPS=== entries from a v1.8-format file text
    static std::vector<std::pair<std::string, std::string>> gclParseV18HiddenApps(const std::string& txt) {
        // Returns pairs of (device root, normalized path)
        std::vector<std::pair<std::string, std::string>> results;
        bool inHiddenApps = false;
        size_t pos = 0, start = 0;
        while (pos <= txt.size()) {
            if (pos == txt.size() || txt[pos] == '\n' || txt[pos] == '\r') {
                std::string raw = trimSpaces(txt.substr(start, pos - start));
                if (!raw.empty()) {
                    std::string low = raw;
                    for (char& c : low) c = toLowerC(c);
                    if (low.rfind("===", 0) == 0) {
                        inHiddenApps = (low == "===hidden apps===");
                    } else if (inHiddenApps) {
                        // Parse device + path
                        std::string devTok, pathPart;
                        size_t comma = raw.find(',');
                        if (comma != std::string::npos) {
                            devTok = raw.substr(0, comma);
                            pathPart = trimSpaces(raw.substr(comma + 1));
                        } else if (raw.size() >= 4 && raw[3] == ':') {
                            devTok = raw.substr(0, 3);
                            pathPart = raw;
                        }
                        std::string root;
                        auto parseDevTok = [](std::string tok, std::string& r) -> bool {
                            for (char& c : tok) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                            if (tok.size() >= 4 && tok[3] == ':') tok = tok.substr(0, 3);
                            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
                            if (tok == "ms0") { r = "ms0:/"; return true; }
                            if (tok == "ef0") { r = "ef0:/"; return true; }
                            return false;
                        };
                        if (!devTok.empty() && parseDevTok(devTok, root)) {
                            std::string norm = normalizeGameFilterPath(pathPart);
                            if (!norm.empty()) results.push_back({root, norm});
                        } else if (devTok.empty()) {
                            std::string norm = normalizeGameFilterPath(raw);
                            if (!norm.empty()) {
                                results.push_back({"ms0:/", norm});
                                results.push_back({"ef0:/", norm});
                            }
                        }
                    }
                }
                if (pos + 1 < txt.size() && txt[pos] == '\r' && txt[pos + 1] == '\n') ++pos;
                start = pos + 1;
            }
            ++pos;
        }
        return results;
    }

    // Bidirectional merge between legacy and v1.8 filter files
    void gclMergeLegacyFilters(const std::string& sourceFile, const std::string& destFile, bool toLegacy) {
        if (toLegacy) {
            // Source = v1.8 format → extract folder names → merge into legacy dest
            std::string srcTxt;
            if (!gclReadWholeText(sourceFile, srcTxt)) return;
            auto hiddenApps = gclParseV18HiddenApps(srcTxt);

            // Collect folder names from v1.8 paths
            std::vector<std::string> newFolderNames;
            for (const auto& entry : hiddenApps) {
                std::string name = extractGameFolderName(entry.second);
                if (name.empty() || name.rfind("===", 0) == 0) continue;
                newFolderNames.push_back(name);
            }

            // Read existing legacy entries (already validated by gclReadLegacyFilterFile)
            std::vector<std::string> legacyNames = gclReadLegacyFilterFile(destFile);

            // Add missing names
            for (const auto& name : newFolderNames) {
                bool exists = false;
                for (const auto& existing : legacyNames) {
                    if (existing == name) { exists = true; break; }
                }
                if (!exists) legacyNames.push_back(name);
            }

            // Write back (only valid folder names)
            std::string out;
            for (const auto& n : legacyNames) {
                std::string cleanN = extractLegacyFolderName(n);
                if (cleanN.empty()) continue;
                out += cleanN + "\r\n";
            }
            gclWriteWholeText(destFile, out);
        } else {
            // Source = legacy format → resolve to full paths → merge into v1.8 dest
            std::vector<std::string> legacyNames = gclReadLegacyFilterFile(sourceFile);
            if (legacyNames.empty()) return;

            // Force-reload the v1.8 filter maps so we have current state
            gclFiltersLoaded = false; gclLegacyFilterLoaded = false;

            // If dest doesn't exist yet, create an empty v1.8 file
            if (!pathExists(destFile)) {
                gclWriteWholeText(destFile, "===HIDDEN CATEGORIES===\r\n===HIDDEN APPS===\r\n");
            }

            gclLoadUnifiedFilters();

            for (const auto& name : legacyNames) {
                if (name.empty() || name.rfind("===", 0) == 0) continue;
                std::vector<std::string> found = gclFindGameFoldersByName(name);
                if (found.empty()) {
                    // Fallback: add as /PSP/GAME/<name> for both devices
                    std::string fallback = normalizeGameFilterPath("/PSP/GAME/" + name);
                    if (!fallback.empty()) {
                        auto addUnique = [](std::vector<std::string>& list, const std::string& v) {
                            for (const auto& w : list) if (!strcasecmp(w.c_str(), v.c_str())) return;
                            list.push_back(v);
                        };
                        addUnique(gclGameFilterMap["ms0:/"], fallback);
                        addUnique(gclGameFilterMap["ef0:/"], fallback);
                    }
                } else {
                    for (const auto& fullPath : found) {
                        std::string root = rootPrefix(fullPath);
                        if (root.empty()) root = "ms0:/";
                        std::string norm = normalizeGameFilterPath(fullPath);
                        if (norm.empty()) continue;
                        auto addUnique = [](std::vector<std::string>& list, const std::string& v) {
                            for (const auto& w : list) if (!strcasecmp(w.c_str(), v.c_str())) return;
                            list.push_back(v);
                        };
                        addUnique(gclGameFilterMap[root], norm);
                    }
                }
            }

            gclSaveUnifiedFilters();
        }
    }

    static bool gclLooksLikeV18FilterText(const std::string& txt) {
        return txt.find("===HIDDEN CATEGORIES===") != std::string::npos ||
               txt.find("===HIDDEN APPS===") != std::string::npos;
    }

    bool gclLooksLikeV18FilterFile(const std::string& path) {
        std::string txt;
        if (!gclReadWholeText(path, txt)) return false;
        return gclLooksLikeV18FilterText(txt);
    }

    // In non-legacy mode, guarantee gclite_filter.txt is sectioned v1.8 format.
    // If the file is legacy/headerless (or malformed), salvage names and rewrite.
    void gclRepairV18FilterFileIfNeeded(const std::string& path) {
        if (path.empty() || gclLegacyMode) return;
        if (gclLooksLikeV18FilterFile(path)) return;

        std::string originalTxt;
        gclReadWholeText(path, originalTxt);
        std::vector<std::string> legacyNames = gclReadLegacyFilterFile(path);

        // Keep a legacy snapshot when converting the active filter in non-legacy mode.
        // This preserves user data for later legacy-toggle restores.
        const std::string activeFilter = gclFiltersPath();
        if (!strcasecmp(path.c_str(), activeFilter.c_str())) {
            std::string oldPath = gclFiltersRoot() + "seplugins/gclite_filter_old.txt";
            if (!pathExists(oldPath) && !originalTxt.empty()) {
                gclWriteWholeText(oldPath, originalTxt);
            }
        }

        gclWriteWholeText(path, "===HIDDEN CATEGORIES===\r\n===HIDDEN APPS===\r\n");

        if (!legacyNames.empty()) {
            std::string root = gclFiltersRoot();
            std::string dir = root + "seplugins";
            sceIoMkdir(dir.c_str(), 0777);
            std::string repairLegacy = joinDirFile(dir, "gclite_filter_repair_legacy.txt");

            std::string out;
            for (const auto& n : legacyNames) {
                std::string clean = extractLegacyFolderName(n);
                if (clean.empty()) continue;
                out += clean + "\r\n";
            }
            if (!out.empty()) {
                gclWriteWholeText(repairLegacy, out);
                gclFiltersLoaded = false; gclLegacyFilterLoaded = false;
                gclMergeLegacyFilters(repairLegacy, path, /*toLegacy=*/false);
                sceIoRemove(repairLegacy.c_str());
            }
        }

        // Final guard: never leave malformed text in non-legacy mode.
        if (!gclLooksLikeV18FilterFile(path)) {
            gclWriteWholeText(path, "===HIDDEN CATEGORIES===\r\n===HIDDEN APPS===\r\n");
        }
    }

    bool gclForceRemoveFile(const std::string& path) {
        if (path.empty() || !pathExists(path)) return true;
        return sceIoRemove(path.c_str()) >= 0;
    }

    static bool gclIsPrxPath(const std::string& path) {
        return endsWithNoCase(path, ".prx");
    }

    static bool gclVerifyPrxHeader(const std::string& path) {
        SceIoStat st{};
        if (!pathExists(path, &st)) return false;
        if (st.st_size < 4096 || st.st_size > 2 * 1024 * 1024) return false;
        SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
        if (fd < 0) return false;
        uint8_t hdr[4] = {0, 0, 0, 0};
        int rd = sceIoRead(fd, hdr, sizeof(hdr));
        sceIoClose(fd);
        return (rd == 4 && hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F');
    }

    static bool gclFilesBinaryEqual(const std::string& a, const std::string& b) {
        SceIoStat sa{}, sb{};
        if (!pathExists(a, &sa) || !pathExists(b, &sb)) return false;
        if (sa.st_size != sb.st_size) return false;

        SceUID fa = sceIoOpen(a.c_str(), PSP_O_RDONLY, 0);
        if (fa < 0) return false;
        SceUID fb = sceIoOpen(b.c_str(), PSP_O_RDONLY, 0);
        if (fb < 0) { sceIoClose(fa); return false; }

        char ba[16 * 1024];
        char bb[16 * 1024];
        bool ok = true;
        for (;;) {
            int ra = sceIoRead(fa, ba, sizeof(ba));
            int rb = sceIoRead(fb, bb, sizeof(bb));
            if (ra != rb) { ok = false; break; }
            if (ra < 0) { ok = false; break; }
            if (ra == 0) break;
            if (memcmp(ba, bb, (size_t)ra) != 0) { ok = false; break; }
        }
        sceIoClose(fa);
        sceIoClose(fb);
        return ok;
    }

    static bool gclVerifyPrxInstall(const std::string& src, const std::string& dst) {
        if (!gclVerifyPrxHeader(dst)) return false;
        return gclFilesBinaryEqual(src, dst);
    }

    bool gclForceCopyFile(const std::string& src, const std::string& dst) {
        if (src.empty() || dst.empty() || !pathExists(src)) return false;
        const bool isPrx = gclIsPrxPath(src) || gclIsPrxPath(dst);
        if (strcasecmp(src.c_str(), dst.c_str()) == 0) {
            return isPrx ? gclVerifyPrxHeader(dst) : true;
        }

        if (!isPrx) {
            gclForceRemoveFile(dst);
            return copyFileBuffered(src, dst);
        }

        const std::string tmp = dst + ".tmp_hsrt";
        const std::string bak = dst + ".bak_hsrt";
        gclForceRemoveFile(tmp);
        gclForceRemoveFile(bak);

        if (!copyFileBuffered(src, tmp)) { gclForceRemoveFile(tmp); return false; }
        if (!gclVerifyPrxInstall(src, tmp)) { gclForceRemoveFile(tmp); return false; }

        const bool hadDst = pathExists(dst);
        if (hadDst) {
            if (sceIoRename(dst.c_str(), bak.c_str()) < 0) {
                gclForceRemoveFile(tmp);
                return false;
            }
        }

        bool moved = (sceIoRename(tmp.c_str(), dst.c_str()) >= 0);
        if (!moved) {
            moved = copyFileBuffered(tmp, dst);
            gclForceRemoveFile(tmp);
        }
        if (!moved || !gclVerifyPrxInstall(src, dst)) {
            gclForceRemoveFile(dst);
            if (hadDst) sceIoRename(bak.c_str(), dst.c_str());
            gclForceRemoveFile(tmp);
            return false;
        }

        if (hadDst) gclForceRemoveFile(bak);
        return true;
    }

    bool gclForceMoveFile(const std::string& src, const std::string& dst) {
        if (src.empty() || dst.empty() || !pathExists(src)) return false;
        const bool isPrx = gclIsPrxPath(src) || gclIsPrxPath(dst);
        if (strcasecmp(src.c_str(), dst.c_str()) == 0) {
            return isPrx ? gclVerifyPrxHeader(dst) : true;
        }

        if (!isPrx) {
            gclForceRemoveFile(dst);
            if (sceIoRename(src.c_str(), dst.c_str()) >= 0) return true;
            if (!copyFileBuffered(src, dst)) return false;
            sceIoRemove(src.c_str());
            return true;
        }

        const std::string bak = dst + ".bak_hsrt";
        gclForceRemoveFile(bak);
        const bool hadDst = pathExists(dst);
        if (hadDst) {
            if (sceIoRename(dst.c_str(), bak.c_str()) < 0) return false;
        }

        if (sceIoRename(src.c_str(), dst.c_str()) >= 0) {
            if (gclVerifyPrxHeader(dst)) {
                if (hadDst) gclForceRemoveFile(bak);
                return true;
            }
            sceIoRename(dst.c_str(), src.c_str());
            if (hadDst) sceIoRename(bak.c_str(), dst.c_str());
            return false;
        }

        if (hadDst && !pathExists(dst) && pathExists(bak)) {
            sceIoRename(bak.c_str(), dst.c_str());
        }

        if (!gclForceCopyFile(src, dst)) return false;
        if (sceIoRemove(src.c_str()) < 0) {
            // Keep source intact if delete failed; treat as failed move.
            return false;
        }
        return true;
    }

    bool gclTryHealActivePrx(const std::string& prxActive,
                             const std::vector<std::string>& sources,
                             bool requireExactMatch) {
        for (const auto& src : sources) {
            if (src.empty() || !pathExists(src)) continue;
            if (!gclVerifyPrxHeader(src)) continue;
            if (!gclForceCopyFile(src, prxActive)) continue;
            if (requireExactMatch) {
                if (gclVerifyPrxInstall(src, prxActive)) return true;
            } else {
                if (gclVerifyPrxHeader(prxActive)) return true;
            }
        }
        return false;
    }

    void gclEnsureV18FilterFile(const std::string& path) {
        if (pathExists(path) && gclLooksLikeV18FilterFile(path)) return;
        gclWriteWholeText(path, "===HIDDEN CATEGORIES===\r\n===HIDDEN APPS===\r\n");
    }

    // Toggle between legacy and v1.8 plugin modes
    void gclToggleLegacyMode(bool enableLegacy) {
        std::string primaryRoot = gclPickDeviceRoot();
        std::string targetRoot = rootPrefix(gclDevice);
        if (targetRoot.empty()) targetRoot = primaryRoot;
        std::string seplugins = gclSepluginsDirForRoot(targetRoot);
        if (!dirExists(seplugins)) sceIoMkdir(seplugins.c_str(), 0777);

        const std::string prxActive = joinDirFile(seplugins, "category_lite.prx");
        const std::string prxOld = joinDirFile(seplugins, "category_lite_old.prx");
        const std::string prxV18 = joinDirFile(seplugins, "category_lite_v1.8.prx");
        const std::string bundledPrx = gclBundledPrxPath();
        std::string selectedLegacyPrx;

        std::string filterRoot = gclFiltersRoot();
        std::string filterDir = filterRoot + "seplugins";
        sceIoMkdir(filterDir.c_str(), 0777);
        const std::string filter = joinDirFile(filterDir, "gclite_filter.txt");
        const std::string filterOld = joinDirFile(filterDir, "gclite_filter_old.txt");
        const std::string filterV18 = joinDirFile(filterDir, "gclite_filter_v1.8.txt");

        if (enableLegacy) {
            // Activate legacy PRX if available.
            std::string legacySrc = gclFindLegacyCategoryLitePrxAny(primaryRoot);
            if (legacySrc.empty() && pathExists(prxOld)) legacySrc = prxOld;
            if (legacySrc.empty()) legacySrc = gclFindCategoryLiteOldPrxAny(primaryRoot);
            selectedLegacyPrx = legacySrc;
            if (!legacySrc.empty() && strcasecmp(legacySrc.c_str(), prxActive.c_str()) != 0) {
                gclForceCopyFile(legacySrc, prxActive);
            }
            if ((!pathExists(prxActive) || !gclVerifyPrxHeader(prxActive)) && pathExists(prxOld)) {
                gclForceCopyFile(prxOld, prxActive);
            }
            if ((!pathExists(prxActive) || !gclVerifyPrxHeader(prxActive)) && !bundledPrx.empty()) {
                // Last resort to avoid leaving a broken plugin path.
                gclForceCopyFile(bundledPrx, prxActive);
            }

            // Normalize filter files for legacy-active mode.
            // Keep only one active filter file in legacy mode (gclite_filter.txt).
            const bool filterIsV18 = pathExists(filter) && gclLooksLikeV18FilterFile(filter);
            if (filterIsV18) {
                // Merge current v1.8 entries into a temporary legacy file, then replace active file.
                const std::string legacyTmp = joinDirFile(filterDir, "gclite_filter_legacy_tmp.txt");
                gclForceRemoveFile(legacyTmp);
                if (pathExists(filterOld) && !gclLooksLikeV18FilterFile(filterOld)) {
                    gclForceCopyFile(filterOld, legacyTmp);
                } else {
                    gclWriteWholeText(legacyTmp, "");
                }
                gclMergeLegacyFilters(filter, legacyTmp, /*toLegacy=*/true);
                gclForceMoveFile(legacyTmp, filter);
            }
            if (!pathExists(filter)) {
                if (pathExists(filterOld)) gclForceMoveFile(filterOld, filter);
                if (!pathExists(filter)) gclWriteWholeText(filter, "");
            }

            gclLegacyMode = true;
            gclSetLegacyMode(true);

            // Legacy mode should keep a single active variant of plugin/filter files.
            gclForceRemoveFile(prxOld);
            gclForceRemoveFile(prxV18);
            gclForceRemoveFile(filterOld);
            gclForceRemoveFile(filterV18);
        } else {
            // Preserve current legacy PRX as *_old (if legacy is active now).
            if (pathExists(prxActive) && !gclIsBundledPrxCopy(prxActive)) {
                gclForceMoveFile(prxActive, prxOld);
            } else if (!pathExists(prxOld)) {
                std::string legacySrc = gclFindLegacyCategoryLitePrxAny(primaryRoot);
                if (!legacySrc.empty()) gclForceCopyFile(legacySrc, prxOld);
            }

            // Activate v1.8 PRX.
            bool haveV18 = false;
            if (pathExists(bundledPrx)) haveV18 = gclForceCopyFile(bundledPrx, prxActive);
            if (!haveV18 && pathExists(prxV18)) haveV18 = gclForceMoveFile(prxV18, prxActive);
            if (!haveV18 && !pathExists(prxActive) && pathExists(prxOld)) {
                gclForceCopyFile(prxOld, prxActive);
            }

            // Keep unchecked mode clean (no *_v1.8 files).
            gclForceRemoveFile(prxV18);

            // Preserve legacy filter as *_old if needed.
            if (pathExists(filter) && !gclLooksLikeV18FilterFile(filter)) {
                gclForceMoveFile(filter, filterOld);
            } else if (!pathExists(filterOld)) {
                gclWriteWholeText(filterOld, "");
            }

            // Activate v1.8 filter.
            gclEnsureV18FilterFile(filter);

            gclLegacyMode = false;
            gclSetLegacyMode(false);

            if (pathExists(filterOld)) {
                gclMergeLegacyFilters(filterOld, filter, /*toLegacy=*/false);
            }

            // Safety: ensure gclite_filter.txt is valid v1.8 format after conversion.
            gclRepairV18FilterFileIfNeeded(filter);

            // Keep unchecked mode clean (no *_v1.8 files).
            gclForceRemoveFile(filterV18);
        }

        // Crash-safety guard:
        // If plugin is enabled in CFW config but category_lite.prx is missing,
        // disable plugin entries to avoid boot-time crashes.
        if ((gclArkOn || gclProOn) && (!pathExists(prxActive) || !gclVerifyPrxHeader(prxActive))) {
            bool healed = false;
            if (enableLegacy) {
                // Prefer true legacy source first, then known backups, then bundled v1.8 as last resort.
                std::vector<std::string> legacySources = {
                    selectedLegacyPrx, prxOld, prxV18, bundledPrx
                };
                healed = gclTryHealActivePrx(prxActive, legacySources, /*requireExactMatch=*/false);
            } else {
                // Prefer bundled v1.8 (exact match), then sidecar, then old as last resort.
                std::vector<std::string> v18StrictSources = { bundledPrx, prxV18 };
                healed = gclTryHealActivePrx(prxActive, v18StrictSources, /*requireExactMatch=*/true);
                if (!healed) {
                    std::vector<std::string> fallbackSources = { prxOld };
                    healed = gclTryHealActivePrx(prxActive, fallbackSources, /*requireExactMatch=*/false);
                }
            }

            if (!healed) {
                std::string pluginsSe, vshSe;
                std::string plugins = gclFindArkPluginsFile(pluginsSe);
                std::string vsh = gclFindProVshFile(vshSe);
                if (!plugins.empty() && pathExists(plugins))
                    gclWriteEnableToFileWithVerify(plugins, false, /*arkPluginsTxt=*/true);
                if (!vsh.empty() && pathExists(vsh))
                    gclWriteEnableToFileWithVerify(vsh, false, /*arkPluginsTxt=*/false);
                gclArkOn = false;
                gclProOn = false;
            }
        }

        if (pathExists(prxActive)) gclPrxPath = prxActive;
        gclOldPrxPath = gclFindCategoryLiteOldPrxAny(primaryRoot);
        gclLegacyMode = enableLegacy;
        gclFiltersLoaded = false; gclLegacyFilterLoaded = false;
    }

    // Detect whether boot should migrate a legacy (headerless) filter file.
    bool gclShouldAutoMigrateLegacyFilterOnBoot() {
        if (gclLegacyMode) return false;  // skip if using legacy plugin
        std::string txt;
        if (!gclReadWholeText(gclFiltersPath(), txt)) return false;
        if (txt.find("===") != std::string::npos) return false;  // already v1.8 format
        if (txt.empty()) return false;  // empty, nothing to migrate
        return true;
    }

    // Auto-migrate old-format gclite_filter.txt on boot
    void gclAutoMigrateLegacyFilterOnBoot() {
        if (!gclShouldAutoMigrateLegacyFilterOnBoot()) return;

        std::string filterPath = gclFiltersPath();

        // Old-format file found. Rename to _old and migrate.
        std::string root = gclFiltersRoot();
        std::string oldPath = root + "seplugins/gclite_filter_old.txt";
        if (!pathExists(oldPath)) {
            sceIoRename(filterPath.c_str(), oldPath.c_str());
        } else {
            // _old already exists; just remove the current file (we'll merge from _old)
            sceIoRemove(filterPath.c_str());
        }

        // Merge old entries into a fresh v1.8 filter
        gclFiltersLoaded = false; gclLegacyFilterLoaded = false;
        gclMergeLegacyFilters(oldPath, filterPath, /*toLegacy=*/false);
        gclRepairV18FilterFileIfNeeded(filterPath);
    }

    static bool isBlacklistedBaseNameFor(const std::string& root, const std::string& base) {
        if (base.empty() || !blacklistActive()) return false;
        gclLoadUnifiedFilters();
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
        gclLoadCategoryFilterFor(currentDevice);
        const std::string root = gclFilterRootKeyFor(currentDevice);
        const auto& fl = gclCategoryFilterMap[root];
        for (const auto& w : fl) {
            if (!strcasecmp(w.c_str(), base.c_str())) return true;
        }
        return false;
    }

    void refreshGclFilterFile() {
        gclLoadCategoryFilterFor(currentDevice);
        gclSaveCategoryFilterFor(currentDevice, buildCategoryBaseToDisplayMap());
    }

    void toggleGclFilterForCategory(const std::string& displayName) {
        std::string base = stripCategoryPrefixes(displayName);
        if (base.empty()) return;
        gclLoadCategoryFilterFor(currentDevice);
        const std::string root = gclFilterRootKeyFor(currentDevice);
        auto& fl = gclCategoryFilterMap[root];
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

    void setCategoryHidden(const std::string& displayName, bool hide) {
        std::string base = stripCategoryPrefixes(displayName);
        if (base.empty()) return;
        gclLoadCategoryFilterFor(currentDevice);
        const std::string root = gclFilterRootKeyFor(currentDevice);
        auto& fl = gclCategoryFilterMap[root];
        bool exists = false;
        for (auto it = fl.begin(); it != fl.end(); ++it) {
            if (!strcasecmp(it->c_str(), base.c_str())) {
                exists = true;
                if (!hide) {
                    fl.erase(it);
                    refreshGclFilterFile();
                }
                return;
            }
        }
        if (hide && !exists) {
            fl.push_back(base);
            refreshGclFilterFile();
        }
    }

    void updateFilterOnCategoryRename(const std::string& oldDisplay, const std::string& newDisplay) {
        std::string oldBase = stripCategoryPrefixes(oldDisplay);
        std::string newBase = stripCategoryPrefixes(newDisplay);
        if (oldBase.empty() || newBase.empty()) return;
        gclLoadUnifiedFilters();
        bool changed = false;

        const std::string catRoot = gclFilterRootKeyFor(currentDevice);
        changed |= updateListEntryCaseInsensitive(gclCategoryFilterMap[catRoot], oldBase, newBase);

        const std::string blRoot = blacklistRootKey(currentDevice);
        changed |= updateListEntryCaseInsensitive(gclBlacklistMap[blRoot], oldBase, newBase);

        if (changed) gclSaveUnifiedFilters();
    }

    bool isGameFilteredPath(const std::string& path) {
        if (gclLegacyMode) return isGameFilteredLegacy(path);
        std::string key = normalizeGameFilterPath(path);
        if (key.empty()) return false;
        gclLoadGameFilterFor(currentDevice);
        const std::string root = gclFilterRootKeyFor(currentDevice);
        const auto& fl = gclGameFilterMap[root];
        for (const auto& w : fl) {
            if (!strcasecmp(w.c_str(), key.c_str())) return true;
        }
        return false;
    }

    void setGameHiddenForPaths(const std::vector<std::string>& paths, bool hide) {
        if (paths.empty()) return;
        if (gclLegacyMode) { setGameHiddenLegacy(paths, hide); return; }
        gclLoadGameFilterFor(currentDevice);
        const std::string root = gclFilterRootKeyFor(currentDevice);
        auto& fl = gclGameFilterMap[root];

        for (const auto& p : paths) {
            std::string key = normalizeGameFilterPath(p);
            if (key.empty()) continue;
            if (hide) {
                bool exists = false;
                for (const auto& w : fl) {
                    if (!strcasecmp(w.c_str(), key.c_str())) { exists = true; break; }
                }
                if (!exists) fl.push_back(key);
            } else {
                for (auto it = fl.begin(); it != fl.end(); ) {
                    if (!strcasecmp(it->c_str(), key.c_str())) it = fl.erase(it);
                    else ++it;
                }
            }
        }
        gclSaveGameFilterFor(currentDevice);
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
                pathExists(gclPrxPath) && gclVerifyPrxHeader(gclPrxPath)) {
                return true;
            }
        }

        if (!dirExists(seplugins)) {
            sceIoMkdir(seplugins.c_str(), 0777);
        }
        std::string dst = joinDirFile(seplugins, "category_lite.prx");

        // Where we’re copying from (next to your images)
        std::string baseDir = currentExecBaseDir();
        std::string src = baseDir + "resources/category_lite.prx";

        if (!gclForceCopyFile(src, dst) || !gclVerifyPrxInstall(src, dst)) {
            // If copy from bundled failed, keep a previously valid installed PRX.
            if (!gclPrxPath.empty() && pathExists(gclPrxPath) && gclVerifyPrxHeader(gclPrxPath)) {
                return true;
            }
            if (pathExists(dst) && gclVerifyPrxHeader(dst)) return true;
            return false;
        }

        gclPrxPath = dst;   // remember the exact installed path
        if (!targetRoot.empty()) gclDevice = targetRoot;
        return true;
    }

    void gclMaybeUpdatePrx() {
        if (!gclArkOn && !gclProOn) return;
        if (gclLegacyMode) return;  // don't touch legacy plugin
        if (gclPrxPath.empty()) return;

        SceIoStat srcSt{}, curSt{};
        std::string baseDir = currentExecBaseDir();
        std::string src = baseDir + "resources/category_lite.prx";
        if (!pathExists(src, &srcSt) || !pathExists(gclPrxPath, &curSt)) return;
        if (srcSt.st_size == curSt.st_size) return;

        std::string dir = parentOf(gclPrxPath);
        if (dir.empty()) return;
        std::string tmp = joinDirFile(dir, "category_lite_new.prx");
        std::string old = joinDirFile(dir, "category_lite_old.prx");

        if (pathExists(tmp)) sceIoRemove(tmp.c_str());
        if (!gclForceCopyFile(src, tmp)) { if (pathExists(tmp)) sceIoRemove(tmp.c_str()); return; }
        if (!gclVerifyPrxInstall(src, tmp)) { if (pathExists(tmp)) sceIoRemove(tmp.c_str()); return; }

        if (pathExists(old)) sceIoRemove(old.c_str());
        if (!gclForceMoveFile(gclPrxPath, old)) {
            sceIoRemove(tmp.c_str());
            return;
        }

        if (!gclForceMoveFile(tmp, gclPrxPath) || !gclVerifyPrxInstall(src, gclPrxPath)) {
            // Roll back to previous known-good plugin
            if (pathExists(gclPrxPath)) sceIoRemove(gclPrxPath.c_str());
            if (pathExists(old)) gclForceMoveFile(old, gclPrxPath);
            if (pathExists(tmp)) sceIoRemove(tmp.c_str());
            return;
        }
    }

    // On non-legacy mode, remove stale v1.8 sidecars when main files are already v1.8-active.
    void gclHealRedundantV18ArtifactsOnLoad() {
        if (gclLegacyMode) return;

        std::string targetRoot = rootPrefix(gclDevice);
        if (targetRoot.empty()) targetRoot = gclPickDeviceRoot();
        std::string seplugins = gclSepluginsDirForRoot(targetRoot);
        if (!seplugins.empty()) {
            std::string prxActive = joinDirFile(seplugins, "category_lite.prx");
            std::string prxV18 = joinDirFile(seplugins, "category_lite_v1.8.prx");
            if (pathExists(prxActive) && pathExists(prxV18) && gclIsBundledPrxCopy(prxActive)) {
                sceIoRemove(prxV18.c_str());
            }
            if (pathExists(prxActive)) gclPrxPath = prxActive;
        }

        std::string filterDir = gclFiltersRoot() + "seplugins";
        std::string filter = joinDirFile(filterDir, "gclite_filter.txt");
        std::string filterV18 = joinDirFile(filterDir, "gclite_filter_v1.8.txt");
        gclRepairV18FilterFileIfNeeded(filter);
        if (pathExists(filter) && pathExists(filterV18) && gclLooksLikeV18FilterFile(filter)) {
            sceIoRemove(filterV18.c_str());
        }
    }

    void gclHardCheckPrxIfEnabled() {
        if (!gclArkOn && !gclProOn) return;
        if (gclLegacyMode) return;  // skip PRX update when using legacy v1.6/v1.7 plugin

        // Prefer ARK-4 if both are enabled (matches UI precedence)
        const bool wantArk = gclArkOn;
        const bool wantPro = (!gclArkOn && gclProOn);

        std::string pluginsSe;
        std::string vshSe;
        std::string plugins = gclFindArkPluginsFile(pluginsSe);
        std::string vsh = gclFindProVshFile(vshSe);
        (void)plugins;
        (void)vsh;

        std::string targetSeplugins;
        if (wantArk && !pluginsSe.empty()) {
            targetSeplugins = pluginsSe;
        } else if (wantPro && !vshSe.empty()) {
            targetSeplugins = vshSe;
        }

        if (targetSeplugins.empty()) {
            gclDevice = gclPickDeviceRoot();
            targetSeplugins = gclSepluginsDirForRoot(gclDevice);
        } else {
            gclDevice = rootPrefix(targetSeplugins);
        }
        if (targetSeplugins.empty()) return;
        if (!dirExists(targetSeplugins)) sceIoMkdir(targetSeplugins.c_str(), 0777);

        // Ensure the PRX exists and refresh it if needed.
        if (!gclEnsurePrxPresent(targetSeplugins)) return;
        gclMaybeUpdatePrx();
        gclHealRedundantV18ArtifactsOnLoad();
    }

    // Run after USB disconnect to recover from host-side edits/races that can
    // leave plugin binaries truncated or malformed.
    void gclRunPostUsbIntegrityHeal() {
        gclComputeInitial();

        std::string targetRoot = rootPrefix(gclDevice);
        if (targetRoot.empty()) targetRoot = gclPickDeviceRoot();
        std::string seplugins = gclSepluginsDirForRoot(targetRoot);
        if (seplugins.empty()) return;
        if (!dirExists(seplugins)) sceIoMkdir(seplugins.c_str(), 0777);

        const std::string prxActive = joinDirFile(seplugins, "category_lite.prx");
        const std::string prxOld = joinDirFile(seplugins, "category_lite_old.prx");
        const std::string prxV18 = joinDirFile(seplugins, "category_lite_v1.8.prx");
        const std::string bundledPrx = gclBundledPrxPath();

        // Remove obviously bad sidecars so they don't get selected as sources.
        if (pathExists(prxOld) && !gclVerifyPrxHeader(prxOld)) gclForceRemoveFile(prxOld);
        if (pathExists(prxV18) && !gclVerifyPrxHeader(prxV18)) gclForceRemoveFile(prxV18);

        if (!pathExists(prxActive) || !gclVerifyPrxHeader(prxActive)) {
            std::vector<std::string> sources;
            if (gclLegacyMode) {
                sources.push_back(gclFindLegacyCategoryLitePrxAny(gclPickDeviceRoot()));
                sources.push_back(prxOld);
                sources.push_back(prxV18);
                sources.push_back(bundledPrx);
            } else {
                sources.push_back(bundledPrx);
                sources.push_back(prxV18);
                sources.push_back(prxOld);
            }
            gclTryHealActivePrx(prxActive, sources, /*requireExactMatch=*/false);
        }

        if (pathExists(prxActive) && gclVerifyPrxHeader(prxActive)) gclPrxPath = prxActive;

        // Re-apply enabled-mode hard checks (and safety disable on failure).
        gclHardCheckPrxIfEnabled();
        gclHealRedundantV18ArtifactsOnLoad();
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
            if (!gclDeferredLegacyConvertPending) {
                gclDeferredLegacyConvertPending = gclShouldAutoMigrateLegacyFilterOnBoot();
            }
            gclMaybeUpdatePrx();
            gclHealRedundantV18ArtifactsOnLoad();
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
        currentCategory.clear(); // forget last highlighted category when backing out to devices
        armHomeAnimationForRoot();

        selectedIndex = 0; scrollOffset = 0;
        bool anySelectable = false;
        if (preselect >= 0 && (rowFlags[preselect] & ROW_DISABLED) == 0) {
            selectedIndex = preselect;
            anySelectable = true;
        } else {
            for (int i = 0; i < (int)entries.size(); ++i) {
                if ((rowFlags[i] & ROW_DISABLED) == 0) { selectedIndex = i; anySelectable = true; break; }
            }
        }
        if (!anySelectable) {
            selectedIndex = -1;
            scrollOffset = 0;
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
            (void)filterRoot;
            gclFiltersLoaded = false; gclLegacyFilterLoaded = false;
            gclLoadCategoryFilterFor(currentDevice);
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
        // Always add Uncategorized to the categories list (even if disabled)
        // The UI will show it as faded/disabled when gclCfg.uncategorized is false
        catsSorted.push_back("Uncategorized");


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
        setMsLedSuppressed(false);
        suspendHomeAnimation();
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
            pauseHomeAnimationKeepFrame(); // freeze current frame while modal is visible
            unsigned long long animStartUs = 0;
            if (gEnablePopAnimations && !gPopAnimDirs.empty()) {
                const std::string* animDir = nextPopAnimDir();
                if (animDir && ensurePopAnimLoaded(*animDir)) {
                    msgBox->setAnimation(gPopAnimFrames.data(), gPopAnimFrames.size(), POP_ANIM_TARGET_H);
                    scanAnimActive = true;
                    animStartUs = (unsigned long long)sceKernelGetSystemTimeWide();
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

            // Ensure at least one full animation cycle plays before closing
            if (scanAnimActive && animStartUs > 0 && gPopAnimTotalCycleUs > 0) {
                unsigned long long animEndUs = animStartUs + gPopAnimTotalCycleUs;
                while ((unsigned long long)sceKernelGetSystemTimeWide() < animEndUs) {
                    renderOneFrame();
                    sceKernelDelayThread(10000); // 10ms between frames to avoid busy-waiting
                }
            }

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
            // Always show Categories view when Game Categories is enabled
            // (even if no category folders exist, we still show Settings + Uncategorized)
            buildCategoryRows();
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
