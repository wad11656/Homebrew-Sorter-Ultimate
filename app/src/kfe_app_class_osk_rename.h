    // ===================== OSK (unchanged) =====================
    static void utf8ToUtf16(const std::string& u8, std::vector<SceWChar16>& u16) {
        u16.clear(); u16.reserve(u8.size()+1);
        for (unsigned char c : u8) u16.push_back((SceWChar16)c);
        u16.push_back(0);
    }
    static std::string utf16ToUtf8(const SceWChar16* u16) {
        std::string out; if (!u16) return out;
        while (*u16) { out.push_back((char)(*u16 & 0xFF)); ++u16; }
        return out;
    }
    static std::vector<SceWChar16>& oskOutBuf() {
        static std::vector<SceWChar16> out16;
        return out16;
    }
    bool promptTextOSK(const char* titleUtf8, const char* initialUtf8, int maxChars, std::string& out) {
        ClockGuard     cg;  cg.boost333();
        ThreadPrioGuard tpg(0x10);
        #ifdef HAVE_SCEPOWERLOCK
        PowerLockGuard  plg;
        #endif

        SceUtilityOskData data{};
        std::vector<SceWChar16> title16, init16;
        utf8ToUtf16(titleUtf8 ? titleUtf8 : "Rename", title16);
        utf8ToUtf16(initialUtf8 ? initialUtf8 : "",    init16);
        std::vector<SceWChar16>& out16 = oskOutBuf();
        out16.assign(maxChars + 1, 0);

        data.language      = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
        data.lines         = 1;
        data.unk_24        = 1;

        unsigned inputMask = 0;
        #ifdef PSP_UTILITY_OSK_INPUTTYPE_LATIN
        inputMask |= PSP_UTILITY_OSK_INPUTTYPE_LATIN;
        #endif
        #ifdef PSP_UTILITY_OSK_INPUTTYPE_NUMERIC
        inputMask |= PSP_UTILITY_OSK_INPUTTYPE_NUMERIC;
        #endif
        #ifdef PSP_UTILITY_OSK_INPUTTYPE_PUNCTUATION
        inputMask |= PSP_UTILITY_OSK_INPUTTYPE_PUNCTUATION;
        #endif
        if (inputMask == 0) inputMask = PSP_UTILITY_OSK_INPUTTYPE_ALL;
        data.inputtype = inputMask;

        data.desc          = title16.data();
        data.intext        = init16.data();
        data.outtextlength = maxChars + 1; // include space for terminator
        data.outtextlimit  = maxChars;
        data.outtext       = out16.data();

        SceUtilityOskParams params{};
        params.base.size           = sizeof(params);
        params.base.language       = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
        params.base.buttonSwap     = PSP_UTILITY_ACCEPT_CROSS;
        params.base.graphicsThread = 0x11;
        params.base.accessThread   = 0x10;
        params.base.fontThread     = 0x10;
        params.base.soundThread    = 0x10;
        params.datacount           = 1;
        params.data                = &data;

        if (sceUtilityOskInitStart(&params) < 0) {
            drawMessage("OSK init failed", COLOR_RED);
            sceKernelDelayThread(800*1000);
            return false;
        }

        while (true) {
            int st = sceUtilityOskGetStatus();
            if (st == PSP_UTILITY_DIALOG_INIT || st == PSP_UTILITY_DIALOG_VISIBLE) {
                drawBackdropOnlyForOSK();
                sceUtilityOskUpdate(1);
            #if OSK_USE_VBLANK_CB
                sceDisplayWaitVblankStartCB();
            #endif
                sceGuSwapBuffers();
            } else if (st == PSP_UTILITY_DIALOG_QUIT) {
                sceUtilityOskShutdownStart();
            } else if (st == PSP_UTILITY_DIALOG_FINISHED) {
                break;
            } else {
                sceKernelDelayThread(1000);
            }
        }

        sceUtilityOskShutdownStart();
        int safety = 0;
        while (sceUtilityOskGetStatus() != PSP_UTILITY_DIALOG_NONE && safety++ < 600) {
            drawBackdropOnlyForOSK();
            sceUtilityOskUpdate(1);
        #if OSK_USE_VBLANK_CB
            sceDisplayWaitVblankStartCB();
        #endif
            sceGuSwapBuffers();
        }

        restoreGuAfterUtility();
        renderOneFrame();

        if (data.result == PSP_UTILITY_OSK_RESULT_OK) {
            std::string raw = utf16ToUtf8(out16.data());
            auto trimSpaces = [](std::string& s){
                size_t n = s.size();
                size_t i = 0;
                while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
                size_t j = n;
                while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r' || s[j - 1] == '\n')) j--;
                if (i > 0 || j < n) s = s.substr(i, j - i);
            };
            trimSpaces(raw);
            if (raw.empty()) return false;
            out = sanitizeFilename(raw);
            return true;
        }
        return false;
    }

    // FAT-style filesystems can reject case-only renames when done directly.
    // Fallback: rename via a temporary sibling name, then to the requested target.
    static int renamePathCaseAware(const std::string& from, const std::string& to) {
        if (from.empty() || to.empty()) return -1;
        if (from == to) return 0;
        int rc = sceIoRename(from.c_str(), to.c_str());
        if (rc >= 0) return rc;
        if (strcasecmp(from.c_str(), to.c_str()) != 0) return rc;

        const std::string parent = parentOf(from);
        if (parent.empty()) return rc;

        const unsigned long long seedUs = (unsigned long long)sceKernelGetSystemTimeWide();
        for (int i = 0; i < 16; ++i) {
            char tmpLeaf[64];
            snprintf(tmpLeaf, sizeof(tmpLeaf), ".__kfe_case_tmp_%08X_%02d",
                     (unsigned)(seedUs & 0xFFFFFFFFu), i);
            const std::string tmp = joinDirFile(parent, tmpLeaf);
            SceIoStat st{};
            if (sceIoGetstat(tmp.c_str(), &st) >= 0) continue;

            int r1 = sceIoRename(from.c_str(), tmp.c_str());
            if (r1 < 0) return r1;
            int r2 = sceIoRename(tmp.c_str(), to.c_str());
            if (r2 >= 0) return r2;

            // Best-effort rollback if second hop fails.
            sceIoRename(tmp.c_str(), from.c_str());
            return r2;
        }
        return rc;
    }

    // ===================== Rename logic (unchanged) =====================
    void beginRenameSelected() {
        if (showRoots || moving) return;

        if (view == View_Categories) {
            if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;
            std::string oldDisplay = entries[selectedIndex].d_name;
            if (!strcasecmp(oldDisplay.c_str(), kCatSettingsLabel) || !strcasecmp(oldDisplay.c_str(), "__GCL_SETTINGS__")) return;
            if (!strcasecmp(oldDisplay.c_str(), "Uncategorized")) return;

            std::string typed;
            if (!promptTextOSK("Rename Category", oldDisplay.c_str(), 64, typed)) return;

            // Treat user input as a BASE name (strip CAT_/XX if they typed it)
            typed = sanitizeFilename(stripCategoryPrefixes(typed));
            // If base didn’t change, nothing to do
            if (stripCategoryPrefixes(oldDisplay) == typed) return;
            if (isBlacklistedBaseNameFor(currentDevice, typed)) {
                drawMessage("Blacklisted name", COLOR_RED);
                sceKernelDelayThread(700*1000);
                return;
            }

            const char* renameText = "Renaming...";
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
            const int renamePanelW = popPanelW + 4;
            const int renamePanelH = popPanelH;
            msgBox = new MessageBox(renameText, nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                    popScale, 0, "", popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                    renamePanelW, renamePanelH);
            renderOneFrame();

        const char* isoRoots[]  = {"ISO/"};
            const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};
            bool anyOk=false, anyFail=false;

            // First, rename the existing folder (whatever its current prefix) to the BASE on each root.
            // If Sort is ON and the base starts with two digits, use a temporary "00" prefix so we don't
            // accidentally strip those digits as a sort number during enforcement.
            std::string renameTo = typed;
            if (gclCfg.catsort && hasTwoDigitsAfter(renameTo.c_str())) {
                char tmpNum[8];
                snprintf(tmpNum, sizeof(tmpNum), "%02d", 0);
                renameTo = gclCfg.prefix
                    ? (std::string("CAT_") + tmpNum + typed)
                    : (std::string(tmpNum) + typed);
            }

            for (auto r : isoRoots) {
                std::string base = currentDevice + std::string(r);
                std::string from = base + oldDisplay;
                std::string to   = base + renameTo;
                if (dirExists(from)) {
                    int rc = renamePathCaseAware(from, to);
                    if (rc < 0) {
                        logInit();
                        logf("category rename FAIL: %s -> %s rc=%d", from.c_str(), to.c_str(), rc);
                        logClose();
                    }
                    (rc >= 0) ? anyOk=true : anyFail=true;
                }
            }
            for (auto r : gameRoots) {
                std::string base = currentDevice + std::string(r);
                std::string from = base + oldDisplay;
                std::string to   = base + renameTo;
                if (dirExists(from)) {
                    int rc = renamePathCaseAware(from, to);
                    if (rc < 0) {
                        logInit();
                        logf("category rename FAIL: %s -> %s rc=%d", from.c_str(), to.c_str(), rc);
                        logClose();
                    }
                    (rc >= 0) ? anyOk=true : anyFail=true;
                }
            }

            enforceCategorySchemeForDevice(currentDevice);
            // Old display name was captured earlier in this function as 'oldDisplay'

            // Compute the *display* name we will now use for this category
            std::string newDisplay = findDisplayNameForCategoryBase(currentDevice, typed);
            bool updatedExecBaseFromCategoryRename = false;
            std::string oldExecDir = currentExecBaseDir();
            if (!oldExecDir.empty() && oldExecDir.back() == '/') oldExecDir.pop_back();
            bool oldExecDirGoneAfterRename = false;

            // Patch in-memory caches (icon paths, no-icon memo set, selection key) to follow the rename
            cachePatchRenameCategory(oldDisplay, newDisplay);
            updateFilterOnCategoryRename(oldDisplay, newDisplay);
            {
                if (oldDisplay != newDisplay) {
                    const char* isoRoots[]  = {"ISO/"};
                    const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};
                    auto maybeUpdateExecBase = [&](const char* r){
                        if (updatedExecBaseFromCategoryRename || oldExecDir.empty()) return;
                        std::string base = currentDevice + std::string(r);
                        std::string oldPath = joinDirFile(base, oldDisplay.c_str());
                        std::string newPath = joinDirFile(base, newDisplay.c_str());
                        if (!oldPath.empty() && oldPath.back() == '/') oldPath.pop_back();
                        if (!newPath.empty() && newPath.back() == '/') newPath.pop_back();
                        const size_t oldLen = oldPath.size();
                        if (oldLen == 0 || oldExecDir.size() <= oldLen) return;
                        if (strncasecmp(oldExecDir.c_str(), oldPath.c_str(), oldLen) != 0) return;
                        if (oldExecDir[oldLen] != '/') return;
                        std::string mappedAppDir = newPath + oldExecDir.substr(oldLen);
                        setExecBaseOverrideFromAppDir(mappedAppDir);
                        updatedExecBaseFromCategoryRename = true;
                    };
                    auto maybeUpdate = [&](const char* r){
                        std::string base = currentDevice + std::string(r);
                        std::string oldPath = joinDirFile(base, oldDisplay.c_str());
                        std::string newPath = joinDirFile(base, newDisplay.c_str());
                        if (!dirExists(oldPath) && dirExists(newPath)) {
                            updateHiddenAppPathsForFolderRename(base, oldDisplay, newDisplay);
                        }
                        maybeUpdateExecBase(r);
                    };
                    for (auto r : isoRoots)  maybeUpdate(r);
                    for (auto r : gameRoots) maybeUpdate(r);
                }
            }
            if (!oldExecDir.empty() && !dirExists(oldExecDir)) {
                oldExecDirGoneAfterRename = true;
            }
            buildCategoryRows();


            // Select the renamed category and keep it highlighted
            int idx = -1;
            for (int i = 0; i < (int)entries.size(); ++i) {
                if (!strcasecmp(entries[i].d_name, newDisplay.c_str())) { idx = i; break; }
            }
            if (idx < 0) {
                auto sameBase = [&](const char* disp){
                    return !strcasecmp(stripCategoryPrefixes(disp).c_str(), typed.c_str());
                };
                for (int i = 0; i < (int)entries.size(); ++i) {
                    if (sameBase(entries[i].d_name)) { idx = i; break; }
                }
            }
            if (idx >= 0) {
                selectedIndex = idx;
                const int visible = categoryVisibleRows();
                scrollOffset = (idx >= visible) ? (idx - visible + 1) : 0;
            }

            drawMessage(anyOk && !anyFail ? "Category renamed" : (anyOk ? "Some renamed" : "Rename failed"),
                        anyOk ? COLOR_GREEN : COLOR_RED);
            sceKernelDelayThread(600*1000);

            // Dismiss the “Renaming...” modal, just like item rename does.
            delete msgBox; 
            msgBox = nullptr;
            renderOneFrame();
            if (updatedExecBaseFromCategoryRename || oldExecDirGoneAfterRename) {
                reloadHomeAnimationsForExec();
            }

            return;
        }


        if (view == View_AllFlat || view == View_CategoryContents) {
            if (selectedIndex < 0 || selectedIndex >= (int)workingList.size()) return;
            GameItem gi = workingList[selectedIndex];
            std::string dir  = dirnameOf(gi.path);
            std::string base = basenameOf(gi.path);

            if (gi.kind == GameItem::EBOOT_FOLDER &&
                !runningAppWarningBypass &&
                isCurrentExecFolderPath(gi.path)) {
                openCurrentAppActionWarning(RAW_Rename);
                return;
            }

            std::string typed;
            int maxChars = 64;
            std::string initial = base;
            const int ebootMaxBytes = 31;
            if (gi.kind == GameItem::EBOOT_FOLDER) {
                maxChars = ebootMaxBytes;
                if ((int)initial.size() > ebootMaxBytes) initial = initial.substr(0, ebootMaxBytes);
            }
            if (!promptTextOSK("Rename", initial.c_str(), maxChars, typed)) return;
            if (gi.kind == GameItem::EBOOT_FOLDER && (int)typed.size() > ebootMaxBytes) {
                typed = typed.substr(0, ebootMaxBytes);
            }
            if (typed == base) return;

            // after OSK
            if (gi.kind == GameItem::ISO_FILE) {
                // Cache the original 4-char extension (includes the dot), e.g. ".iso"
                std::string origExt = fileExtOf(base);  // from the CURRENT filename
                // If user’s new text doesn’t end with a valid ISO-like suffix, tack the original back on.
                auto hasIsoLikeTail = [&](const std::string& s)->bool {
                    return endsWithNoCase(s, ".iso") ||
                        endsWithNoCase(s, ".cso") ||
                        endsWithNoCase(s, ".zso") ||
                        endsWithNoCase(s, ".jso") ||
                        endsWithNoCase(s, ".dax");
                };

                // Only enforce if we actually had an ISO-like original extension
                bool origIsIsoLike = hasIsoLikeTail(base);
                if (origIsIsoLike && !hasIsoLikeTail(typed)) {
                    // Requirement: check the **last 4** and append the original 4 if they’re not ISO-like
                    // All supported extensions are 4 chars (dot + 3 letters), so this matches the spec.
                    if (typed.size() < 4 || !hasIsoLikeTail(typed)) {
                        if (origExt.size() == 4) {
                            typed += origExt;  // preserve the original casing
                        }
                    }
                }
            }


            std::string newPath = joinDirFile(dir, typed.c_str());

            SceIoStat tmp{};
            if (sceIoGetstat(newPath.c_str(), &tmp) >= 0) {
                if (strcasecmp(newPath.c_str(), gi.path.c_str()) != 0) {
                    drawMessage("Name exists", COLOR_RED);
                    sceKernelDelayThread(700*1000);
                    return;
                }
            }

            bool wasChecked = (checked.find(gi.path) != checked.end());

            const char* renameText = "Renaming...";
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
            msgBox = new MessageBox(renameText, nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                    popScale, 0, "", popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                    renamePanelW, renamePanelH);
            renderOneFrame();

                int rc = renamePathCaseAware(gi.path, newPath);
                if (rc < 0) {
                    delete msgBox; msgBox = nullptr;
                    drawMessage("Rename failed", COLOR_RED);
                    sceKernelDelayThread(700*1000);
                    return;
                }
                const bool renamedCurrentApp =
                    (gi.kind == GameItem::EBOOT_FOLDER) && isCurrentExecFolderPath(gi.path);
                if (renamedCurrentApp) {
                    setExecBaseOverrideFromAppDir(newPath);
                }

                // If we were showing an ICON0 for this exact item, carry it across the path change.
                // (Don't carry the placeholder; only a real texture.)
                if (!renamedCurrentApp &&
                    selectionIconTex && selectionIconTex != placeholderIconTexture &&
                    selectionIconKey == gi.path) {
                    iconCarryTex = selectionIconTex;      // do NOT free; we'll reattach after refresh
                    iconCarryForPath = newPath;
                    selectionIconTex = nullptr;
                    selectionIconKey.clear();
                }
                // Make sure we don't accidentally block future loads for the new path.
                noIconPaths.erase(newPath);

                // Keep modal visible while we finish all follow-up work
                if (wasChecked) {
                    checked.erase(gi.path);
                    checked.insert(newPath);
                }

                // after successful sceIoRename(...)
                std::string keepPath = newPath;
                const bool forceFullRescanAfterRename = renamedCurrentApp;

                updateGameFilterOnItemRename(gi.path, newPath);
                if (!forceFullRescanAfterRename) {
                    // Patch cache instead of rescanning
                    cachePatchRenameItem(gi.path, newPath, gi.kind);

                    // Preserve the current unsaved order in this list.
                    bool updated = false;
                    for (auto &item : workingList) {
                        if (item.path == gi.path) {
                            item = makeItemFor(newPath, gi.kind);
                            updated = true;
                            break;
                        }
                    }
                    if (updated) {
                        refillRowsFromWorkingPreserveSel();
                    } else {
                        // Fallback: refresh just the current view
                        if (view == View_CategoryContents) {
                            openCategory(currentCategory);
                        } else if (view == View_AllFlat) {
                            rebuildFlatFromCache();
                        } else {
                            buildCategoryRows();
                        }
                    }
                } else {
                    // Renaming the running app can stale icon/path memoization.
                    // Force a clean in-memory rebuild from disk.
                    const bool wasCategoryView = (view == View_CategoryContents);
                    const std::string restoreCategory = currentCategory;
                    markAllDevicesDirty();
                    freeSelectionIcon();
                    noIconPaths.clear();
                    selectionIconRetryAtUs = 0;
                    if (iconCarryTex) {
                        texFree(iconCarryTex);
                        iconCarryTex = nullptr;
                        iconCarryForPath.clear();
                    }

                    // Use the exact same full rebuild path as selecting the device from root.
                    openDevice(currentDevice);
                    armIconReloadGraceWindow();
                    if (wasCategoryView && hasCategories) {
                        openCategory(restoreCategory);
                    }
                }

                // Re-select the renamed item; ensureSelectionIcon() will snap the carried ICON0 into place.
                selectByPath(keepPath);
                drawMessage("Renamed", COLOR_GREEN);
                sceKernelDelayThread(600*1000);

                // NOW dismiss the "Renaming..." modal
                delete msgBox; msgBox = nullptr;
                renderOneFrame();
                if (renamedCurrentApp) {
                    reloadHomeAnimationsForExec();
                }

        }
    }

    void renderOneFrame() {
        sceGuStart(GU_DIRECT, list);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDepthMask(GU_TRUE);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuScissor(0,0,SCREEN_WIDTH,SCREEN_HEIGHT);

        if (backgroundTexture && backgroundTexture->data) {
            sceGuTexMode  (GU_PSM_8888, 0, 0, GU_FALSE);
            sceGuTexFunc  (GU_TFX_REPLACE, GU_TCC_RGB);
            sceGuTexImage (0, backgroundTexture->stride, backgroundTexture->stride, backgroundTexture->stride, backgroundTexture->data);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap  (GU_CLAMP,   GU_CLAMP);
            sceGuEnable   (GU_TEXTURE_2D);

            struct { float u,v; uint32_t color; float x,y,z; } *vtx =
                (decltype(vtx))sceGuGetMemory(2 * sizeof(*vtx));
            vtx[0].u = 0.0f;                            vtx[0].v = 0.0f;
            vtx[0].x = 0.0f;                            vtx[0].y = 0.0f;  vtx[0].color = 0xFFFFFFFF;  vtx[0].z = 0.0f;
            vtx[1].u = float(backgroundTexture->width); vtx[1].v = float(backgroundTexture->height);
            vtx[1].x = float(SCREEN_WIDTH);             vtx[1].y = float(SCREEN_HEIGHT);
            vtx[1].color = 0xFFFFFFFF;                  vtx[1].z = 0.0f;
            sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_COLOR_8888 | GU_TRANSFORM_2D, 2, nullptr, vtx);
            sceGuDisable(GU_TEXTURE_2D);
        } else {
            sceGuClearColor(COLOR_BG);
            sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);
        }

        drawHeader();
        drawFileList();
        drawControls();
        drawFooter();

        ensureSelectionIcon();
        drawSelectedIconLowerRight();
        ensureCategoryIcon();
        drawCategoryIconLowerRight();

        if (msgBox)  msgBox->render(font);
        if (gUsbBox) gUsbBox->render(font);
        if (fileMenu) fileMenu->render(font);

        // NEW: draw the option picker modal (on top)
        // NEW: draw the option picker modal (on top)
        if (optMenu) optMenu->render(font);

        // USB Connected modal (handle input here so Circle disconnects)
        if (gUsbBox) {
            if (!gUsbBox->update()) {
                UsbDeactivate();
                UsbStopStacked();
                gUsbActive = false;
                gUsbShownConnected = false;
                delete gUsbBox; gUsbBox = nullptr;
                inputWaitRelease = true;
                reloadHomeAnimationsForExec();
                gclRunPostUsbIntegrityHeal();
            }
        }



        sceGuFinish();
        sceGuSync(0,0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }




    void selectByPath(const std::string& path){
        if (path.empty()) return;
        for (int i = 0; i < (int)workingList.size(); ++i) {
            if (workingList[i].path == path) {
                selectedIndex = i;
                if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                const int visible = contentVisibleRows();
                if (selectedIndex >= scrollOffset + visible)
                    scrollOffset = selectedIndex - visible + 1;
                return;
            }
        }
    }

    // --- add near other small helpers in the class (optional, but tidy) ---
    void bulkSelect(int dir, bool uncheck) {
        if (showRoots) return;
        if (!(view == View_AllFlat || view == View_CategoryContents)) return;
        if (selectedIndex < 0 || selectedIndex >= (int)workingList.size()) return;

        const std::string& cur = workingList[selectedIndex].path;
        if (uncheck) {
            checked.erase(cur);
            int j = selectedIndex + dir;
            while (j >= 0 && j < (int)workingList.size()) {
                const std::string& p = workingList[j].path;
                if (checked.find(p) == checked.end()) break;
                checked.erase(p);
                j += dir;
            }
        } else {
            checked.insert(cur);
            int j = selectedIndex + dir;
            while (j >= 0 && j < (int)workingList.size()) {
                const std::string& p = workingList[j].path;
                if (checked.find(p) != checked.end()) break;
                checked.insert(p);
                j += dir;
            }
        }
    }

    // -----------------------------------------------------------
