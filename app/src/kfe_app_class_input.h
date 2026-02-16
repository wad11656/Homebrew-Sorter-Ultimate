    void handleInput(){
        if (inputWaitRelease) {
            SceCtrlData pad{}; sceCtrlReadBufferPositive(&pad, 1);
            if (pad.Buttons == 0) inputWaitRelease = false;
            return;
        }

        SceCtrlData pad; sceCtrlReadBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~lastButtons;

        // Drive USB connection + UI while active
        if (gUsbActive) {
            int s = sceUsbGetState();  // OR’d PSP_USB_* flags
            // If the cable is in but not activated yet, activate now
            if ((s & PSP_USB_CABLE_CONNECTED) && !(s & PSP_USB_ACTIVATED)) {
                sceUsbActivate(0x1c8); // default mass storage PID
            }
            const bool connected = (s & PSP_USB_CONNECTION_ESTABLISHED);
            if (connected != gUsbShownConnected) {
                if (gUsbBox) { delete gUsbBox; gUsbBox = nullptr; }
                const char* usbMsg = connected
                    ? "Connected to PC\nOn PSP Go, sometimes both devices don't mount to the PC immediately. It may take 30sec-1min."
                    : "Connect to PC...\nOn PSP Go, Bluetooth must be turned off in the System Settings.\nwarning.png Not Vita-compatible.";
                const int usbPanelH = 110;
                gUsbBox = new MessageBox(
                    usbMsg,
                    circleIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "Disconnect",
                    10, 18, 60, 9, 280, usbPanelH, PSP_CTRL_CIRCLE);
                gUsbBox->setOkAlignLeft(true);
                gUsbBox->setOkPosition(10, 7);
                gUsbBox->setOkStyle(0.7f, 0xFFBBBBBB);
                gUsbBox->setOkTextOffset(-2, -1);
                gUsbBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                gUsbBox->setSubtitleGapAdjust(-8);
                gUsbBox->setInlineIcon(warningIconTexture, "warning.png");
                gUsbShownConnected = connected;
            }
        }
        // Disabled: thumbstick-up timestamp toggle
        // const bool analogUpNow = (pad.Ly <= 30);
        // if (analogUpNow && !analogUpHeld) {
        //     showDebugTimes = !showDebugTimes;
        // }
        // analogUpHeld = analogUpNow;

        bool repeatUp = false, repeatDown = false;
        if (pad.Buttons & PSP_CTRL_UP) {
            if ((pressed & PSP_CTRL_UP) == 0)
                repeatUp = shouldRepeat(PSP_CTRL_UP, upHoldStartUS, upLastRepeatUS);
            else { upHoldStartUS = nowUS(); upLastRepeatUS = 0; }
        } else { upHoldStartUS = 0; upLastRepeatUS = 0; }

        if (pad.Buttons & PSP_CTRL_DOWN) {
            if ((pressed & PSP_CTRL_DOWN) == 0)
                repeatDown = shouldRepeat(PSP_CTRL_DOWN, downHoldStartUS, downLastRepeatUS);
            else { downHoldStartUS = nowUS(); downLastRepeatUS = 0; }
        } else { downHoldStartUS = 0; downLastRepeatUS = 0; }

        if ((pad.Buttons & PSP_CTRL_SQUARE) == 0) {
            bulkSquareUncheck = false;
        }

        lastButtons = pad.Buttons;

        // ===== Bulk select while holding Square =====
        if ((pad.Buttons & PSP_CTRL_SQUARE) &&
            !showRoots && (view == View_AllFlat || view == View_CategoryContents))
        {
            // Square + Left  => Select all
            if (pressed & PSP_CTRL_LEFT) {
                // mark every visible item
                for (const auto& gi : workingList) checked.insert(gi.path);
                return;  // swallow input
            }

            // Square + Right => Unselect all
            if (pressed & PSP_CTRL_RIGHT) {
                checked.clear();
                return;  // swallow input
            }

            // Square + Up    => Bulk select upward from current row until a checked barrier
            if ((pressed & PSP_CTRL_UP) || repeatUp) {
                bulkSelect(-1, bulkSquareUncheck);
                return;  // swallow navigation while painting
            }
            // Square + Down  => Bulk select downward from current row until a checked barrier
            if ((pressed & PSP_CTRL_DOWN) || repeatDown) {
                bulkSelect(+1, bulkSquareUncheck);
                return;  // swallow navigation while painting
            }
        }

        // ===== SELECT: Category sort mode in Categories view; A→Z in content views =====
        // if (pressed & PSP_CTRL_SELECT) {
        //     if (!showRoots && view == View_Categories) {
        //         // Toggle on-screen sort mode
        //         if (!catSortMode) {
        //             catSortMode   = true;
        //             catPickActive = false;
        //             catPickIndex  = -1;
        //         } else {
        //             // Leaving sort mode: apply the visible order to numbering & rename as needed
        //             catSortMode   = false;
        //             catPickActive = false;
        //             catPickIndex  = -1;

        //             applyCategoryOrderAndPersist();   // updates XX and CAT_XX (if enabled), renames on disk
        //             buildCategoryRows();              // rebuild rows from current cache/files

        //             // Clamp selection to a valid row (stay in Categories screen)
        //             if (selectedIndex >= (int)entries.size()) selectedIndex = (int)entries.size() - 1;
        //             if (selectedIndex < 0) selectedIndex = 0;
        //         }
        //         return; // swallow SELECT
        //     }

        //     // In content views, keep existing behavior: quick A→Z sort of the working list
        //     if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
        //         moving = false;
        //         sortWorkingListAlpha(
        //             /*byTitle=*/showTitles,
        //             /*workingList=*/workingList,
        //             /*selectedIndex=*/selectedIndex,
        //             /*scrollOffset=*/scrollOffset
        //         );
        //         refillRowsFromWorkingPreserveSel();
        //         return;
        //     }
        // }


        // ---------------------------
        // While an op is active (Move),
        // ---------------------------
        // ---------------------------
        // While an op is active (Move/Copy), restrict navigation per spec
        // ---------------------------
        if (actionMode != AM_None) {
            // Movement in lists (Up/Down)
            if ((pressed & PSP_CTRL_UP) || repeatUp) {
                if (selectedIndex < 0) return;
                if (selectedIndex > 0) {
                    int j = selectedIndex - 1;
                    if (!showRoots && opPhase == OP_SelectCategory) {
                        while (j >= 0 &&
                               opDisabledCategories.find(entries[j].d_name) != opDisabledCategories.end())
                            j--;
                    } else if (showRoots && opPhase == OP_SelectDevice) {
                        while (j >= 0 && (rowFlags[j] & ROW_DISABLED)) j--;
                    }
                    if (j >= 0) { selectedIndex = j; if (selectedIndex < scrollOffset) scrollOffset = selectedIndex; }
                }
            }
            if ((pressed & PSP_CTRL_DOWN) || repeatDown) {
                if (selectedIndex < 0) return;
                if (selectedIndex + 1 < (int)entries.size()) {
                    int j = selectedIndex + 1;
                    if (!showRoots && opPhase == OP_SelectCategory) {
                        while (j < (int)entries.size() &&
                               opDisabledCategories.find(entries[j].d_name) != opDisabledCategories.end())
                            j++;
                    } else if (showRoots && opPhase == OP_SelectDevice) {
                        while (j < (int)entries.size() && (rowFlags[j] & ROW_DISABLED)) j++;
                    }
                    if (j < (int)entries.size()) {
                        selectedIndex = j;
                        const int visible = (!showRoots && opPhase == OP_SelectCategory)
                            ? categoryVisibleRows()
                            : MAX_DISPLAY;
                        if (selectedIndex >= scrollOffset + visible)
                            scrollOffset = selectedIndex - visible + 1;
                    }
                }
            }

            // Confirm/cancel flow
            if (pressed & PSP_CTRL_CIRCLE) {
                if (!showRoots && opPhase == OP_SelectCategory && !opDestDevice.empty() &&
                    (actionMode == AM_Copy || roots.size() > 1)) {
                    opDestCategory.clear();
                    buildRootRowsForDevicePicker();
                    return;
                }
                // Show immediate exit overlay, restore view, then close overlay.
                const char* actWord = (actionMode == AM_Move) ? "Move" : "Copy";
                char exitingMsg[64];
                snprintf(exitingMsg, sizeof(exitingMsg), "Exiting %s operation...", actWord);

                // Show a passive overlay
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
                msgBox = new MessageBox(exitingMsg, nullptr,
                                        SCREEN_WIDTH, SCREEN_HEIGHT,
                                        popScale, 0, "", popPadX, popPadY,
                                        popWrapTweak, popForcedPxPerChar,
                                        renamePanelW + 135, renamePanelH);
                renderOneFrame();   // paint the overlay once

                // Restore previous view/state
                cancelActionRestore();

                // Close the overlay immediately after restore
                delete msgBox; msgBox = nullptr;
                renderOneFrame();   // paint restored UI

                inputWaitRelease = true;  // avoid re-trigger from held ◯
                return;
            }

            if (pressed & PSP_CTRL_CROSS) {
                if (opPhase == OP_SelectDevice) {
                    if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;
                    if (showRoots &&
                        selectedIndex < (int)rowFlags.size() &&
                        (rowFlags[selectedIndex] & ROW_DISABLED)) {

                        RowDisableReason rsn = (selectedIndex < (int)rowReason.size())
                                            ? rowReason[selectedIndex] : RD_NONE;

                        if (rsn == RD_NO_SPACE) {
                            uint64_t needB = (selectedIndex < (int)rowNeedBytes.size()) ? rowNeedBytes[selectedIndex] : 0;
                            uint64_t freeB = (selectedIndex < (int)rowFreeBytes.size()) ? rowFreeBytes[selectedIndex] : 0;
                            std::string msg = "Not enough space (need " + humanBytes(needB) +
                                            ", free " + humanBytes(freeB) + ")";
                            msgBox = new MessageBox(msg.c_str(), okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                        } else {
                            msgBox = new MessageBox("Device not selectable.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                        }
                        return; // swallow X
                    }

                    // proceed with your existing selection flow:
                    opDestDevice = entries[selectedIndex].d_name; // "ms0:/" or "ef0:/"

                    // NEW: on PSP Go running from ms0, begin a background probe of the OPPOSITE device now.
                    if (!runningFromEf0) {
                        bool hasMs=false, hasEf=false;
                        for (auto &r : roots) { if (r=="ms0:/") hasMs=true; if (r=="ef0:/") hasEf=true; }
                        if (hasMs && hasEf) {
                            // Opposite of the picked one:
                            FreeSpaceProbeOppositeOf(opDestDevice.c_str());
                        }
                    }

                    const bool useCategoryPicker = (gclArkOn || gclProOn);

                    if (hasPreOpScan && strcasecmp(opDestDevice.c_str(), preOpDevice.c_str()) == 0) {
                        // Same device as the one we were already viewing → reuse instantly
                        restoreScan(preOpScan);

                        if (useCategoryPicker) {
                            buildCategoryRowsForOp();
                            opPhase = OP_SelectCategory;
                        } else {
                            opDestCategory.clear();
                            showConfirmAndRun();
                        }
                    } else {
                        // Prefer cache; only show spinner if we actually need to scan
                        bool needScan = deviceCache[rootPrefix(opDestDevice)].dirty;
                        if (needScan) {
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
                            msgBox = new MessageBox("Scanning destination...", nullptr,
                                                    SCREEN_WIDTH, SCREEN_HEIGHT,
                                                    popScale, 0, "", popPadX, popPadY,
                                                    popWrapTweak, popForcedPxPerChar,
                                                    renamePanelW + 121, renamePanelH);
                            renderOneFrame();
                        }
                        scanDevicePreferCache(opDestDevice);
                        if (needScan) { delete msgBox; msgBox = nullptr; }


                        if (useCategoryPicker) {
                            buildCategoryRowsForOp();
                            opPhase = OP_SelectCategory;
                        } else {
                            opDestCategory.clear();
                            showConfirmAndRun();
                        }
                    }
                    return;
                }
                else if (opPhase == OP_SelectCategory) {
                    if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;

                    // Block X on a disabled category
                    if (opDisabledCategories.find(entries[selectedIndex].d_name) != opDisabledCategories.end()) {
                        msgBox = new MessageBox("Cannot choose the source category.", okIconTexture,
                                                SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                        return;
                    }

                    std::string cat = entries[selectedIndex].d_name;
                    if (!strcasecmp(cat.c_str(), "Uncategorized")) opDestCategory.clear();
                    else opDestCategory = cat;
                    if (opDestDevice.empty()) opDestDevice = preOpDevice; // same-device move/copy
                    showConfirmAndRun();
                    return;
                }
                else if (opPhase == OP_Confirm) {
                    // The confirm dialog itself closes on X (handled in run loop), then we perform the op there.
                    return;
                }
            }
            return; // swallow other inputs during op mode
        }

        // --- OPEN (X) while just browsing categories ---
        if ((pressed & PSP_CTRL_CROSS) && actionMode == AM_None &&
            !showRoots && view == View_Categories && !msgBox && !fileMenu &&
            !catSortMode) { // ← don’t open while sorting
            FreeSpacePauseNow();
            if (selectedIndex >= 0 && selectedIndex < (int)entries.size()) {
                std::string nm = entries[selectedIndex].d_name;
                if (nm == kCatSettingsLabel || nm == "__GCL_SETTINGS__") {
                    openGclSettingsScreen();
                } else {
                    openCategory(nm.c_str());
                }
            }
            FreeSpaceResume();
            return;
        }




        // R trigger: toggle Sort mode on Categories; A→Z elsewhere
        if (pressed & PSP_CTRL_RTRIGGER) {
            if (showRoots) {
                cycleHomeAnimation(+1);
                return;
            }
            if (!showRoots && view == View_Categories) {
                if (!gclCfg.catsort) {
                    msgBox = new MessageBox(
                        "Sorting not enabled\n"
                        "To enter Sort mode, select \"Game Categories Settings\" at the top of the list, then turn on the \"Sort Categories\" option.",
                        okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "OK",
                        10, 18, 80, 9, 280, 120, PSP_CTRL_CROSS);
                    msgBox->setOkAlignLeft(true);
                    msgBox->setOkPosition(10, 7);
                    msgBox->setOkStyle(0.7f, 0xFFBBBBBB);
                    msgBox->setOkTextOffset(-2, -1);
                    msgBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                    msgBox->setSubtitleGapAdjust(-8);
                } else {
                    setCategorySortMode(true);
                }
            } else if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                moving = false;
                sortWorkingListAlpha(showTitles, workingList, selectedIndex, scrollOffset, contentVisibleRows());
                refillRowsFromWorkingPreserveSel();
            } else {
                beginRenameSelected();
            }
            return;
        }

        // □ toggle checkmark on current item
        if (pressed & PSP_CTRL_SQUARE) {
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents) &&
                selectedIndex >= 0 && selectedIndex < (int)workingList.size()) {
                const std::string& p = workingList[selectedIndex].path;
                auto it = checked.find(p);
                if (it == checked.end()) {
                    checked.insert(p);
                    bulkSquareUncheck = false;
                } else {
                    checked.erase(it);
                    bulkSquareUncheck = true;
                }
            }
            return;
        }

        // START: save
        if (pressed & PSP_CTRL_START) {
            if (!showRoots && view == View_Categories && catSortMode) {
                applyCategoryOrderAndPersist();
                return;
            }
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                commitOrderTimestamps();
            }
            return;
        }

        // Toggle label mode (Triangle → previously LTRIGGER; keep as-is)
        if (pressed & PSP_CTRL_LTRIGGER) {
            if (showRoots) {
                cycleHomeAnimation(-1);
                return;
            }
            if (!showRoots && view == View_Categories) {
                if (!gclCfg.catsort) {
                    msgBox = new MessageBox(
                        "Sorting not enabled\n"
                        "To enter Sort mode, select \"Game Categories Settings\" at the top of the list, then turn on the \"Sort Categories\" option.",
                        okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "OK",
                        10, 18, 80, 9, 280, 120, PSP_CTRL_CROSS);
                    msgBox->setOkAlignLeft(true);
                    msgBox->setOkPosition(10, 7);
                    msgBox->setOkStyle(0.7f, 0xFFBBBBBB);
                    msgBox->setOkTextOffset(-2, -1);
                    msgBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                    msgBox->setSubtitleGapAdjust(-8);
                } else {
                    setCategorySortMode(false);
                }
            } else {
                showTitles = !showTitles;
                if (!showRoots && (view==View_AllFlat || view==View_CategoryContents))
                    refillRowsFromWorkingPreserveSel();
            }
            return;
        }

        // SELECT: Categories → Rename; Content views → Rename
        if (pressed & PSP_CTRL_SELECT) {
            if (!showRoots && view == View_Categories) {
                if (!catSortMode) {
                    if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;
                    const char* nm = entries[selectedIndex].d_name;
                    if (!strcasecmp(nm, kCatSettingsLabel) || !strcasecmp(nm, "__GCL_SETTINGS__")) return;
                    beginRenameSelected();
                }
                return;
            }

            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                beginRenameSelected();
                return;
            }
            return;
        }






        // △: open modal menu (content views → Move/Copy/Delete; categories → New/Delete)
        if (pressed & PSP_CTRL_TRIANGLE) {
            if (!showRoots && !fileMenu) {
                if (view == View_AllFlat || view == View_CategoryContents) {
                    const bool gclOn = (gclArkOn || gclProOn);
                    const bool canCrossDevices = dualDeviceAvailableFromMs0();
                    const bool canWithinDevice = gclOn && hasCategories;
                    const bool canMoveCopy     = canCrossDevices || canWithinDevice;

                    std::vector<std::string> hidePaths;
                    if (!checked.empty()) {
                        for (const auto& p : checked) hidePaths.push_back(p);
                    } else if (selectedIndex >= 0 && selectedIndex < (int)workingList.size()) {
                        hidePaths.push_back(workingList[selectedIndex].path);
                    }
                    int hiddenCount = 0, unhiddenCount = 0;
                    for (const auto& p : hidePaths) {
                        if (isGameFilteredPath(p)) hiddenCount++;
                        else unhiddenCount++;
                    }
                    const bool hideInXmb = (unhiddenCount >= hiddenCount);
                    bool hasIsoLike = false;
                    for (const auto& p : hidePaths) {
                        if (isIsoLike(p)) { hasIsoLike = true; break; }
                    }
                    const bool hideDisabled = hidePaths.empty() || hasIsoLike || !gclOn;
                    const char* hideLabel = hideInXmb ? "Hide in XMB" : "Unhide in XMB";

                    std::vector<FileOpsItem> items = {
                        { "Move",   !canMoveCopy },
                        { "Copy",   !canMoveCopy },
                        { hideLabel, hideDisabled },
                        { "Delete", false }
                    };
                    menuContext = MC_ContentOps;
                    fileMenu = new FileOpsMenu("App Operations", items, SCREEN_WIDTH, SCREEN_HEIGHT, 250, 160);

                    // NEW: Prime & debounce
                    SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
                    fileMenu->primeButtons(now.Buttons);
                    inputWaitRelease = true;
                } else if (view == View_Categories) {
                    const bool gclOn = (gclArkOn || gclProOn);
                    bool canHide = false;
                    bool canDelete = false;
                    bool catHidden = false;
                    if (selectedIndex >= 0 && selectedIndex < (int)entries.size() &&
                        !isCategoryRowLocked(selectedIndex)) {
                        const char* nm = entries[selectedIndex].d_name;
                        std::string base = stripCategoryPrefixes(nm);
                        catHidden = isFilteredBaseName(base);
                        canHide = gclOn && !gclLegacyMode;  // legacy plugins can't hide categories
                        canDelete = true;
                    }
                    const char* catHideLabel = catHidden ? "Unhide in XMB" : "Hide in XMB";
                    std::vector<FileOpsItem> items = {
                        { "New",    false },
                        { catHideLabel, !canHide },
                        { "Delete", !canDelete }
                    };
                    menuContext = MC_CategoryOps;
                    fileMenu = new FileOpsMenu("Category Operations", items, SCREEN_WIDTH, SCREEN_HEIGHT, 250, 140);

                    // NEW: Prime & debounce
                    SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
                    fileMenu->primeButtons(now.Buttons);
                    inputWaitRelease = true;
                }
            }
            return;
        }



        // UP navigation
        if ((pressed & PSP_CTRL_UP) || repeatUp) {
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                if (moving && selectedIndex > 0) {
                    std::swap(workingList[selectedIndex], workingList[selectedIndex-1]);
                    selectedIndex--;
                    if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                    refillRowsFromWorkingPreserveSel();
                } else {
                    if (selectedIndex > 0){
                        selectedIndex--;
                        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                    }
                }
            } else {
                if (showRoots) {
                    int j = selectedIndex - 1;
                    while (j >= 0 && (rowFlags[j] & ROW_DISABLED)) j--;
                    if (j >= 0) {
                        selectedIndex = j;
                        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                    }
                } else {
                    if (view == View_GclSettings) {
                        int j = selectedIndex - 1;
                        while (j >= 0 && (rowFlags[j] & ROW_DISABLED)) j--;
                        if (j >= 0) {
                            selectedIndex = j;
                            if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                        }
                    } else
                    // NEW: while sorting categories and a row is picked, move the picked category instead of just moving selection
                    if (!showRoots && view == View_Categories && catSortMode && catPickActive) {
                        int i = selectedIndex;
                        int j = i - 1;
                        // skip locked rows (e.g., "Category Settings", "Uncategorized")
                        while (j >= 0 && isCategoryRowLocked(j)) j--;
                        if (j >= 0) {
                            std::swap(entries[i], entries[j]);
                            selectedIndex = j;
                            catPickIndex  = j; // keep [MOVE] on the row you're carrying
                            if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                        }
                    } else {
                        if (selectedIndex > 0){
                            selectedIndex--;
                            if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                        }
                    }
                }
            }
        }

        // DOWN navigation
        if ((pressed & PSP_CTRL_DOWN) || repeatDown) {
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                if (moving && selectedIndex + 1 < (int)workingList.size()) {
                    std::swap(workingList[selectedIndex], workingList[selectedIndex+1]);
                    selectedIndex++;
                    const int visible = contentVisibleRows();
                    if (selectedIndex >= scrollOffset + visible) scrollOffset = selectedIndex - visible + 1;
                    refillRowsFromWorkingPreserveSel();
                } else {
                    if (selectedIndex + 1 < (int)entries.size()){
                        selectedIndex++;
                        const int visible = contentVisibleRows();
                        if (selectedIndex >= scrollOffset + visible) scrollOffset = selectedIndex - visible + 1;
                    }
                }
            } else {
                if (showRoots) {
                    int j = selectedIndex + 1;
                    while (j < (int)entries.size() && (rowFlags[j] & ROW_DISABLED)) j++;
                    if (j < (int)entries.size()) {
                        selectedIndex = j;
                        if (selectedIndex >= scrollOffset + MAX_DISPLAY) scrollOffset = selectedIndex - MAX_DISPLAY + 1;
                    }
                } else {
                    if (view == View_GclSettings) {
                        if (selectedIndex + 1 < (int)entries.size()) {
                            const char* cur = entries[selectedIndex].d_name;
                            if (!strncasecmp(cur, "Sort Categories:", 16) && gclCfg.prefix == 0) {
                                int next = selectedIndex + 1;
                                if (next < (int)rowFlags.size() && (rowFlags[next] & ROW_DISABLED)) {
                                    msgBox = new MessageBox(
                                        "Prefixes not enabled\n"
                                        "Enable the \"Category prefix\" setting to blacklist certain folders from auto-converting/auto-renaming as category folders.",
                                        okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "OK",
                                        10, 18, 80, 9, 280, 120, PSP_CTRL_CROSS);
                                    msgBox->setOkAlignLeft(true);
                                    msgBox->setOkPosition(10, 7);
                                    msgBox->setOkStyle(0.7f, 0xFFBBBBBB);
                                    msgBox->setOkTextOffset(-2, -1);
                                    msgBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                                    msgBox->setSubtitleGapAdjust(-8);
                                    return;
                                }
                            }
                        }
                        int j = selectedIndex + 1;
                        while (j < (int)entries.size() && (rowFlags[j] & ROW_DISABLED)) j++;
                        if (j < (int)entries.size()) {
                            selectedIndex = j;
                            const int visible = gclSettingsVisibleRows();
                            if (selectedIndex >= scrollOffset + visible) scrollOffset = selectedIndex - visible + 1;
                        }
                    } else
                    // NEW: while sorting categories and a row is picked, move the picked category instead of just moving selection
                    if (!showRoots && view == View_Categories && catSortMode && catPickActive) {
                        int i = selectedIndex;
                        int j = i + 1;
                        // skip locked rows (e.g., "Category Settings", "Uncategorized")
                        while (j < (int)entries.size() && isCategoryRowLocked(j)) j++;
                        if (j < (int)entries.size()) {
                            std::swap(entries[i], entries[j]);
                            selectedIndex = j;
                            catPickIndex  = j; // keep [MOVE] on the row you're carrying
                            const int visible = categoryVisibleRows();
                            const int lastVisible = scrollOffset + visible - 1;
                            if (selectedIndex > lastVisible) scrollOffset = selectedIndex - (visible - 1);
                        }
                    } else {
                        // Check if navigating down would land on disabled Uncategorized in Categories view
                        if (!showRoots && view == View_Categories && actionMode == AM_None &&
                            !isUncategorizedEnabledForDevice(currentDevice) && selectedIndex + 1 < (int)entries.size()) {
                            const char* nextName = entries[selectedIndex + 1].d_name;
                            if (strcasecmp(nextName, "Uncategorized") == 0) {
                                // Show modal and don't move selection
                                msgBox = new MessageBox(
                                    "Uncategorized Disabled\n"
                                    "To enable the \"Uncategorized\" folder, select \"Game Categories Settings\" at the top of the list, then turn on the \"Show Uncategorized\" option.",
                                    okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "OK",
                                    10, 18, 106, 9, 280, 120, PSP_CTRL_CROSS);
                                msgBox->setOkAlignLeft(true);
                                msgBox->setOkPosition(10, 7);
                                msgBox->setOkStyle(0.7f, 0xFFBBBBBB);
                                msgBox->setOkTextOffset(-2, -1);
                                msgBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                                msgBox->setSubtitleGapAdjust(-8);
                                return; // Block navigation
                            }
                        }
                        if (selectedIndex + 1 < (int)entries.size()){
                            selectedIndex++;
                            const int visible = (!showRoots && view == View_Categories)
                                ? categoryVisibleRows()
                                : ((!showRoots && (view == View_AllFlat || view == View_CategoryContents))
                                    ? contentVisibleRows()
                                    : MAX_DISPLAY);
                            if (selectedIndex >= scrollOffset + visible) scrollOffset = selectedIndex - visible + 1;
                        }
                    }
                }
            }
        }


        // X / O default behaviors
        if (pressed & PSP_CTRL_CROSS) {
            if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;

            // NEW: handle toggles inside the settings screen
            if (!showRoots && view == View_GclSettings) {
                handleGclToggleAt(selectedIndex);
                return;
            }

            if (showRoots) {
                if (selectedIndex < (int)rowFlags.size() && (rowFlags[selectedIndex] & ROW_DISABLED)) return;

                std::string dev = entries[selectedIndex].d_name;
                // Root-level master toggles
                if (dev == "__GCL_TOGGLE__") {
                    // Show a 3-option picker: Off / ARK-4 / PRO/ME
                    std::vector<OptionItem> items = {
                        { "Off",   false },
                        { "ARK-4", false },
                        { "PRO/ME",false }
                    };
                    optMenu = new OptionListMenu(
                        "Game Categories",
                        "Pick your installed CFW version to activate category folders to organize your games.",
                        items, SCREEN_WIDTH, SCREEN_HEIGHT
                    );
                    optMenu->setHeightOverride(175); // +15px for legacy checkbox
                    // Preselect current state
                    int sel = (!gclArkOn && !gclProOn) ? 0 : (gclArkOn ? 1 : 2);
                    optMenu->setSelected(sel);

                    std::string primaryRoot = gclPickDeviceRoot();
                    bool legacyCandidate = gclHasLegacyCandidatePrx(primaryRoot);
                    bool cbDisabled = !legacyCandidate && !gclLegacyMode;
                    optMenu->setCheckbox("Use my own existing category_lite.prx plugin",
                                         gclLegacyMode, cbDisabled);

                    // Prime & debounce so held X/O won't auto-activate the choice
                    SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
                    optMenu->primeButtons(now.Buttons);
                    inputWaitRelease = true;

                    rootPickGcl = true;
                    return;
                }


                if (dev == "__USB_MODE__") {
                    if (!gUsbActive) {
                        // Start drivers and activate mass storage when entering USB Mode.
                        UsbStartStacked();
                        UsbActivate();
                        gUsbActive = true;
                        gUsbShownConnected = false;
                        disableHomeAnimationForUsb();
                        markAllDevicesDirty();
                        gUsbBox = new MessageBox(
                            "Connect to PC...\nOn PSP Go, Bluetooth must be turned off in the System Settings.\nwarning.png Not Vita-compatible.",
                            circleIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "Disconnect",
                            10, 18, 60, 9, 280, 110, PSP_CTRL_CIRCLE);
                        gUsbBox->setOkAlignLeft(true);
                        gUsbBox->setOkPosition(10, 7);
                        gUsbBox->setOkStyle(0.7f, 0xFFBBBBBB);
                        gUsbBox->setOkTextOffset(-2, -1);
                        gUsbBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                        gUsbBox->setSubtitleGapAdjust(-8);
                        gUsbBox->setInlineIcon(warningIconTexture, "warning.png");
                    }
                    return; // don’t fall through to openDevice()
                }

                openDevice(dev);
                return;
            }
            // (other view handling continues…)



            if (view == View_Categories) {
                std::string nm = entries[selectedIndex].d_name;

                // Block "Category Settings" row
                if (nm == kCatSettingsLabel || nm == "__GCL_SETTINGS__") {
                    openGclSettingsScreen();
                    return;
                }

                // In sort mode: X = Pick/Drop (no opening)
                if (catSortMode) {
                    // Block non-movable rows: top "Category Settings" and bottom "Uncategorized"
                    if (!isCategoryRowLocked(selectedIndex)) {
                        if (!catPickActive) {
                            // Start a pick
                            catPickActive = true;
                            catPickIndex  = selectedIndex;
                        } else {
                            // Drop onto the new spot (swap visible rows)
                            if (catPickIndex >= 0 && catPickIndex < (int)entries.size() &&
                                selectedIndex  >= 0 && selectedIndex  < (int)entries.size() &&
                                catPickIndex != selectedIndex &&
                                !isCategoryRowLocked(catPickIndex) && !isCategoryRowLocked(selectedIndex)) {

                                std::swap(entries[catPickIndex], entries[selectedIndex]);
                                std::swap(entryPaths[catPickIndex], entryPaths[selectedIndex]);
                                std::swap(entryKinds[catPickIndex], entryKinds[selectedIndex]);
                            }
                            // End pick/drop cycle; stay in sort mode until SELECT
                            catPickActive = false;
                            catPickIndex  = -1;
                        }
                    }
                    return; // swallow X in sort mode
                }



                // Normal mode: open the category
                if (FIO_S_ISDIR(entries[selectedIndex].d_stat.st_mode)) {
                    openCategory(nm.c_str());
                }
            } else {
                moving = !moving;
            }



        }
        else if (pressed & PSP_CTRL_CIRCLE) {
            if (showRoots) {
                // nothing
            } else if (view == View_GclSettings) {
                MessageBox* retBox = pushReturningModal();
                if (gclBlacklistDirty) {
                    rescanCurrentDeviceAfterBlacklist();
                    gclBlacklistDirty = false;
                } else {
                    patchCategoryCacheFromSettings();
                }
                buildCategoryRows();      // Back to categories
                selectedIndex = 0;        // highlight "Category Settings"
                scrollOffset  = 0;        // ensure it's visible at the top
                popModal(retBox);
                inputWaitRelease = true;
                return;                   // stop further input from this frame
            } else if (view == View_CategoryContents) {
                if (moving) {
                    moving = false;
                } else {
                    checked.clear();
                    buildCategoryRows();

                    // Re-highlight the category we just left
                    // (match the *displayed* name exactly)
                    int idx = -1;
                    for (int i = 0; i < (int)entries.size(); ++i) {
                        if (!strcmp(entries[i].d_name, currentCategory.c_str())) { idx = i; break; }
                    }
                    if (idx < 0) idx = 0;
                    if (idx >= (int)entries.size()) idx = (int)entries.size() - 1;
                    selectedIndex = idx;

                    // Ensure visible
                    if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                    const int visible = categoryVisibleRows();
                    const int lastVisible = scrollOffset + visible - 1;
                    if (selectedIndex > lastVisible) scrollOffset = selectedIndex - (visible - 1);
                }
            } else if (view == View_AllFlat) {
                if (moving) {
                    moving = false;
                } else {
                    if (roots.size() > 1) checked.clear();
                    buildRootRows();
                }
            } else if (view == View_Categories) {
                if (moving) moving = false;
                else {
                    catSortMode = false;
                    catPickActive = false;
                    catPickIndex = -1;
                    MessageBox* retBox = pushReturningModal();
                    buildRootRows();
                    popModal(retBox);
                    inputWaitRelease = true;
                }
            }
        }



    }

    void run(){
        init();
        while (1) {
            renderOneFrame();

            // One-shot deferred boot migration:
            // show main screen first, then run legacy gclite_filter conversion behind a blocking modal.
            if (gclDeferredLegacyConvertPending &&
                showRoots && opPhase == OP_None &&
                actionMode == AM_None && !msgBox && !fileMenu && !optMenu)
            {
                const char* convertText = "Converting legacy gclite_filter.txt...";
                const float popScale = 1.0f;
                const int popPadX = 10;
                const int popPadY = 24;
                const int popLineH = (int)(24.0f * popScale + 0.5f);
                const float popTextW = measureTextWidth(popScale, convertText);
                const int popExtraW = 4;
                int popPanelW = (int)(popTextW + popPadX * 2 + popExtraW + 0.5f);
                popPanelW -= 6;
                const int popBottom = 14;
                const int popPanelH = popPadY + popLineH + popBottom - 24;
                const int popWrapTweak = 32;
                const int popForcedPxPerChar = 8;
                const int convertPanelH = popPanelH - 4; // match "Renaming..." item modal height
                const int convertPanelW = popPanelW + 4;
                msgBox = new MessageBox(convertText,
                                        nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, popScale, 0, "",
                                        popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                        convertPanelW, convertPanelH);
                renderOneFrame();
                gclAutoMigrateLegacyFilterOnBoot();
                gclDeferredLegacyConvertPending = false;
                delete msgBox; msgBox = nullptr;
                inputWaitRelease = true;
                continue;
            }

            // One-shot: after main screen + home animation are ready, hard-check plugin file if enabled
            if (!gclHardCheckDone && showRoots && opPhase == OP_None && !msgBox) {
                bool animReady = gHomeAnimEntries.empty();
                if (!animReady) {
                    Texture* animTex = getCurrentHomeAnimTexture();
                    animReady = animTex && animTex->data && animTex->width > 0 && animTex->height > 0;
                }
                if (animReady) {
                    gclHardCheckDone = true;
                    gclComputeInitial();
                    gclHardCheckPrxIfEnabled();
                }
            }

            // Global debounce: wait for full release before any modal eats input
            if (inputWaitRelease) {
                SceCtrlData pad{}; sceCtrlReadBufferPositive(&pad, 1);
                if (pad.Buttons != 0) continue;   // keep waiting
                inputWaitRelease = false;          // buttons now released
            }

            // Handle active dialogs
            if (msgBox) {
                if (!msgBox->update()) {
                    const bool canceled = msgBox->wasCanceled();
                    delete msgBox; msgBox = nullptr; inputWaitRelease = true;
                    msgBoxOwnedText.clear();

                    if (runningAppWarningPending != RAW_None) {
                        resolveCurrentAppActionWarning(canceled);
                        continue;
                    }

                    // If we just closed a confirmation, perform the chosen op now.
                    if (opPhase == OP_Confirm) {
                        if (canceled) {
                            if (opDestDevice == "__DELETE__" || opDestDevice == "__DEL_CAT__") {
                                opPhase = OP_None;
                                opDestDevice.clear();
                                opDestCategory.clear();
                                opSrcPaths.clear();
                                opSrcKinds.clear();
                                opSrcCount = 0;
                                opSrcTotalBytes = 0;
                                actionMode = AM_None;
                                continue;
                            }
                            // For Move/Copy confirms, return to the selection phase instead of canceling the op.
                            opDestCategory.clear();
                            const bool gclOn = (gclArkOn || gclProOn);
                            if (!gclOn) {
                                opPhase = OP_SelectDevice;
                                if (!showRoots) buildRootRowsForDevicePicker();
                            } else {
                                opPhase = showRoots ? OP_SelectDevice : OP_SelectCategory;
                            }
                            continue;
                        }
                        if (opDestDevice == "__DELETE__") {
                            ClockGuard cg; cg.boost333();
                            msgBox = new MessageBox("Deleting...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
                            renderOneFrame();
                            KfeFileOps::performDelete(this);
                            continue;
                        }
                        if (opDestDevice == "__DEL_CAT__") {
                            // Delete the CAT_ folder across ISO/GAME roots (all contents),
                            // per spec: non-game files don't affect the confirmation count and will just be removed.
                            ClockGuard cg; cg.boost333();
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
                            msgBox = new MessageBox("Deleting category...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                                    popScale, 0, "", popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                                    popPanelW + 80, popPanelH);
                            renderOneFrame();

                            std::string delCat = opDestCategory;
                            deleteCategoryDirs(currentDevice, delCat);
                            removeHiddenFiltersForDeletedCategory(delCat);

                            // Force a true repopulate pass (with Populating... modal) after category delete.
                            delete msgBox; msgBox = nullptr;
                            markDeviceDirty(currentDevice);
                            openDevice(currentDevice);

                            drawMessage("Category deleted", COLOR_GREEN);
                            sceKernelDelayThread(700*1000);

                            // Clear sentinel state
                            opDestDevice.clear();
                            opDestCategory.clear();
                            opPhase = OP_None;
                            continue;
                        }

                        if (actionMode == AM_Move) performMove();
                        else if (actionMode == AM_Copy) performCopy();
                    }
                } else {
                    continue; // keep modal
                }
            }

            // NEW: Categories Lite option picker (modal)
            // NEW: Categories Lite option picker (modal)
            else if (optMenu) {
                if (!optMenu->update()) {
                    const int pick = optMenu->choice();   // -1 if canceled
                    const bool deleteReq = optMenu->deleteRequested();
                    const GclSettingKey pending = gclPending;  // capture BEFORE clearing
                    const bool wasRootPick = rootPickGcl;       // capture BEFORE clearing
                    const bool cbToggled = optMenu->checkboxToggled();
                    const bool cbChecked = optMenu->checkboxChecked();
                    delete optMenu; optMenu = nullptr;
                    optMenuOwnedLabels.clear();
                    gclPending = GCL_SK_None;
                    if (wasRootPick) rootPickGcl = false;

                    // Handle legacy checkbox toggle (independent of CFW pick)
                    if (wasRootPick && cbToggled && cbChecked != gclLegacyMode) {
                        if (!cbChecked) {
                            // Unchecking legacy mode can trigger recursive conversion/merge.
                            // Keep UI responsive by showing a blocking progress modal.
                            const char* convertText = "Converting legacy gclite_filter.txt...";
                            const float popScale = 1.0f;
                            const int popPadX = 10;
                            const int popPadY = 24;
                            const int popLineH = (int)(24.0f * popScale + 0.5f);
                            const float popTextW = measureTextWidth(popScale, convertText);
                            const int popExtraW = 4;
                            int popPanelW = (int)(popTextW + popPadX * 2 + popExtraW + 0.5f);
                            popPanelW -= 6;
                            const int popBottom = 14;
                            const int popPanelH = popPadY + popLineH + popBottom - 24;
                            const int popWrapTweak = 32;
                            const int popForcedPxPerChar = 8;
                            const int convertPanelH = popPanelH - 4; // match "Renaming..." item modal height
                            const int convertPanelW = popPanelW + 4;
                            msgBox = new MessageBox(convertText,
                                                    nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, popScale, 0, "",
                                                    popPadX, popPadY, popWrapTweak, popForcedPxPerChar,
                                                    convertPanelW, convertPanelH);
                            renderOneFrame();
                            gclToggleLegacyMode(cbChecked);
                            delete msgBox; msgBox = nullptr;
                            inputWaitRelease = true;
                        } else {
                            gclToggleLegacyMode(cbChecked);
                        }
                        // If user only toggled checkbox and canceled CFW pick, refresh UI
                        if (pick < 0) {
                            rootKeepGclSelection = true;
                            buildRootRows();
                        }
                    }

                if (pending == GCL_SK_Blacklist && pick < 0) {
                    if (gclBlacklistDirty) {
                        // leave dirty flag set; will trigger full rescan when exiting settings
                    }
                    buildGclSettingsRowsFromState();
                    inputWaitRelease = true;
                    continue;
                    }

                    if (pick >= 0) {
                        if (wasRootPick) {
                            // Apply Off / ARK-4 / PRO/ME to the two back-end toggles
                            std::string pluginsSe;
                            std::string vshSe;
                            std::string plugins = gclFindArkPluginsFile(pluginsSe);
                            std::string vsh = gclFindProVshFile(vshSe);

                            const bool wantArk = (pick == 1);
                            const bool wantPro = (pick == 2);

                            std::string targetSeplugins;
                            std::string targetPrxPath;
                            bool needArkFile = false;
                            bool needProFile = false;
                            if (wantArk) needArkFile = true;
                            if (wantPro) needProFile = true;

                            if (wantArk) {
                                gclFindConfiguredPrxPathForBackend(true, /*enabledOnly=*/false, targetPrxPath);
                            }
                            if (targetPrxPath.empty() && wantPro) {
                                gclFindConfiguredPrxPathForBackend(false, /*enabledOnly=*/false, targetPrxPath);
                            }
                            if (targetPrxPath.empty()) {
                                gclFindConfiguredPrxPathAny(/*enabledOnly=*/false, wantArk, targetPrxPath);
                            }
                            if (targetPrxPath.empty()) targetPrxPath = gclPrxPath;
                            if (!targetPrxPath.empty()) {
                                gclPrxPath = targetPrxPath;
                                targetSeplugins = parentOf(targetPrxPath);
                            }

                            if (wantPro) {
                                if (targetSeplugins.empty()) targetSeplugins = vshSe;
                            } else if (wantArk) {
                                if (targetSeplugins.empty()) targetSeplugins = pluginsSe;
                            }
                            if (targetSeplugins.empty()) {
                                gclDevice = gclPickDeviceRoot();
                                targetSeplugins = gclSepluginsDirForRoot(gclDevice);
                                if (wantPro) {
                                    vsh = joinDirFile(targetSeplugins, "VSH.txt");
                                } else if (wantArk) {
                                    plugins = joinDirFile(targetSeplugins, "PLUGINS.txt");
                                }
                            } else {
                                gclDevice = rootPrefix(targetSeplugins);
                                if (wantPro && vsh.empty()) vsh = joinDirFile(targetSeplugins, "VSH.txt");
                                if (wantArk && plugins.empty()) plugins = joinDirFile(targetSeplugins, "PLUGINS.txt");
                            }
                            if (!dirExists(targetSeplugins)) sceIoMkdir(targetSeplugins.c_str(), 0777);

                            // Ensure the PRX is present if enabling either mode
                            if ((wantArk || wantPro) && !gclEnsurePrxPresent(targetSeplugins)) {
                                msgBox = new MessageBox("Could not install category_lite.prx from /resources.\nMake sure resources/category_lite.prx exists.",
                                                        nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14, PSP_CTRL_CIRCLE);
                            } else {
                                const bool pluginsExists = !plugins.empty() && pathExists(plugins);
                                const bool vshExists = !vsh.empty() && pathExists(vsh);
                                auto applyBackendStateOnce = [&]() -> bool {
                                    bool ok = true;

                                    // Only touch the file we actually need for the chosen mode
                                    if (needArkFile) ok &= gclWriteEnableToFileWithVerify(plugins, wantArk, /*arkPluginsTxt=*/true);
                                    if (needProFile) ok &= gclWriteEnableToFileWithVerify(vsh,     wantPro, /*arkPluginsTxt=*/false);

                                    // Ensure the other backend is disabled without creating empty files
                                    if (wantPro) {
                                        if (pluginsExists) ok &= gclWriteEnableToFileWithVerify(plugins, false, /*arkPluginsTxt=*/true);
                                    } else if (wantArk) {
                                        if (vshExists) ok &= gclWriteEnableToFileWithVerify(vsh, false, /*arkPluginsTxt=*/false);
                                    } else {
                                        if (pluginsExists) ok &= gclWriteEnableToFileWithVerify(plugins, false, /*arkPluginsTxt=*/true);
                                        if (vshExists)     ok &= gclWriteEnableToFileWithVerify(vsh,     false, /*arkPluginsTxt=*/false);
                                    }

                                    // Recompute from disk and verify state actually matches request.
                                    gclComputeInitial();
                                    if (gclArkOn != wantArk || gclProOn != wantPro) ok = false;
                                    return ok;
                                };

                                bool backendOk = applyBackendStateOnce();
                                if (!backendOk) {
                                    // One additional pass for intermittent I/O lag on some setups.
                                    sceKernelDelayThread(12 * 1000);
                                    backendOk = applyBackendStateOnce();
                                }

                                if (!backendOk) {
                                    msgBox = new MessageBox(
                                        "Error\n"
                                        "Could not verify plugin update. Please try again.",
                                        okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "Close",
                                        10, 18, 80, 9, 280, 102, PSP_CTRL_CROSS);
                                    msgBox->setOkAlignLeft(true);
                                    msgBox->setOkPosition(10, 7);
                                    msgBox->setOkStyle(0.7f, 0xFFBBBBBB);
                                    msgBox->setOkTextOffset(-2, -1);
                                    msgBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                                    msgBox->setSubtitleGapAdjust(-8);
                                } else {
                                    gclArkOn = wantArk;
                                    gclProOn = wantPro;
                                    rootKeepGclSelection = true;
                                    buildRootRows();   // reflect new state immediately
                                }
                            }
                        } else {
                            const uint32_t prevCatsort = gclCfg.catsort;
                            // Existing in-plugin settings pickers
                            switch (pending) {
                                case GCL_SK_Mode:   gclCfg.mode = (uint32_t)pick; break;
                                case GCL_SK_Prefix: gclCfg.prefix = (uint32_t)pick; break;
                                case GCL_SK_Uncat:  gclCfg.uncategorized = (uint32_t)pick; break;
                                case GCL_SK_Sort:   gclCfg.catsort = (uint32_t)pick; break;
                                case GCL_SK_Blacklist: {
                                    if (deleteReq && pick > 0) {
                                        // Triangle on existing item → delete
                                        deleteBlacklistAtIndex(pick - 1);
                                        openBlacklistModal(pick);
                                        continue;
                                    }

                                    if (pick > 0) {
                                        // X on existing item → rename via OSK
                                        gclLoadBlacklistFor(currentDevice);
                                        auto& blRen = gclBlacklistMap[blacklistRootKey(currentDevice)];
                                        int idx = pick - 1;
                                        if (idx >= 0 && idx < (int)blRen.size()) {
                                            std::string typed;
                                            if (promptTextOSK("Rename blacklist item", blRen[idx].c_str(), 64, typed)) {
                                                typed = normalizeBlacklistInput(typed);
                                                if (!typed.empty() && strcasecmp(typed.c_str(), blRen[idx].c_str()) != 0) {
                                                    // Check for duplicate
                                                    bool dup = false;
                                                    for (const auto& w : blRen) {
                                                        if (!strcasecmp(w.c_str(), typed.c_str())) { dup = true; break; }
                                                    }
                                                    if (!dup) {
                                                        blRen[idx] = typed;
                                                        gclBlacklistDirty = true;
                                                    }
                                                }
                                            }
                                        }
                                        openBlacklistModal(pick);
                                        continue;
                                    }

                                    if (pick == 0) {
                                        // X on "Add..." → add new item via OSK
                                        std::string typed;
                                        if (!promptTextOSK("Add blacklist item", "", 64, typed)) {
                                            openBlacklistModal(0);
                                            continue;
                                        }
                                        typed = normalizeBlacklistInput(typed);
                                        if (typed.empty()) {
                                            openBlacklistModal(0);
                                            continue;
                                        }
                                        gclLoadBlacklistFor(currentDevice);
                                        auto& blDup = gclBlacklistMap[blacklistRootKey(currentDevice)];
                                        bool dup = false;
                                        for (const auto& w : blDup) {
                                            if (!strcasecmp(w.c_str(), typed.c_str())) { dup = true; break; }
                                        }
                                        if (dup) {
                                            openBlacklistModal(0);
                                            continue;
                                        }

                                        auto& bl = gclBlacklistMap[blacklistRootKey(currentDevice)];
                                        gclLoadBlacklistFor(currentDevice);
                                        bl.push_back(typed);
                                        gclBlacklistDirty = true;
                                        openBlacklistModal(0);
                                        continue;
                                    }
                                    break;
                                }
                                default: break;
                            }
                            if (pending != GCL_SK_Blacklist) {
                                gclSaveConfig();

                                // If Prefix or Sort changed, apply immediately and refresh caches
                                if (pending == GCL_SK_Prefix || pending == GCL_SK_Sort) {
                                    const bool sortTurnedOff =
                                        (pending == GCL_SK_Sort && prevCatsort != 0 && gclCfg.catsort == 0);
                                    const bool forceStripNumbers = sortTurnedOff;
                                    // Clear run-once guard so future opens are allowed to re-enforce if needed
                                    gclSchemeApplied.erase(rootPrefix(currentDevice));
                                    s_catNamingEnforced.erase(rootPrefix(currentDevice));

                                    enforceCategorySchemeForDevice(currentDevice, forceStripNumbers);
                                    const bool onMs0 = (strncasecmp(currentDevice.c_str(), "ms0:/", 5) == 0);
                                    if (isPspGo()) {
                                        // Also clear & enforce the opposite root once to keep ms0:/ and ef0:/ consistent
                                        std::string other = onMs0 ? std::string("ef0:/") : std::string("ms0:/");
                                        gclSchemeApplied.erase(rootPrefix(other));
                                        s_catNamingEnforced.erase(rootPrefix(other));
                                        enforceCategorySchemeForDevice(other, forceStripNumbers);
                                    }

                                    // If prefix was just disabled, immediately bring back blacklisted bases
                                    if (!blacklistActive()) {
                                        const auto& blNow = gclBlacklistMap[blacklistRootKey(currentDevice)];
                                        refreshCategoriesForBases(currentDevice, blNow);
                                        if (isPspGo()) {
                                            std::string other = onMs0 ? std::string("ef0:/") : std::string("ms0:/");
                                            markDeviceDirty(other);
                                        }
                                        gclPendingUnblacklistMap[blacklistRootKey(currentDevice)].clear();
                                    }

                                    // Keep in-memory cache consistent with on-disk names, preserving ICON0 paths
                                    patchCategoryCacheFromSettings(forceStripNumbers);
                                    refreshGclFilterFile();
                                }
                            }

                            buildGclSettingsRowsFromState();

                        }
                    }
                    inputWaitRelease = true;
                    continue;   // keep modal behavior consistent
                } else {
                    continue;   // still open; skip normal input
                }
            }



            // File ops menu (modal)
            if (fileMenu) {
                if (!fileMenu->update()) {
                    int choice = fileMenu->choice();
                    delete fileMenu; fileMenu = nullptr; inputWaitRelease = true;

                    if (menuContext == MC_ContentOps) {
                        // 0=Move,1=Copy,2=Hide/Unhide,3=Delete
                        if (choice == 0)      startAction(AM_Move);
                        else if (choice == 1) startAction(AM_Copy);
                        else if (choice == 2) {
                            std::vector<std::string> hidePaths;
                            if (!checked.empty()) {
                                for (const auto& p : checked) hidePaths.push_back(p);
                            } else if (selectedIndex >= 0 && selectedIndex < (int)workingList.size()) {
                                hidePaths.push_back(workingList[selectedIndex].path);
                            }

                            if (hidePaths.empty()) {
                                msgBox = new MessageBox("Nothing to hide/unhide.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                            } else {
                                int hiddenCount = 0, unhiddenCount = 0;
                                for (const auto& p : hidePaths) {
                                    if (isGameFilteredPath(p)) hiddenCount++;
                                    else unhiddenCount++;
                                }
                                const bool hideInXmb = (unhiddenCount >= hiddenCount);
                                setGameHiddenForPaths(hidePaths, hideInXmb);
                                const int count = (int)hidePaths.size();
                                const char* verb = hideInXmb ? "Hidden" : "Unhidden";
                                char msg[96];
                                snprintf(msg, sizeof(msg), "%s %d %s.", verb, count, (count == 1) ? "app" : "apps");
                                drawMessage(msg, hideInXmb ? COLOR_CYAN : COLOR_GREEN);
                                sceKernelDelayThread(600*1000);
                            }
                        } else if (choice == 3) {
                            // (existing content delete flow)
                            std::vector<std::string> delPaths;
                            std::vector<GameItem::Kind> delKinds;
                            if (!checked.empty()) {
                                for (auto &p : checked) {
                                    delPaths.push_back(p);
                                    GameItem::Kind k = GameItem::ISO_FILE;
                                    for (auto &gi : workingList) if (gi.path == p) { k = gi.kind; break; }
                                    delKinds.push_back(k);
                                }
                            } else if (selectedIndex >= 0 && selectedIndex < (int)workingList.size()) {
                                delPaths.push_back(workingList[selectedIndex].path);
                                delKinds.push_back(workingList[selectedIndex].kind);
                            }

                            if (delPaths.empty()) {
                                msgBox = new MessageBox("Nothing to delete.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                            } else {
                                const int count = (int)delPaths.size();
                                const char* title = (count == 1) ? "Delete App" : "Delete Apps";
                                char subtitle[96];
                                snprintf(subtitle, sizeof(subtitle),
                                         "Delete %d %s?", count, (count == 1) ? "app" : "apps");
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
                                opSrcPaths = delPaths; opSrcKinds = delKinds;
                                actionMode = AM_None;
                                opPhase    = OP_Confirm;
                                opDestDevice = "__DELETE__";
                            }
                        }
                    } else {
// MC_CategoryOps: 0=New, 1=Hide/Unhide, 2=Delete
                        if (choice == 0) {
                            // New category: OSK → create across roots
                            std::string typed;
                            if (!promptTextOSK("New Category", "", 64, typed)) {
                                // cancelled → nothing
                            } else {
                                // Treat as BASE name (strip any CAT_/XX they typed)
                                typed = sanitizeFilename(stripCategoryPrefixes(typed));
                                if (isBlacklistedBaseNameFor(currentDevice, typed)) {
                                    drawMessage("Blacklisted name", COLOR_RED);
                                    sceKernelDelayThread(600*1000);
                                    continue;
                                }

                                const char* createText = "Creating...";
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
                                const int createPanelW = popPanelW + 4;
                                const int createPanelH = popPanelH;
                                msgBox = new MessageBox(createText, nullptr, SCREEN_WIDTH, SCREEN_HEIGHT,
                                                        popScale, 0, "", popPadX, popPadY,
                                                        popWrapTweak, popForcedPxPerChar,
                                                        createPanelW, createPanelH);
                                renderOneFrame();

                                // Create as BASE across roots…
                                createCategoryDirs(currentDevice, typed);
                                // Apply CAT_/XX scheme to category directories
                                enforceCategorySchemeForDevice(currentDevice);

                                // Compute the *display* name we will now use for this category
                                std::string newDisplay = findDisplayNameForCategoryBase(currentDevice, typed);

                                // Patch in-memory caches (icon paths, no-icon memo set, selection key)
                                // to include the new (empty) category, instead of renaming the current one
                                if (!newDisplay.empty()) {
                                    cachePatchAddCategory(newDisplay);
                                }

                                // Invalidate per-device “scheme applied” so initial-load enforcement can run again
                                gclSchemeApplied.erase(rootPrefix(currentDevice));
                                s_catNamingEnforced.erase(rootPrefix(currentDevice));
                                if (isPspGo()) {
                                    // Be safe on Go: clear both roots so next load can re-apply where needed
                                    gclSchemeApplied.erase(std::string("ms0:/"));
                                    gclSchemeApplied.erase(std::string("ef0:/"));
                                    s_catNamingEnforced.erase(std::string("ms0:/"));
                                    s_catNamingEnforced.erase(std::string("ef0:/"));
                                }

                                buildCategoryRows();
                                delete msgBox; msgBox = nullptr;

                                auto sameBase = [&](const char* disp){
                                    return !strcasecmp(stripCategoryPrefixes(disp).c_str(), typed.c_str());
                                };
                                int idx = -1;
                                for (int i = 0; i < (int)entries.size(); ++i) {
                                    if (sameBase(entries[i].d_name)) { idx = i; break; }
                                }
                                if (idx >= 0) {
                                    selectedIndex = idx;
                                    const int visible = categoryVisibleRows();
                                    scrollOffset = (idx >= visible) ? (idx - visible + 1) : 0;
                                }
                                drawMessage("Category created", COLOR_GREEN);
                                sceKernelDelayThread(600*1000);
                            }

                        } else if (choice == 1) {
                            if (selectedIndex < 0 || selectedIndex >= (int)entries.size() ||
                                isCategoryRowLocked(selectedIndex)) {
                                msgBox = new MessageBox("Pick a category folder.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                            } else {
                                const char* nm = entries[selectedIndex].d_name;
                                const std::string base = stripCategoryPrefixes(nm);
                                const bool hideInXmb = !isFilteredBaseName(base);
                                setCategoryHidden(nm, hideInXmb);
                                const char* verb = hideInXmb ? "Hidden" : "Unhidden";
                                char msg[96];
                                snprintf(msg, sizeof(msg), "%s category in XMB.", verb);
                                drawMessage(msg, hideInXmb ? COLOR_CYAN : COLOR_GREEN);
                                sceKernelDelayThread(600*1000);
                            }
                        } else if (choice == 2) {
                            // Delete category: count games first and confirm
                            if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) {
                                msgBox = new MessageBox("No category selected.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                            } else if (isCategoryRowLocked(selectedIndex)) {
                                // Don't allow deleting "Category Settings" or "Uncategorized"
                                msgBox = new MessageBox("Pick a category folder.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                            } else {
                                std::string cat = entries[selectedIndex].d_name;

                                int games = countGamesInCategory(currentDevice, cat);
                                char subtitle[128];
                                if (games > 0) {
                                    snprintf(subtitle, sizeof(subtitle),
                                             "%d app(s) are in this category and will be deleted.",
                                             games);
                                } else {
                                    snprintf(subtitle, sizeof(subtitle),
                                             "Delete empty category?");
                                }
                                msgBoxOwnedText = std::string("Delete Category\n") + subtitle;
                                msgBox = new MessageBox(msgBoxOwnedText.c_str(), okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT,
                                                        0.9f, 15, "Confirm",
                                                        10, 18, 82, 9,
                                                        240, 90);
                                msgBox->setOkAlignLeft(true);
                                msgBox->setOkPosition(10, 7);
                                msgBox->setOkStyle(0.7f, 0xFFBBBBBB);
                                msgBox->setOkTextOffset(-2, -1);
                                msgBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                                msgBox->setSubtitleGapAdjust(-6);
                                msgBox->setCancel(circleIconTexture, "Cancel", PSP_CTRL_CIRCLE);

                                // Reuse the confirm-close hook, sentinel device + stash cat in opDestCategory
                                actionMode = AM_None;
                                opPhase    = OP_Confirm;
                                opDestDevice   = "__DEL_CAT__";
                                opDestCategory = cat;
                            }
                        }
                    }
                } else {
                    continue; // keep menu modal
                }
            }


            handleInput();
        }
    }

};
