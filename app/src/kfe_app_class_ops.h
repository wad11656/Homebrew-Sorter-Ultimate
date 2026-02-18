    // ====== Key repeat helpers ======
    static unsigned long long nowUS() {
        return (unsigned long long)sceKernelGetSystemTimeWide();
    }
    bool shouldRepeat(unsigned btnMask,
                      unsigned long long& holdStart,
                      unsigned long long& lastRepeat) {
        if ((lastButtons & btnMask) == 0) {
            if (holdStart == 0) holdStart = nowUS();
            lastRepeat = 0;
            return false;
        }
        unsigned long long tnow = nowUS();
        if (tnow - holdStart < REPEAT_DELAY_US) return false;
        unsigned long long interval = (tnow - holdStart >= REPEAT_ACCEL_AFTER_US)
                                      ? REPEAT_INTERVAL_FAST_US
                                      : REPEAT_INTERVAL_US;
        if (lastRepeat == 0 || (tnow - lastRepeat) >= interval) {
            lastRepeat = tnow;
            return true;
        }
        return false;
    }
    // Returns the extracted/cached game title for a given source path,
    // preferring the pre-op snapshot so titles stay stable across the op.
    // Returns "" if no title is known.
    std::string getCachedTitleForPath(const std::string& path) const {
        auto findIn = [&](const std::vector<GameItem>& vec) -> std::string {
            for (const auto& gi : vec) {
                if (gi.path == path) {
                    if (!gi.title.empty()) return gi.title;
                    break;
                }
            }
            return std::string();
        };

        // 1) Pre-op snapshot: check flatAll, then uncategorized, then every category
        if (hasPreOpScan) {
            if (!preOpScan.flatAll.empty()) {
                std::string t = findIn(preOpScan.flatAll);
                if (!t.empty()) return t;
            }
            {
                std::string t = findIn(preOpScan.uncategorized);
                if (!t.empty()) return t;
            }
            for (const auto& kv : preOpScan.categories) {
                const auto& vec = kv.second;
                std::string t = findIn(vec);
                if (!t.empty()) return t;
            }
        }

        // 2) Last resort: whatever is in the current workingList (may be destination)
        return findIn(workingList);
    }



    void performCopy() {
        ClockGuard cg; cg.boost333();
        logInit();
        logf("=== performCopy: n=%d destDev=%s destCat=%s ===",
            (int)opSrcPaths.size(), opDestDevice.c_str(),
            (opDestCategory.empty() ? "Uncategorized" : opDestCategory.c_str()));

        // PSP Go ms0 mode cached free-space preflight (same as Move)
        if (dualDeviceAvailableFromMs0()) {
            const std::string destDevFull = opDestDevice.empty() ? currentDevice : opDestDevice;
            const uint64_t need = bytesNeededForOp(opSrcPaths, opSrcKinds, destDevFull, actionMode==AM_Copy);
            if (need > 0) {
                uint64_t freeBytes = 0; bool haveFree=false;
                FreeSpaceGet(destDevFull.c_str(), freeBytes, haveFree);
                if (haveFree) {
                    static const uint64_t HEADROOM = (4ull << 20);
                    if (freeBytes + HEADROOM < need) {
                        MessageBox mb("Not enough free space on destination.\n\nCopy requires more space than available.",
                                    nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 0.9f, 0, "OK", 16, 18, 8, 14);
                        while (mb.update()) mb.render(font);
                        logClose(); return;
                    }
                }
            }
        }

        msgBox = new MessageBox("Copying...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
        renderOneFrame();

        KfeFileOps::resetCriticalGuardFailure();
        int okCount = 0, failCount = 0;
        bool canceledCritical = false;
        std::vector<std::pair<std::string, std::string>> copiedPairs;
        std::vector<GameItem::Kind> copiedKinds;
        for (size_t i = 0; i < opSrcPaths.size(); ++i) {
            const std::string& src = opSrcPaths[i];
            const GameItem::Kind k = opSrcKinds[i];
            std::string dst = KfeFileOps::buildDestPath(src, k, opDestDevice.empty() ? currentDevice : opDestDevice, opDestCategory);

            // Show the game title as the progress headline
            if (msgBox) {
                std::string title = getCachedTitleForPath(src);
                if (title.empty()) title = basenameOf(src);
                msgBox->setProgressTitle(title.c_str());
                renderOneFrame();
            }

            // Filename detail is already shown by copyFile() via showProgress/updateProgress
            bool ok = KfeFileOps::copyOne(src, dst, k, this);
            if (ok) {
                okCount++;
                copiedPairs.push_back(std::make_pair(src, dst));
                copiedKinds.push_back(k);
            } else {
                failCount++;
                if (KfeFileOps::hasCriticalGuardFailure()) {
                    canceledCritical = true;
                    logf("performCopy: canceling after critical file verification failure");
                    break;
                }
            }
            sceKernelDelayThread(0);
        }


        delete msgBox; msgBox = nullptr;
        logf("=== performCopy: done ok=%d fail=%d ===", okCount, failCount);
        logClose();

        // ---- Refresh rules for COPY (compute BEFORE restoring op state) ----
        const std::string srcDev = preOpDevice;                      // "ms0:/" or "ef0:/"
        const std::string dstDev = opDestDevice.empty() ? srcDev     // same-device copy if empty
                                                        : opDestDevice;
        const bool isCross = (strncasecmp(srcDev.c_str(), dstDev.c_str(), 4) != 0);

        bool refreshMs = false, refreshEf = false;
        if (isCross) {
            refreshMs = true;
            refreshEf = true;
        } else {
            if (!strncasecmp(srcDev.c_str(), "ms0:", 4)) refreshMs = true;
            if (!strncasecmp(srcDev.c_str(), "ef0:", 4)) refreshEf = true;
        }
        if (refreshMs || refreshEf) {
            FreeSpaceInit();
            FreeSpaceSetPresence(refreshMs, refreshEf);
            FreeSpaceRequestRefresh();
        }
        // --------------------------------------------------------------------

        auto &dstEntry = deviceCache[rootPrefix(dstDev)];
        if (dstEntry.snap.flatAll.empty() && dstEntry.snap.uncategorized.empty()
            && dstEntry.snap.categories.empty() && dstEntry.snap.categoryNames.empty()) {
            scanDevice(dstDev);
            snapshotCurrentScan(dstEntry.snap);
            dstEntry.dirty = false;
        }

        // Insert each successfully copied item into destination snapshot
        for (size_t i = 0; i < copiedPairs.size(); ++i) {
            const std::string& s = copiedPairs[i].first;
            const std::string& d = copiedPairs[i].second;
            const GameItem::Kind k = copiedKinds[i];
            cacheApplyMoveOrCopy(dstEntry.snap, dstEntry.snap, s, d, k, /*isMove*/false);
        }
        dstEntry.dirty = false;

        if (!canceledCritical) {
            // Jump to destination view immediately (match Move behavior)
            showDestinationCategoryNow(dstDev, opDestCategory);

            // optional: focus the last copied item
            if (!copiedPairs.empty()) {
                selectByPath(copiedPairs.back().second);
            }
        }

        // clear op state
        actionMode = AM_None;
        opPhase    = OP_None;
        opSrcPaths.clear(); opSrcKinds.clear();
        opSrcCount = 0;
        opSrcTotalBytes = 0;
        opDestDevice.clear(); opDestCategory.clear();

        char res[64];
        if (canceledCritical) {
            MessageBox mb(
                "Error\n"
                "The copy operation failed to move all files to the destination directory.",
                okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "OK",
                10, 18, 82, 9, 340, 95, PSP_CTRL_CROSS);
            while (mb.update()) mb.render(font);
        } else if (failCount == 0) {
            snprintf(res, sizeof(res), "Copied %d item(s)", okCount);
            drawMessage(res, COLOR_GREEN);
        } else if (okCount == 0) {
            snprintf(res, sizeof(res), "Copy failed (%d)", failCount);
            drawMessage(res, COLOR_RED);
        } else {
            snprintf(res, sizeof(res), "Copied %d, failed %d", okCount, failCount);
            drawMessage(res, COLOR_YELLOW);
        }
        if (!canceledCritical) sceKernelDelayThread(800*1000);
    }



    // --- helper: quick probe for any CAT_* folder on a device ---
    bool deviceHasAnyCategory(const std::string& dev) const {
        const char* isoRoots[]  = {"ISO/"};
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};

        auto hasCatIn = [](const std::string& base)->bool {
            if (!dirExists(base)) return false;

            bool hit = false;
            forEachEntry(base, [&](const SceIoDirent &e){
                if (FIO_S_ISDIR(e.d_stat.st_mode) && startsWithCAT(e.d_name)) hit = true;
            });
            return hit;
        };

        for (auto r : isoRoots)  if (hasCatIn(dev + r)) return true;
        for (auto r : gameRoots) if (hasCatIn(dev + r)) return true;
        return false;
    }

    // --- helper (NEW): “any device has categories?” ---
    bool anyDeviceHasCategories() const {
        for (auto &r : roots)
            if (deviceHasAnyCategory(r))
                return true;
        return false;
    }

    // -------- Operation flow control ----------
    bool dualDeviceAvailableFromMs0() const {
        // PSP Go running from ms0:/ means runningFromEf0 == false and both roots exist
        bool hasMs=false, hasEf=false;
        for (auto &r : roots) { if (r=="ms0:/") hasMs=true; if (r=="ef0:/") hasEf=true; }
        return (!runningFromEf0) && hasMs && hasEf;
    }

    void startAction(ActionMode mode) {
        if ((mode == AM_Move || mode == AM_Copy) &&
            !runningAppWarningBypass &&
            selectionIncludesCurrentAppFolder()) {
            openCurrentAppActionWarning(mode == AM_Copy ? RAW_Copy : RAW_Move);
            return;
        }

        actionMode = mode;
        opPhase    = OP_None;
        opSrcPaths.clear();
        opSrcKinds.clear();
        opSrcCount = 0;
        opSrcTotalBytes = 0;
        opDestDevice.clear();
        opDestCategory.clear();

        if (showRoots || !(view == View_AllFlat || view == View_CategoryContents)) {
            msgBox = new MessageBox("Open a file list to use file operations.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
            actionMode = AM_None;
            return;
        }

        // Snapshot selection
        if (!checked.empty()) {
            for (const auto& gi : workingList) {
                if (checked.find(gi.path) == checked.end()) continue;
                opSrcPaths.push_back(gi.path);
                opSrcKinds.push_back(gi.kind);
                opSrcTotalBytes += gi.sizeBytes;
            }
            opSrcCount = (int)opSrcPaths.size();
        } else {
            if (selectedIndex < 0 || selectedIndex >= (int)workingList.size()) {
                msgBox = new MessageBox("No item selected.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                actionMode = AM_None;
                return;
            }
            opSrcPaths.push_back(workingList[selectedIndex].path);
            opSrcKinds.push_back(workingList[selectedIndex].kind);
            opSrcTotalBytes = workingList[selectedIndex].sizeBytes;
            opSrcCount = 1;
        }

        // Save UI snapshot so we can restore
        preOpDevice   = currentDevice;
        preOpView     = view;
        preOpCategory = currentCategory;
        preOpSel      = selectedIndex;
        preOpScroll   = scrollOffset;
        snapshotCurrentScan(preOpScan);
        hasPreOpScan = true;

        const bool goMs0Mode = dualDeviceAvailableFromMs0();
        const bool gclOn = (gclArkOn || gclProOn);

        if (mode == AM_Move || mode == AM_Copy) {
            // Move on single-device contexts still requires categories for a meaningful destination.
            if (mode == AM_Move && !gclOn && !goMs0Mode) {
                msgBox = new MessageBox("Game Categories must be enabled on this device.", okIconTexture,
                                        SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                actionMode = AM_None;
                return;
            }

            // Copy: always show device picker.
            // Move: show device picker only in PSP Go dual-device mode.
            if (mode == AM_Copy || goMs0Mode) {
                opPhase = OP_SelectDevice;

                // NEW: transient feedback while we do the first free-space probe
                const char* returnText = "Returning...";
                const float popScale = 1.0f;
                const int popPadX = 10;
                const int popPadY = 24;
                const int popLineH = (int)(24.0f * popScale + 0.5f);
                const float popTextW = measureTextWidth(popScale, returnText);
                const int popExtraW = 4;
                int popPanelW = (int)(popTextW + popPadX * 2 + popExtraW + 0.5f);
                popPanelW -= 6;
                const int popBottom = 14;
                const int popPanelH = popPadY + popLineH + popBottom - 24;
                const int popWrapTweak = 32;
                const int popForcedPxPerChar = 8;
                const int renamePanelW = popPanelW - 4;
                const int renamePanelH = popPanelH - 4;
                msgBox = new MessageBox("Calculating free space...", nullptr,
                                        SCREEN_WIDTH, SCREEN_HEIGHT,
                                        popScale, 0, "", popPadX, popPadY,
                                        popWrapTweak, popForcedPxPerChar,
                                        renamePanelW + 135, renamePanelH);
                renderOneFrame();

                buildRootRows();  // (will probe free space as needed)

                // close the transient overlay
                delete msgBox; msgBox = nullptr;
            } else {
                if (!hasSameDeviceMoveCopyDestinationForCurrentSelection()) {
                    msgBox = new MessageBox("Needs categories on this device.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                    actionMode = AM_None;
                    return;
                }
                // Selecting within current device
                buildCategoryRowsForOp();
                opPhase = OP_SelectCategory;
            }
        }
    }



    void cancelActionRestore() {
        hasPreOpScan = false;
        // Clear op state
        actionMode = AM_None;
        opPhase    = OP_None;
        opSrcPaths.clear();
        opSrcKinds.clear();
        opSrcCount = 0;
        opSrcTotalBytes = 0;
        opDestDevice.clear();
        opDestCategory.clear();

        // Restore UI state
        scanDevicePreferCache(preOpDevice);
        const bool gclOn = (gclArkOn || gclProOn);
        if (gclOn) {
            if (preOpView == View_CategoryContents) {
                openCategory(preOpCategory.empty() ? "Uncategorized" : preOpCategory);
            } else {
                buildCategoryRows();
            }
        } else {
            openDevice(preOpDevice);
        }
        selectedIndex = preOpSel;
        scrollOffset  = preOpScroll;
    }

    void showConfirmAndRun() {
        const int count = (int)opSrcPaths.size();
        const char* verb = (actionMode == AM_Copy) ? "Copy" : "Move";
        const char* title = (count == 1)
            ? ((actionMode == AM_Copy) ? "Copy App" : "Move App")
            : ((actionMode == AM_Copy) ? "Copy Apps" : "Move Apps");
        char subtitle[96];
        snprintf(subtitle, sizeof(subtitle),
                 "%s %d %s?", verb, count, (count == 1) ? "app" : "apps");
        msgBoxOwnedText = std::string(title) + "\n" + subtitle;
        msgBox = new MessageBox(msgBoxOwnedText.c_str(), okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT,
                                0.9f, 15, "Confirm",
                                10, 18, 82, 9,
                                240, 75);
        msgBox->setOkAlignLeft(true);
        msgBox->setOkPosition(10, 7);
        msgBox->setOkStyle(0.7f, 0xFFBBBBBB);
        msgBox->setOkTextOffset(-2, -1);
        msgBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
        msgBox->setSubtitleGapAdjust(-6);
        msgBox->setCancel(circleIconTexture, "Cancel", PSP_CTRL_CIRCLE);
        opPhase = OP_Confirm;
    }

    void performMove() {
        ClockGuard cg; cg.boost333();
        logInit();
        logf("=== performMove: n=%d destDev=%s destCat=%s ===",
            (int)opSrcPaths.size(),
            opDestDevice.c_str(),
            (opDestCategory.empty() ? "Uncategorized" : opDestCategory.c_str()));

        bool didCross = false;  // <--- ADD THIS
        (void)didCross; // suppress 'set but not used' warnings when no cross-device move happens


        // Cached, non-blocking preflight (PSP Go, running from ms0, both devices present).
        // Uses the background probe only; never calls getFreeBytesCMF() here.
        {
            bool hasMs = false, hasEf = false;
            for (auto &r : roots) { if (r=="ms0:/") hasMs = true; if (r=="ef0:/") hasEf = true; }

            const std::string destDevFull = opDestDevice.empty() ? currentDevice : opDestDevice;

            if (!runningFromEf0 && hasMs && hasEf) {
                const uint64_t need = bytesNeededForOp(opSrcPaths, opSrcKinds, destDevFull, actionMode==AM_Copy);
                if (need > 0) { // only matters for cross-device moves
                    uint64_t freeBytes = 0;
                    bool haveFree = false;
                    FreeSpaceGet(destDevFull.c_str(), freeBytes, haveFree /*ok*/);

                    if (haveFree) {
                        static const uint64_t HEADROOM = (4ull << 20); // ~4 MiB
                        logf("preflight(cached): need=%llu free=%llu on %s",
                            (unsigned long long)need, (unsigned long long)freeBytes, canonicalDev(destDevFull.c_str()));
                        if (freeBytes + HEADROOM < need) {
                            MessageBox mb("Not enough free space on destination.\n\n"
                                        "Move requires more space than available.",
                                        nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 0.9f, 0, "OK", 16, 18, 8, 14);
                            while (mb.update()) mb.render(font);
                            logClose();
                            return; // abort move
                        }
                    } else {
                        // No cached number yet — proceed (picker already handled disabling when cache arrived).
                        logf("preflight(cached): free unknown on %s; proceeding", canonicalDev(destDevFull.c_str()));
                    }
                }
            }
        }



        msgBox = new MessageBox("Moving...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
        renderOneFrame();

        KfeFileOps::resetCriticalGuardFailure();
        int okCount = 0, failCount = 0;
        bool canceledCritical = false;
        struct MovedPair {
            std::string src;
            std::string dst;
            GameItem::Kind kind;
        };
        std::vector<MovedPair> movedPairs;
        movedPairs.reserve(opSrcPaths.size());
        bool movedCurrentApp = false;
        std::string movedCurrentAppDst;
        for (size_t i = 0; i < opSrcPaths.size(); ++i) {
            const std::string& src = opSrcPaths[i];
            const GameItem::Kind k = opSrcKinds[i];
            std::string dst = KfeFileOps::buildDestPath(src, k, opDestDevice.empty() ? currentDevice : opDestDevice, opDestCategory);
            const bool srcIsCurrentApp = (k == GameItem::EBOOT_FOLDER) && isCurrentExecFolderPath(src);

            // Game title as headline
            if (msgBox) {
                std::string title = getCachedTitleForPath(src);
                if (title.empty()) title = basenameOf(src);
                msgBox->setProgressTitle(title.c_str());
                // Show filename detail + progress bar, even for instant renames
                msgBox->showProgress(basenameOf(src).c_str(), 0, 1);
                renderOneFrame();
            }

            if (!KfeFileOps::sameDevice(src, dst)) didCross = true;

            // Filename detail appears from the underlying copy/move implementation
            bool ok = KfeFileOps::moveOne(src, dst, k, this);
            if (msgBox) {
                msgBox->updateProgress(1, 1);
                renderOneFrame();
            }
            if (ok) {
                okCount++;
                checked.erase(src);
                movedPairs.push_back({src, dst, k});
                if (srcIsCurrentApp) {
                    movedCurrentApp = true;
                    movedCurrentAppDst = dst;
                }
            } else {
                failCount++;
                if (KfeFileOps::hasCriticalGuardFailure()) {
                    canceledCritical = true;
                    logf("performMove: canceling after critical file verification failure");
                    break;
                }
            }
            sceKernelDelayThread(0);
        }


        delete msgBox; msgBox = nullptr;
        logf("=== performMove: done ok=%d fail=%d ===", okCount, failCount);
        logClose();

        // didCross already computed inside the loop

        // Update gclite_filter for moved items only (never for copies)
        for (const auto& mv : movedPairs) {
            updateGameFilterOnItemRename(mv.src, mv.dst);
        }
        const bool forceFullRescanAfterMove = movedCurrentApp;
        if (movedCurrentApp && !movedCurrentAppDst.empty()) {
            setExecBaseOverrideFromAppDir(movedCurrentAppDst);
            reloadHomeAnimationsForExec();
            // Moving the running app can leave stale ICON0/path memoization behind.
            // Force fresh scans and clear icon-related transient caches.
            markAllDevicesDirty();
            freeSelectionIcon();
            noIconPaths.clear();
            selectionIconRetryAtUs = 0;
            if (iconCarryTex) {
                texFree(iconCarryTex);
                iconCarryTex = nullptr;
                iconCarryForPath.clear();
            }
        }

        // --- Patch caches and jump instantly to destination ---

        const std::string srcDev = preOpDevice;                                  // e.g., "ms0:/"
        const std::string dstDev = opDestDevice.empty() ? srcDev : opDestDevice; // same-device if empty

        // 1) Ensure we have cache entries (created if missing)
        auto &srcEntry = deviceCache[rootPrefix(srcDev)];
        auto &dstEntry = deviceCache[rootPrefix(dstDev)];

        // 2) If we have no snapshots yet (first time ever), build them once and store.
        //    Otherwise we will patch them below.
        if (!forceFullRescanAfterMove) {
            if (srcEntry.snap.flatAll.empty() && srcEntry.snap.uncategorized.empty()
                && srcEntry.snap.categories.empty() && srcEntry.snap.categoryNames.empty()) {
                scanDevice(srcDev);                 // one-time build
                snapshotCurrentScan(srcEntry.snap);
                srcEntry.dirty = false;
            }
            if (strncasecmp(srcDev.c_str(), dstDev.c_str(), 4) != 0) {
                if (dstEntry.snap.flatAll.empty() && dstEntry.snap.uncategorized.empty()
                    && dstEntry.snap.categories.empty() && dstEntry.snap.categoryNames.empty()) {
                    scanDevice(dstDev);             // one-time build for the other device
                    snapshotCurrentScan(dstEntry.snap);
                    dstEntry.dirty = false;
                }
            }
        }

        // 3) Patch both snapshots with the results (remove from src; add/rename into dst)
        if (!forceFullRescanAfterMove) {
            for (size_t i = 0; i < movedPairs.size(); ++i) {
                const std::string& src = movedPairs[i].src;
                const std::string& dst = movedPairs[i].dst;
                const GameItem::Kind k = movedPairs[i].kind;
                cacheApplyMoveOrCopy(srcEntry.snap, dstEntry.snap, src, dst, k, /*isMove*/true);
            }
        }
        // Keep them valid for instant reuse
        if (!forceFullRescanAfterMove) {
            srcEntry.dirty = false;
            dstEntry.dirty = false;
        } else {
            srcEntry.dirty = true;
            dstEntry.dirty = true;
        }

        // Select the destination category and repaint full contents
        if (forceFullRescanAfterMove) {
            // Use the exact same slow/full rebuild path as manually selecting a storage device.
            openDevice(dstDev);
            armIconReloadGraceWindow();
            if (hasCategories) {
                const std::string cat = opDestCategory.empty() ? std::string("Uncategorized") : opDestCategory;
                openCategory(cat);
            }
        } else {
            showDestinationCategoryNow(dstDev, opDestCategory);
        }

        // Focus the last moved item so it’s highlighted
        if (!movedPairs.empty()) {
            selectByPath(movedPairs.back().dst);
        }

        // clear op state
        actionMode = AM_None;
        opPhase    = OP_None;
        opSrcPaths.clear(); opSrcKinds.clear();
        opSrcCount = 0;
        opSrcTotalBytes = 0;
        opDestDevice.clear(); opDestCategory.clear();

        // Toast
        char res[64];
        if (canceledCritical) {
            snprintf(res, sizeof(res), "Move canceled: critical file missing");
            drawMessage(res, COLOR_RED);
        } else {
            if (failCount == 0)      snprintf(res, sizeof(res), "Moved %d item(s)", okCount);
            else if (okCount == 0)   snprintf(res, sizeof(res), "Move failed (%d)", failCount);
            else                     snprintf(res, sizeof(res), "Moved %d, failed %d", okCount, failCount);
            drawMessage(res, (failCount ? (okCount ? COLOR_YELLOW : COLOR_RED) : COLOR_GREEN));
        }
        sceKernelDelayThread(800 * 1000);

    }

    // -----------------------------------------------------------
    // Input handling
    // -----------------------------------------------------------
public:
    KernelFileExplorer(){ detectRoots(); scrubHiddenAppFiltersOnStartup(); buildRootRows(); }
    ~KernelFileExplorer(){
        setMsLedSuppressed(false);
        if (font) intraFontUnload(font);
        if (fontJpn) intraFontUnload(fontJpn);
        if (fontKr) intraFontUnload(fontKr);
        freeSelectionIcon();
        freeCategoryIcon();
        if (placeholderIconTexture) { texFree(placeholderIconTexture); placeholderIconTexture = nullptr; }
        if (circleIconTexture) { texFree(circleIconTexture); circleIconTexture = nullptr; }
        if (triangleIconTexture) { texFree(triangleIconTexture); triangleIconTexture = nullptr; }
        if (squareIconTexture) { texFree(squareIconTexture); squareIconTexture = nullptr; }
        if (selectIconTexture) { texFree(selectIconTexture); selectIconTexture = nullptr; }
        if (startIconTexture) { texFree(startIconTexture); startIconTexture = nullptr; }
        if (rootMemIcon) { texFree(rootMemIcon); rootMemIcon = nullptr; }
        if (rootInternalIcon) { texFree(rootInternalIcon); rootInternalIcon = nullptr; }
        if (rootUsbIcon) { texFree(rootUsbIcon); rootUsbIcon = nullptr; }
        if (rootCategoriesIcon) { texFree(rootCategoriesIcon); rootCategoriesIcon = nullptr; }
        if (rootArk4Icon) { texFree(rootArk4Icon); rootArk4Icon = nullptr; }
        if (rootProMeIcon) { texFree(rootProMeIcon); rootProMeIcon = nullptr; }
        if (rootOffBulbIcon) { texFree(rootOffBulbIcon); rootOffBulbIcon = nullptr; }
        if (catFolderIcon) { texFree(catFolderIcon); catFolderIcon = nullptr; }
        if (catFolderIconGray) { texFree(catFolderIconGray); catFolderIconGray = nullptr; }
        if (catSettingsIcon) { texFree(catSettingsIcon); catSettingsIcon = nullptr; }
        if (blacklistIcon) { texFree(blacklistIcon); blacklistIcon = nullptr; }
        if (lIconTexture) { texFree(lIconTexture); lIconTexture = nullptr; }
        if (rIconTexture) { texFree(rIconTexture); rIconTexture = nullptr; }
        if (ps1IconTexture) { texFree(ps1IconTexture); ps1IconTexture = nullptr; }
        if (homebrewIconTexture) { texFree(homebrewIconTexture); homebrewIconTexture = nullptr; }
        if (isoIconTexture) { texFree(isoIconTexture); isoIconTexture = nullptr; }
        if (updateIconTexture) { texFree(updateIconTexture); updateIconTexture = nullptr; }
        if (ps1IconTextureGray) { texFree(ps1IconTextureGray); ps1IconTextureGray = nullptr; }
        if (homebrewIconTextureGray) { texFree(homebrewIconTextureGray); homebrewIconTextureGray = nullptr; }
        if (isoIconTextureGray) { texFree(isoIconTextureGray); isoIconTextureGray = nullptr; }
        if (updateIconTextureGray) { texFree(updateIconTextureGray); updateIconTextureGray = nullptr; }
        if (warningIconTexture) { texFree(warningIconTexture); warningIconTexture = nullptr; }
        if (updownIconTexture) { texFree(updownIconTexture); updownIconTexture = nullptr; }
        if (!gPopAnimFrames.empty()) { freeAnimationFrames(gPopAnimFrames); gPopAnimMinDelayUs = 0; }
        if (!gHomeAnimFrames.empty()) { freeAnimationFrames(gHomeAnimFrames); gHomeAnimMinDelayUs = 0; }
        freeHomeAnimStreaming();  // Free streaming mode resources
        gHomeAnimEntries.clear();
        gHomeAnimIndex = -1;
        gHomeAnimFrameIndex = 0;
        gHomeAnimNextUs = 0;
        gHomeAnimStreaming = false;
        gPopAnimLoadedDir.clear();
        gPopAnimDirs.clear();
        gPopAnimOrder.clear();
        gPopAnimOrderIndex = 0;
        if (fileMenu) { delete fileMenu; fileMenu = nullptr; }
    }

    void init() {
    #if FORCE_APP_333
        // Run the entire app at 333/166 to keep background scans snappy.
        // (OSK/Move already boost via ClockGuard; this makes it global.)
        scePowerSetClockFrequency(333, 333, 166);
    #endif

        sceGuInit(); sceGuStart(GU_DIRECT,list);
        sceGuDrawBuffer(GU_PSM_8888,(void*)0,512);
        sceGuDispBuffer(SCREEN_WIDTH,SCREEN_HEIGHT,(void*)0x88000,512);
        sceGuDepthBuffer((void*)0x110000,512);
        sceGuOffset(2048-(SCREEN_WIDTH/2),2048-(SCREEN_HEIGHT/2));
        sceGuViewport(2048,2048,SCREEN_WIDTH,SCREEN_HEIGHT);
        sceGuDepthRange(65535,0);
        sceGuScissor(0,0,SCREEN_WIDTH,SCREEN_HEIGHT);
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuDepthFunc(GU_GEQUAL);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuFrontFace(GU_CW);
        sceGuShadeModel(GU_SMOOTH);
        sceGuEnable(GU_CULL_FACE);
        sceGuEnable(GU_CLIP_PLANES);
        sceGuFinish(); sceGuSync(0,0);
        sceDisplayWaitVblankStart(); sceGuDisplay(GU_TRUE);

        intraFontInit();
        font = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_MED | INTRAFONT_STRING_UTF8);
        fontJpn = intraFontLoad("flash0:/font/jpn0.pgf", INTRAFONT_CACHE_MED | INTRAFONT_STRING_UTF8);
        fontKr  = intraFontLoad("flash0:/font/kr0.pgf", INTRAFONT_CACHE_MED | INTRAFONT_STRING_UTF8);
        if (!font) pspDebugScreenInit();

        // Chain fonts for automatic fallback: Latin → Japanese → Korean
        if (font && fontJpn) intraFontSetAltFont(font, fontJpn);
        if (fontJpn && fontKr) intraFontSetAltFont(fontJpn, fontKr);

        // Prime font cache by rendering dummy text to eliminate artifacting
        if (font) {
            sceGuStart(GU_DIRECT, list);
            intraFontActivate(font);
            intraFontSetStyle(font, 1.0f, 0x00000000, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
            intraFontPrint(font, -100.0f, -100.0f, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,!? ");
            sceGuFinish();
            sceGuSync(0, 0);
        }

        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

        // USB drivers are started on demand when USB Mode is entered.

    }
