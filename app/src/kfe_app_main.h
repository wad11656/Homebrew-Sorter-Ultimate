
// --- static member definitions (moved out of class) ---
KernelFileExplorer::GclConfig KernelFileExplorer::gclCfg = {0,0,0,0,0};
bool KernelFileExplorer::gclCfgLoaded = false;
std::unordered_map<std::string, std::vector<std::string>> KernelFileExplorer::gclBlacklistMap;
std::unordered_map<std::string, std::vector<std::string>> KernelFileExplorer::gclPendingUnblacklistMap;
std::unordered_map<std::string, std::vector<std::string>> KernelFileExplorer::gclCategoryFilterMap;
std::unordered_map<std::string, std::vector<std::string>> KernelFileExplorer::gclGameFilterMap;
bool KernelFileExplorer::gclFiltersLoaded = false;
bool KernelFileExplorer::gclFiltersScrubbed = false;
KernelFileExplorer::GclSettingKey KernelFileExplorer::gclPending = KernelFileExplorer::GCL_SK_None;
bool KernelFileExplorer::rootPickGcl = false;   // ← add this definition
bool KernelFileExplorer::rootKeepGclSelection = false;


// Load & start fs_driver.prx (needed for kernel-level IO wrappers).
int LoadStartModule(const char *path) {
    SceUID m = kuKernelLoadModule(path, 0, NULL);
    if (m < 0) {
        logInit();
        logf("kuKernelLoadModule(%s) failed: 0x%08X", path, (unsigned)m);
        logClose();
        return m;
    }
    int st = 0;
    int rc = sceKernelStartModule(m, 0, NULL, &st, NULL);
    if (rc < 0) {
        logInit();
        logf("sceKernelStartModule(%s) failed: 0x%08X", path, (unsigned)rc);
        logClose();
        return rc;
    }
    return m;
}

int RunKernelFileExplorer(const char* execPath) {
    gExecPath = execPath;
    std::string baseDir = getBaseDir(execPath);
    gLogBaseDir = baseDir;
    logInit();
    logf("boot: RunKernelFileExplorer start");
    logf("boot: execPath=%s", execPath ? execPath : "(null)");
    logf("boot: baseDir=%s", baseDir.c_str());
    std::string fsDriverPath = baseDir + "fs_driver.prx";
    logf("boot: LoadStartModule %s", fsDriverPath.c_str());
    if (LoadStartModule(fsDriverPath.c_str()) < 0) {
        logf("boot: LoadStartModule failed, retrying fs_driver.prx");
        LoadStartModule("fs_driver.prx");
    }
    logf("boot: SetupCallbacks");
    SetupCallbacks();

    auto logTex = [](const char* label, const std::string& p, Texture* t) {
        logf("boot: load %s %s -> %p", label, p.c_str(), (void*)t);
    };

    std::string pngPath  = baseDir + "resources/bkg.png";
    std::string crossPath= baseDir + "resources/cross.png";
    std::string circlePath= baseDir + "resources/circle.png";
    std::string trianglePath= baseDir + "resources/triangle.png";
    std::string squarePath= baseDir + "resources/square.png";
    std::string selectPath  = baseDir + "resources/select.png";   // used for SELECT icon
    std::string startPath   = baseDir + "resources/start.png";
    std::string icon0Path= baseDir + "resources/icon0.png";
    std::string boxOffPath = baseDir + "resources/unchecked.png";
    std::string boxOnPath  = baseDir + "resources/checked.png";
    std::string memPath    = baseDir + "resources/memcard_40h.png";
    std::string intPath    = baseDir + "resources/internal_40h.png";
    std::string usbPath    = baseDir + "resources/usb_21h.png";
    std::string catPath    = baseDir + "resources/categories_25h.png";
    std::string ark4Path   = baseDir + "resources/ark4_small_border_18h.png";
    std::string proPath    = baseDir + "resources/pro_me_18h.png";
    std::string offPath    = baseDir + "resources/off_bulb_18h.png";
    std::string folderPath = baseDir + "resources/folder.png";
    std::string folderGrayPath = baseDir + "resources/folder_grayscale.png";
    std::string catSettingsPath = baseDir + "resources/categoriessettings_15h.png";
    std::string blacklistPath = baseDir + "resources/blacklist.png";
    std::string lPath      = baseDir + "resources/L.png";
    std::string rPath      = baseDir + "resources/R.png";
    std::string memSmallPath = baseDir + "resources/memcard_small.png";
    std::string intSmallPath = baseDir + "resources/internal_small.png";
    std::string ps1Path    = baseDir + "resources/ps1.png";
    std::string homebrewPath = baseDir + "resources/homebrew.png";
    std::string isoPath    = baseDir + "resources/iso.png";
    std::string updatePath = baseDir + "resources/update.png";
    std::string ps1GrayPath    = baseDir + "resources/ps1_grayscale.png";
    std::string homebrewGrayPath = baseDir + "resources/homebrew_grayscale.png";
    std::string isoGrayPath    = baseDir + "resources/iso_grayscale.png";
    std::string updateGrayPath = baseDir + "resources/update_grayscale.png";
    std::string warningPath = baseDir + "resources/warning.png";
    std::string updownPath = baseDir + "resources/updown.png";
    std::string animRoot   = baseDir + "resources/animations";

    backgroundTexture      = texLoadPNG(pngPath.c_str()); logTex("background", pngPath, backgroundTexture);
    okIconTexture          = texLoadPNG(crossPath.c_str()); logTex("ok", crossPath, okIconTexture);
    circleIconTexture      = texLoadPNG(circlePath.c_str()); logTex("circle", circlePath, circleIconTexture);
    triangleIconTexture    = texLoadPNG(trianglePath.c_str()); logTex("triangle", trianglePath, triangleIconTexture);
    squareIconTexture      = texLoadPNG(squarePath.c_str()); logTex("square", squarePath, squareIconTexture);
    selectIconTexture      = texLoadPNG(selectPath.c_str()); logTex("select", selectPath, selectIconTexture);
    startIconTexture       = texLoadPNG(startPath.c_str()); logTex("start", startPath, startIconTexture);
    placeholderIconTexture = texLoadPNG(icon0Path.c_str()); logTex("icon0", icon0Path, placeholderIconTexture);
    checkTexUnchecked = texLoadPNG(boxOffPath.c_str()); logTex("check_off", boxOffPath, checkTexUnchecked);
    checkTexChecked   = texLoadPNG(boxOnPath.c_str()); logTex("check_on", boxOnPath, checkTexChecked);
    rootMemIcon       = texLoadPNG(memPath.c_str()); logTex("root_mem", memPath, rootMemIcon);
    rootInternalIcon  = texLoadPNG(intPath.c_str()); logTex("root_internal", intPath, rootInternalIcon);
    rootUsbIcon       = texLoadPNG(usbPath.c_str()); logTex("root_usb", usbPath, rootUsbIcon);
    rootCategoriesIcon= texLoadPNG(catPath.c_str()); logTex("root_categories", catPath, rootCategoriesIcon);
    rootArk4Icon      = texLoadPNG(ark4Path.c_str()); logTex("root_ark4", ark4Path, rootArk4Icon);
    rootProMeIcon     = texLoadPNG(proPath.c_str()); logTex("root_pro", proPath, rootProMeIcon);
    rootOffBulbIcon   = texLoadPNG(offPath.c_str()); logTex("root_off", offPath, rootOffBulbIcon);
    catFolderIcon     = texLoadPNG(folderPath.c_str()); logTex("cat_folder", folderPath, catFolderIcon);
    catFolderIconGray = texLoadPNG(folderGrayPath.c_str()); logTex("cat_folder_gray", folderGrayPath, catFolderIconGray);
    catSettingsIcon   = texLoadPNG(catSettingsPath.c_str()); logTex("cat_settings", catSettingsPath, catSettingsIcon);
    blacklistIcon     = texLoadPNG(blacklistPath.c_str()); logTex("blacklist", blacklistPath, blacklistIcon);
    lIconTexture      = texLoadPNG(lPath.c_str()); logTex("L", lPath, lIconTexture);
    rIconTexture      = texLoadPNG(rPath.c_str()); logTex("R", rPath, rIconTexture);
    memcardSmallIcon  = texLoadPNG(memSmallPath.c_str()); logTex("mem_small", memSmallPath, memcardSmallIcon);
    internalSmallIcon = texLoadPNG(intSmallPath.c_str()); logTex("int_small", intSmallPath, internalSmallIcon);
    ps1IconTexture    = texLoadPNG(ps1Path.c_str()); logTex("ps1", ps1Path, ps1IconTexture);
    homebrewIconTexture = texLoadPNG(homebrewPath.c_str()); logTex("homebrew", homebrewPath, homebrewIconTexture);
    isoIconTexture    = texLoadPNG(isoPath.c_str()); logTex("iso", isoPath, isoIconTexture);
    updateIconTexture = texLoadPNG(updatePath.c_str()); logTex("update", updatePath, updateIconTexture);
    ps1IconTextureGray    = texLoadPNG(ps1GrayPath.c_str()); logTex("ps1_gray", ps1GrayPath, ps1IconTextureGray);
    homebrewIconTextureGray = texLoadPNG(homebrewGrayPath.c_str()); logTex("homebrew_gray", homebrewGrayPath, homebrewIconTextureGray);
    isoIconTextureGray    = texLoadPNG(isoGrayPath.c_str()); logTex("iso_gray", isoGrayPath, isoIconTextureGray);
    updateIconTextureGray = texLoadPNG(updateGrayPath.c_str()); logTex("update_gray", updateGrayPath, updateIconTextureGray);
    warningIconTexture = texLoadPNG(warningPath.c_str()); logTex("warning", warningPath, warningIconTexture);
    updownIconTexture = texLoadPNG(updownPath.c_str()); logTex("updown", updownPath, updownIconTexture);

    logf("boot: initHomeAnimations %s", animRoot.c_str());
    initHomeAnimations(animRoot);
    logf("boot: initHomeAnimations done");

    if (gEnablePopAnimations) {
        if (dirExists(animRoot)) {
            bool prefUsed = false;
            if (POP_ANIM_PREF && POP_ANIM_PREF[0]) {
                std::string animDir = joinDirFile(animRoot, POP_ANIM_PREF);
                if (dirExists(animDir)) {
                    gPopAnimDirs.push_back(animDir);
                    prefUsed = true;
                }
            }
            if (!prefUsed) {
                forEachEntry(animRoot, [&](const SceIoDirent& e){
                    if (FIO_S_ISDIR(e.d_stat.st_mode)) {
                        gPopAnimDirs.push_back(joinDirFile(animRoot, e.d_name));
                    }
                });
            }
            if (!gPopAnimDirs.empty()) shufflePopAnimOrder();
        }
    }

    logf("boot: computeDominantColor");
    if (backgroundTexture && backgroundTexture->data) {
        gOskBgColorABGR = computeDominantColorABGRFromTexture(backgroundTexture);
    }
    logf("boot: computeDominantColor done");

    if (!backgroundTexture) {
        pspDebugScreenInit();
        pspDebugScreenPrintf("PNG load failed at:\n  %s\n", pngPath.c_str());
        sceKernelDelayThread(800 * 1000);
    }

    logf("boot: construct app");
    KernelFileExplorer app;
    logf("boot: app.run");
    app.run();
    return 0;
}
