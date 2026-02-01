
// --- static member definitions (moved out of class) ---
KernelFileExplorer::GclConfig KernelFileExplorer::gclCfg = {0,0,0,0,0};
bool KernelFileExplorer::gclCfgLoaded = false;
std::unordered_map<std::string, std::vector<std::string>> KernelFileExplorer::gclBlacklistMap;
std::unordered_map<std::string, bool> KernelFileExplorer::gclBlacklistLoadedMap;
std::unordered_map<std::string, std::vector<std::string>> KernelFileExplorer::gclPendingUnblacklistMap;
std::unordered_map<std::string, std::vector<std::string>> KernelFileExplorer::gclFilterMap;
std::unordered_map<std::string, bool> KernelFileExplorer::gclFilterLoadedMap;
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
    std::string fsDriverPath = baseDir + "fs_driver.prx";
    if (LoadStartModule(fsDriverPath.c_str()) < 0) {
        LoadStartModule("fs_driver.prx");
    }
    SetupCallbacks();

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
    std::string animRoot   = baseDir + "resources/animations";

    backgroundTexture      = texLoadPNG(pngPath.c_str());
    okIconTexture          = texLoadPNG(crossPath.c_str());
    circleIconTexture      = texLoadPNG(circlePath.c_str());
    triangleIconTexture    = texLoadPNG(trianglePath.c_str());
    squareIconTexture      = texLoadPNG(squarePath.c_str());
    selectIconTexture      = texLoadPNG(selectPath.c_str());
    startIconTexture       = texLoadPNG(startPath.c_str());
    placeholderIconTexture = texLoadPNG(icon0Path.c_str());
    checkTexUnchecked = texLoadPNG(boxOffPath.c_str());
    checkTexChecked   = texLoadPNG(boxOnPath.c_str());
    rootMemIcon       = texLoadPNG(memPath.c_str());
    rootInternalIcon  = texLoadPNG(intPath.c_str());
    rootUsbIcon       = texLoadPNG(usbPath.c_str());
    rootCategoriesIcon= texLoadPNG(catPath.c_str());
    rootArk4Icon      = texLoadPNG(ark4Path.c_str());
    rootProMeIcon     = texLoadPNG(proPath.c_str());
    rootOffBulbIcon   = texLoadPNG(offPath.c_str());
    catFolderIcon     = texLoadPNG(folderPath.c_str());
    catFolderIconGray = texLoadPNG(folderGrayPath.c_str());
    catSettingsIcon   = texLoadPNG(catSettingsPath.c_str());
    blacklistIcon     = texLoadPNG(blacklistPath.c_str());
    lIconTexture      = texLoadPNG(lPath.c_str());
    rIconTexture      = texLoadPNG(rPath.c_str());
    memcardSmallIcon  = texLoadPNG(memSmallPath.c_str());
    internalSmallIcon = texLoadPNG(intSmallPath.c_str());
    ps1IconTexture    = texLoadPNG(ps1Path.c_str());
    homebrewIconTexture = texLoadPNG(homebrewPath.c_str());
    isoIconTexture    = texLoadPNG(isoPath.c_str());
    updateIconTexture = texLoadPNG(updatePath.c_str());

    initHomeAnimations(animRoot);

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

    if (backgroundTexture && backgroundTexture->data) {
        gOskBgColorABGR = computeDominantColorABGRFromTexture(backgroundTexture);
    }

    if (!backgroundTexture) {
        pspDebugScreenInit();
        pspDebugScreenPrintf("PNG load failed at:\n  %s\n", pngPath.c_str());
        sceKernelDelayThread(800 * 1000);
    }

    KernelFileExplorer app;
    app.run();
    return 0;
}
