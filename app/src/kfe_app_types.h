struct HomeAnimEntry {
    std::string dir;
    std::string creditFile;
};
static std::vector<HomeAnimEntry> gHomeAnimEntries;
static int gHomeAnimIndex = -1;
static std::vector<MBAnimFrame> gHomeAnimFrames;
static size_t gHomeAnimFrameIndex = 0;
static unsigned long long gHomeAnimNextUs = 0;
static unsigned long long gHomeAnimMinDelayUs = 0;
static const int CHECKBOX_PX      = 11;

// Reserve ~4–5 chars for sizes (e.g., "123M") so it clears the left tag.
static const int SIZE_FIELD_RIGHT_X = 72;               // <--- moved right

// Push checkbox to the right of the size field (add a little gap)
static const int CHECKBOX_X       = SIZE_FIELD_RIGHT_X + 10; // ~136px   <--- moved right
static const int CHECKBOX_Y_NUDGE = -6;

// Start filename a bit after the checkbox
static const int NAME_TEXT_X      = CHECKBOX_X + 14;     // ~154px     <--- new

// --- one-shot guard to avoid re-enforcing category naming more than once per root
static std::unordered_set<std::string> s_catNamingEnforced;

// NEW: run-once guard so we don't re-enforce on every return to the category list
static std::unordered_set<std::string> gclSchemeApplied; // keys like "ms0:/" or "ef0:/"

static std::string oppositeRootOf(const std::string& dev){
    if (strncasecmp(dev.c_str(), "ms0:/", 5) == 0) return "ef0:/";
    if (strncasecmp(dev.c_str(), "ef0:/", 5) == 0) return "ms0:/";
    return "";
}

// ===== Exit callback (HOME menu) =====
static int ExitCallback(int, int, void*) { sceKernelExitGame(); return 0; }
static int CallbackThread(SceSize, void*) {
    int cb = sceKernelCreateCallback("ExitCallback", ExitCallback, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}
static void SetupCallbacks() {
    int th = sceKernelCreateThread("CallbackThread", CallbackThread, 0x11, 0x1000, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}

static constexpr bool kfeLoggingEnabled = false;
static SceUID gLogFd = -1;
static inline void trimTrailingSpaces(char* s);
static void logInit() {
    if (!kfeLoggingEnabled) return;
    if (gLogFd >= 0) return;
    gLogFd = sceIoOpen("ms0:/KFE_move.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    if (gLogFd < 0) gLogFd = sceIoOpen("ef0:/KFE_move.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
}
static void logWrite(const char* s) { if (kfeLoggingEnabled && gLogFd >= 0) sceIoWrite(gLogFd, s, (int)strlen(s)); }
static void logf(const char* fmt, ...) {
    if (!kfeLoggingEnabled) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logWrite(buf); logWrite("\r\n");
}
static void logClose(){ if (kfeLoggingEnabled && gLogFd >= 0) { sceIoClose(gLogFd); gLogFd = -1; } }

// Fallback to user-mode dir I/O if kernel bridge fails on some CFWs (LME).
static std::unordered_set<SceUID> gUserDirHandles;
static SceUID kfeIoOpenDir(const char* path) {
    SceUID d = pspIoOpenDir(path);
    if (d >= 0) return d;
    SceUID ud = sceIoDopen(path);
    if (ud >= 0) gUserDirHandles.insert(ud);
    return ud;
}
static int kfeIoReadDir(SceUID dir, SceIoDirent* ent) {
    if (gUserDirHandles.find(dir) != gUserDirHandles.end())
        return sceIoDread(dir, ent);
    return pspIoReadDir(dir, ent);
}
static int kfeIoCloseDir(SceUID dir) {
    if (gUserDirHandles.erase(dir))
        return sceIoDclose(dir);
    return pspIoCloseDir(dir);
}

// Debug: log a short directory listing when a scan comes back empty.
static void logDirSample(const std::string& path, int maxEntries = 20) {
    SceUID d = kfeIoOpenDir(path.c_str());
    if (d < 0) { logf("scanlog: open %s failed %d", path.c_str(), d); return; }
    logf("scanlog: listing %s", path.c_str());
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    int count = 0;
    while (kfeIoReadDir(d, &ent) > 0 && count < maxEntries) {
        trimTrailingSpaces(ent.d_name);
        const int len = (int)strlen(ent.d_name);
        logf("  [%d] mode=0x%X len=%d name='%s'", count,
             (unsigned)ent.d_stat.st_mode, len, ent.d_name);
        memset(&ent, 0, sizeof(ent));
        ++count;
    }
    if (count == 0) logf("  (no entries)");
    kfeIoCloseDir(d);
}

static void logEmptyScanOnce(const std::string& root) {
    static std::unordered_set<std::string> seen;
    if (!seen.insert(root).second) return;
    logInit();
    logf("scanlog: empty scan for %s", root.c_str());
    logDirSample(root + "PSP/GAME/");
    logDirSample(root + "ISO/");
    logClose();
}


// --- path/device helpers ---
static inline bool sameDevice(const std::string& a, const std::string& b) {
    if (a.size() < 4 || b.size() < 4) return false;
    return strncasecmp(a.c_str(), b.c_str(), 4) == 0;   // "ms0:" vs "ef0:"
}
static inline std::string parentOf(const std::string& p) { return dirnameOf(p); }

// --- stat/exists ---
static inline bool pathExists(const std::string& p, SceIoStat* out=nullptr) {
    SceIoStat st{}; int rc = sceIoGetstat(p.c_str(), &st);
    if (rc >= 0 && out) *out = st;
    return rc >= 0;
}
static inline bool isDirMode(const SceIoStat& st){ return (st.st_mode & FIO_S_IFDIR) != 0; }

// --- mkdir (no error if already exists) ---
static bool ensureDir(const std::string& dirNoSlash) {
    if (dirExists(dirNoSlash)) return true;
    int rc = sceIoMkdir(dirNoSlash.c_str(), 0777);
    return (rc >= 0) || dirExists(dirNoSlash);
}
static bool ensureDirRecursive(const std::string& fullDir) {
    if (dirExists(fullDir)) return true;
    std::string cur; cur.reserve(fullDir.size());
    for (size_t i = 0; i < fullDir.size(); ++i) {
        char c = fullDir[i]; cur.push_back(c);
        if (c == '/' || i + 1 == fullDir.size()) {
            if (cur.size() >= 3 && cur[cur.size()-1] == '/') {
                // skip roots like "ms0:/"
                if (cur.size() <= 4) continue;
            }
            if (!dirExists(cur)) {
                if (!ensureDir(cur)) return false;
            }
        }
    }
    return true;
}

// --- dir empty? ---
static bool __attribute__((unused)) isDirEmpty(const std::string& dir) {
    SceUID d = kfeIoOpenDir(dir.c_str());
    if (d < 0) return true;
    bool empty = true;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        if (strcmp(ent.d_name, ".") && strcmp(ent.d_name, "..")) { empty = false; break; }
        memset(&ent, 0, sizeof(ent));
    }
    kfeIoCloseDir(d);
    return empty;
}

// --- remove recursively (you already have a version; keep one) ---
static bool removeDirRecursive(const std::string& dir) {
    SceUID d = kfeIoOpenDir(dir.c_str());
    if (d < 0) return sceIoRmdir(dir.c_str()) >= 0;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
        std::string child = joinDirFile(dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) removeDirRecursive(child);
        else sceIoRemove(child.c_str());
        memset(&ent, 0, sizeof(ent));
        sceKernelDelayThread(0);
    }
    kfeIoCloseDir(d);
    return sceIoRmdir(dir.c_str()) >= 0;
}

// --- fast per-file rename based folder move (same device, no data copy) ---
static bool fastMoveDirByRenames(const std::string& srcDir, const std::string& dstDir) {
    logf("fastMoveDirByRenames: %s -> %s", srcDir.c_str(), dstDir.c_str());
    if (!ensureDirRecursive(dstDir)) { logf("  ensureDirRecursive(dst) FAILED"); return false; }

    SceUID d = kfeIoOpenDir(srcDir.c_str());
    if (d < 0) { logf("  open src failed %d", d); return false; }

    bool ok = true;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (ok && kfeIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
        std::string s = joinDirFile(srcDir, ent.d_name);
        std::string t = joinDirFile(dstDir, ent.d_name);

        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            // move subtree first
            ok = fastMoveDirByRenames(s, t);
            if (ok) sceIoRmdir(s.c_str());
        } else {
            // if target file exists, remove it (replace semantics)
            if (pathExists(t)) sceIoRemove(t.c_str());
            int rr = sceIoRename(s.c_str(), t.c_str());
            if (rr < 0) {
                // rename refused (some drivers); fall back to real copy for this file
                logf("  rename file -> %d; falling back to copy", rr);
                SceUID in = sceIoOpen(s.c_str(), PSP_O_RDONLY, 0);
                SceUID out = sceIoOpen(t.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
                if (in < 0 || out < 0) { if (in >= 0) sceIoClose(in); if (out >= 0) sceIoClose(out); ok = false; }
                else {
                    const int BUF = 128 * 1024; std::vector<uint8_t> buf(BUF);
                    ok = true;
                    while (ok) {
                        int r = sceIoRead(in, buf.data(), BUF);
                        if (r < 0) { ok = false; break; }
                        if (r == 0) break;
                        int off = 0;
                        while (off < r) {
                            int w = sceIoWrite(out, buf.data() + off, r - off);
                            if (w <= 0) { ok = false; break; }
                            off += w;
                        }
                        sceKernelDelayThread(0);
                    }
                    sceIoClose(in); sceIoClose(out);
                    if (ok) sceIoRemove(s.c_str());
                }
            }
        }
        memset(&ent, 0, sizeof(ent));
        sceKernelDelayThread(0);
    }
    kfeIoCloseDir(d);
    logf("fastMoveDirByRenames: %s", ok ? "OK" : "FAIL");
    return ok;
}

// --- size calculators (for preflight) ---
static bool sumDirBytes(const std::string& dir, uint64_t& out) {
    logf("sumDirBytes: enter %s (start=%llu)", dir.c_str(), (unsigned long long)out);
    SceUID d = kfeIoOpenDir(dir.c_str()); if (d < 0) return false;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name,".") || !strcmp(ent.d_name,"..")) { memset(&ent,0,sizeof(ent)); continue; }
        std::string p = joinDirFile(dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) { if (!sumDirBytes(p, out)) { kfeIoCloseDir(d); return false; } }
        else out += (uint64_t)ent.d_stat.st_size;
        memset(&ent, 0, sizeof(ent));
    }
    kfeIoCloseDir(d);
    logf("sumDirBytes: leave %s (now=%llu)", dir.c_str(), (unsigned long long)out);
    return true;
}

// add near the other helpers
static void __attribute__((unused)) hexdump(const void* p, size_t n) {
    if (!p || n == 0) return;
    const uint8_t* b = (const uint8_t*)p;
    char line[96];
    for (size_t i = 0; i < n; i += 16) {
        int off = snprintf(line, sizeof(line), "%04X:", (unsigned)i);
        for (size_t j = 0; j < 16 && i + j < n; ++j) {
            off += snprintf(line + off, sizeof(line) - off, " %02X", b[i + j]);
        }
        logWrite(line);
        logWrite("\r\n");
    }
}



static const char* canonicalDev(const char* devPath) {
    static char dev[5] = {0};
    if (!devPath) return nullptr;
    // Expect "ms0:/", "ms0:", "ef0:/", or "ef0:"
    if (strlen(devPath) >= 4 && devPath[3] == ':') {
        memcpy(dev, devPath, 4);
        dev[4] = '\0';
        return dev;           // e.g., "ms0:" or "ef0:"
    }
    return nullptr;
}

// ===== CMFileManager-style free space probe (only) =====
typedef struct {
    unsigned long maxClusters;
    unsigned long freeClusters;
    int           unk1;
    unsigned int  sectorSize;
    u64           sectorCount;
} CMF_SystemDevCtl;

typedef struct {
    CMF_SystemDevCtl* devCtl;
} CMF_SystemDevCommand;

// Returns true on success; out = free bytes. Uses dev "ms0:" / "ef0:" (with or without slash OK).
static bool getFreeBytesCMF(const char* devMaybeSlash, uint64_t& outFree) {
    outFree = 0;
    if (!devMaybeSlash || std::strlen(devMaybeSlash) < 4) return false;

    char dev4[5];
    std::memcpy(dev4, devMaybeSlash, 4); // "ms0:" / "ef0:"
    dev4[4] = '\0';

    CMF_SystemDevCtl devctl{};
    CMF_SystemDevCommand cmd{ &devctl };

    int rc = pspIoDevctl(dev4, 0x02425818, &cmd, sizeof(cmd), nullptr, 0);
    if (rc < 0 || devctl.sectorSize == 0) return false;

    outFree = (u64)devctl.freeClusters * devctl.sectorCount * devctl.sectorSize;
    return true;
}

// Returns total capacity bytes for a device (ms0:/ or ef0:/).
static bool getTotalBytesCMF(const char* devMaybeSlash, uint64_t& outTotal) {
    outTotal = 0;
    if (!devMaybeSlash || std::strlen(devMaybeSlash) < 4) return false;

    char dev4[5];
    std::memcpy(dev4, devMaybeSlash, 4); // "ms0:" / "ef0:"
    dev4[4] = '\0';

    CMF_SystemDevCtl devctl{};
    CMF_SystemDevCommand cmd{ &devctl };

    int rc = pspIoDevctl(dev4, 0x02425818, &cmd, sizeof(cmd), nullptr, 0);
    if (rc < 0 || devctl.sectorSize == 0) return false;

    outTotal = (u64)devctl.maxClusters * devctl.sectorCount * devctl.sectorSize;
    return true;
}

// ===== Free-space background probe cache (non-blocking) =====
struct FreeSpaceCache {
    // cached bytes + success flags
    volatile uint64_t ms0Free = 0;
    volatile uint64_t ef0Free = 0;
    volatile int ms0Ok = 0;
    volatile int ef0Ok = 0;
    volatile int paused    = 0;  // 1 = pause worker
    volatile int pausedAck = 0;  // worker says "I am paused"

    // age bookkeeping (microseconds since boot)
    volatile unsigned long long lastUS = 0;

    // thread plumbing
    SceUID threadId = -1;
    SceUID semId    = -1;
    volatile int pending = 0;
    volatile int running = 0;

    // which devices are present (updated by detectRoots)
    volatile int hasMs0 = 0;
    volatile int hasEf0 = 0;
};

static FreeSpaceCache gFSC;

static inline unsigned long long nowUS() {
    return (unsigned long long)sceKernelGetSystemTimeWide();
}

// Non-blocking getter (returns whatever we have, ok==0 means "unknown")
static bool FreeSpaceGet(const char* dev4, uint64_t& outBytes, bool& ok, unsigned long long* outAgeUS = nullptr) {
    if (!dev4) { ok = false; outBytes = 0; return false; }
    if (!strncasecmp(dev4, "ms0:", 4)) {
        outBytes = gFSC.ms0Free; ok = (gFSC.ms0Ok != 0);
    } else if (!strncasecmp(dev4, "ef0:", 4)) {
        outBytes = gFSC.ef0Free; ok = (gFSC.ef0Ok != 0);
    } else {
        ok = false; outBytes = 0; return false;
    }
    if (outAgeUS) *outAgeUS = (gFSC.lastUS ? (nowUS() - gFSC.lastUS) : ~0ull);
    return true;
}

// Signal the worker to refresh ASAP (returns immediately)
static void FreeSpaceRequestRefresh() {
    gFSC.pending = 1;
    if (gFSC.semId >= 0) sceKernelSignalSema(gFSC.semId, 1);
}

// Update which devices exist (call from detectRoots)
static void FreeSpaceSetPresence(bool hasMs, bool hasEf) {
    gFSC.hasMs0 = hasMs ? 1 : 0;
    gFSC.hasEf0 = hasEf ? 1 : 0;
    // also nudge a refresh when availability changes
    FreeSpaceRequestRefresh();
}

static int FreeSpaceThread(SceSize, void*) {
    gFSC.running = 1;
    while (gFSC.running) {
        // Fast path: if paused, just yield until unpaused
        if (gFSC.paused) {
            gFSC.pausedAck = 1;
            sceKernelDelayThread(10000); // 10ms nap
            continue;
        } else {
            gFSC.pausedAck = 0;
        }

        // Wait until asked, but also wake up occasionally (fallback: 3 seconds)
        for (int i = 0; i < 300; ++i) {
            if (gFSC.pending || gFSC.paused) break;
            if (gFSC.semId >= 0) {
                // (no-op try-wait; we just nap briefly)
            }
            sceKernelDelayThread(10000); // 10ms
        }
        if (gFSC.paused) { gFSC.pausedAck = 1; continue; }

        gFSC.pending = 0;

        uint64_t ms0B = 0, ef0B = 0;
        int msOk = 0, efOk = 0;

        // Probe ms0, but bail out immediately if paused
        if (!gFSC.paused && gFSC.hasMs0) {
            uint64_t v = 0;
            if (getFreeBytesCMF("ms0:", v)) { ms0B = v; msOk = 1; }
        }
        if (gFSC.paused) { gFSC.pausedAck = 1; continue; }

        // Probe ef0, ditto
        if (!gFSC.paused && gFSC.hasEf0) {
            uint64_t v = 0;
            if (getFreeBytesCMF("ef0:", v)) { ef0B = v; efOk = 1; }
        }
        if (gFSC.paused) { gFSC.pausedAck = 1; continue; }

        gFSC.ms0Free = ms0B; gFSC.ms0Ok = msOk;
        gFSC.ef0Free = ef0B; gFSC.ef0Ok = efOk;
        gFSC.lastUS  = nowUS();
    }
    return 0;
}


static void FreeSpaceInit() {
    if (gFSC.threadId >= 0) return;
    gFSC.semId = sceKernelCreateSema("FSC_Sema", 0, 0, 1, nullptr);
    gFSC.threadId = sceKernelCreateThread("FSC_Worker", FreeSpaceThread, 0x18 /* fairly low prio */, 0x1000, 0, nullptr);
    if (gFSC.threadId >= 0) sceKernelStartThread(gFSC.threadId, 0, nullptr);
    // request an immediate first probe
    FreeSpaceRequestRefresh();
}


// Start (or reconfigure) the worker to probe exactly one device.
// Only used on PSP Go running from ms0:, after the user picks a destination device.
static void FreeSpaceProbeOppositeOf(const char* pickedDev4) {
    if (!pickedDev4) return;
    const bool pickedIsMs = (strncasecmp(pickedDev4, "ms0:", 4) == 0);
    const char* opp = pickedIsMs ? "ef0:" : "ms0:";

    // Lazily init the worker the first time we need it.
    FreeSpaceInit();

    // Configure presence so the worker only probes the opposite side.
    const bool hasMs = (strncasecmp(opp, "ms0:", 4) == 0);
    const bool hasEf = (strncasecmp(opp, "ef0:", 4) == 0);
    FreeSpaceSetPresence(hasMs, hasEf);   // sets hasMs0/hasEf0 bits accordingly

    // Kick it.
    FreeSpaceRequestRefresh();
}

static void FreeSpacePauseNow() {
    gFSC.paused = 1;
    // Best-effort wait (~10ms) for the worker to acknowledge
    for (int i = 0; i < 10; ++i) {
        if (gFSC.pausedAck) break;
        sceKernelDelayThread(1000); // 1ms
    }
}

static void FreeSpaceResume() {
    gFSC.paused = 0;
    gFSC.pausedAck = 0;
    // Optional: nudge the worker so it resumes quickly
    if (gFSC.semId >= 0) sceKernelSignalSema(gFSC.semId, 1);
}



#ifndef PSP_UTILITY_OSK_RESULT_OK
#define PSP_UTILITY_OSK_RESULT_OK PSP_UTILITY_OSK_RESULT_CHANGED
#endif

// ---- forward decls from iso_titles_extras.cpp (titles/icons) ----
bool readJsoTitle(const std::string& path, std::string& outTitle);
bool readDaxTitle(const std::string& path, std::string& outTitle);
bool readJsoIconPNG(const std::string& path, std::vector<uint8_t>& outPng);
bool readDaxIconPNG(const std::string& path, std::vector<uint8_t>& outPng);

// ---------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------
static const char* kCatSettingsLabel = "Game Categories Settings";

static const char* rootDisplayName(const char* r) {
    if (!r) return "";
    if (!strcmp(r, "__USB_MODE__"))     return "USB Mode";
    if (!strcmp(r, "__GCL_TOGGLE__"))   return "Game Categories:";
    if (!strcmp(r, "__GCL_SETTINGS__")) return kCatSettingsLabel;
    if (!strcmp(r, "ms0:/"))            return "ms0:/ (Memory Stick)";
    if (!strcmp(r, "ef0:/"))            return "ef0:/ (Internal)";
    return r;
}


static bool startsWithCAT(const char* name) {
    return name && name[0]=='C' && name[1]=='A' && name[2]=='T' && name[3]=='_';
}
static char toLowerC(char c){ return (c>='A'&&c<='Z')? (c-'A'+'a') : c; }
static bool endsWithNoCase(const std::string& s, const char* ext){
    size_t n = s.size(), m = strlen(ext);
    if (m>n) return false;
    for (size_t i=0;i<m;i++){ if (toLowerC(s[n-m+i]) != toLowerC(ext[i])) return false; }
    return true;
}
static bool isIsoLike(const std::string& n){
    return endsWithNoCase(n, ".iso") || endsWithNoCase(n, ".cso") ||
           endsWithNoCase(n, ".zso") || endsWithNoCase(n, ".dax") ||
           endsWithNoCase(n, ".jso");
}
static void sanitizeTitleInPlace(std::string& s) {
    if (s.empty()) return;
    std::string out; out.reserve(s.size() + 4);
    bool justWrotePipe = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r' || c == '\n') {
            if (!justWrotePipe) { out.append(" | "); justWrotePipe = true; }
            while (i + 1 < s.size() && (s[i + 1] == '\r' || s[i + 1] == '\n')) ++i;
        } else {
            out.push_back(c);
            justWrotePipe = false;
        }
    }
    s.swap(out);
}

// FAT-safe filename sanitizer
static std::string sanitizeFilename(const std::string& in) {
    std::string out; out.reserve(in.size());
    for (unsigned char c : in) {
        if (c < 0x20) continue;
        switch (c) {
            case '\"': case '<': case '>': case ':':
            case '/': case '\\': case '|': case '?': case '*':
                out.push_back('_'); break;
            default: out.push_back((char)c); break;
        }
    }
    while (!out.empty() && (out.back()==' ' || out.back()=='.')) out.pop_back();
    size_t i=0; while (i<out.size() && out[i]==' ') i++;
    if (i>0) out.erase(0,i);
    if (out.empty()) out = "_";
    return out;
}

// Some firmwares pad dirent names with trailing spaces; trim so path lookups work.
static inline void trimTrailingSpaces(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') { s[--n] = '\0'; }
}

static bool isJunkHidden(const char* n) {
    if (!n || !*n) return false;
    if (n[0] == '.') {
        if (n[1] == '_') return true;
        if (!strcasecmp(n, ".ds_store")) return true;
        if (!strcasecmp(n, ".trashes")) return true;
        if (!strcasecmp(n, ".spotlight-v100")) return true;
        if (!strcasecmp(n, ".fseventsd")) return true;
    }
    if (!strncasecmp(n, "__macosx", 8)) return true;
    if (!strcasecmp(n, "thumbs.db")) return true;
    if (!strcasecmp(n, "desktop.ini")) return true;
    return false;
}

// case-insensitive EBOOT finder (for presence checks only)
static std::string findEbootCaseInsensitive(const std::string& dirMaybeSlash){
    std::string dpath = dirMaybeSlash;
    if (!dpath.empty() && dpath[dpath.size()-1]=='/') dpath.erase(dpath.size()-1);
    SceUID d = kfeIoOpenDir(dpath.c_str());
    if (d < 0) return {};
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        trimTrailingSpaces(ent.d_name);
        if (!FIO_S_ISDIR(ent.d_stat.st_mode) && strcasecmp(ent.d_name, "EBOOT.PBP") == 0) {
            std::string full = joinDirFile(dpath, ent.d_name); kfeIoCloseDir(d); return full;
        }
        memset(&ent, 0, sizeof(ent));
    }
    kfeIoCloseDir(d);
    return {};
}

// Legacy-style date string
static std::string buildLegacySortKey(const ScePspDateTime& dt){
    unsigned y  = (dt.year  < 0) ? 0u : (dt.year  > 9999 ? 9999u : (unsigned)dt.year);
    unsigned mo = (dt.month < 0) ? 0u : ((unsigned)dt.month % 100u);
    unsigned d  = (dt.day   < 0) ? 0u : ((unsigned)dt.day   % 100u);
    unsigned h  = (dt.hour  < 0) ? 0u : ((unsigned)dt.hour  % 100u);
    unsigned mi = (dt.minute< 0) ? 0u : ((unsigned)dt.minute% 100u);
    unsigned s  = (dt.second< 0) ? 0u : ((unsigned)dt.second% 100u);
    unsigned us = (dt.microsecond < 0) ? 0u :
                  (dt.microsecond > 999999 ? 999999u : (unsigned)dt.microsecond);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u%02u%02u%02u%02u%02u%06u", y,mo,d,h,mi,s,us);
    return std::string(buf);
}
static void fmtDT(const ScePspDateTime& dt, char* out, size_t n){
    unsigned y  = (dt.year  < 0) ? 0u : (dt.year  > 9999 ? 9999u : (unsigned)dt.year);
    unsigned mo = (dt.month < 0) ? 0u : ((unsigned)dt.month % 100u);
    unsigned d  = (dt.day   < 0) ? 0u : ((unsigned)dt.day   % 100u);
    unsigned h  = (dt.hour  < 0) ? 0u : ((unsigned)dt.hour  % 100u);
    unsigned mi = (dt.minute< 0) ? 0u : ((unsigned)dt.minute% 100u);
    unsigned s  = (dt.second< 0) ? 0u : ((unsigned)dt.second% 100u);
    snprintf(out, n, "%04u/%02u/%02u %02u:%02u:%02u", y,mo,d,h,mi,s);
}

// Match Game Categories Lite ordering when sorting is OFF (mtime desc).
static u64 categoryFolderMtime(const std::string& dev, const std::string& catName) {
    if (dev.empty() || catName.empty()) return 0;
    const char* roots[] = {"PSP/GAME/","PSP/GAME150/","PSP/GAME/PSX/","PSP/GAME/Utility/","ISO/"};
    SceIoStat st{}; 
    for (auto r : roots) {
        std::string p = dev + std::string(r) + catName;
        if (sceIoGetstat(p.c_str(), &st) >= 0 && FIO_S_ISDIR(st.st_mode)) {
            u64 t = 0;
            sceRtcGetTick((ScePspDateTime *)&st.sce_st_mtime, &t);
            return t;
        }
    }
    return 0;
}

// dir iterator
template<typename F>
static void forEachEntry(const std::string& dir, F f){
    std::string dpath = dir;
    if (!dpath.empty() && dpath[dpath.size()-1]=='/') dpath.erase(dpath.size()-1);
    SceUID d = kfeIoOpenDir(dpath.c_str());
    if (d < 0) return;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        trimTrailingSpaces(ent.d_name);
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
        if (isJunkHidden(ent.d_name))                                 { memset(&ent,0,sizeof(ent)); continue; }
        f(ent);
        memset(&ent, 0, sizeof(ent));
    }
    kfeIoCloseDir(d);
}

struct AnimFileInfo {
    std::string path;
    int index = 0;
    uint32_t delayMs = 0;
};

static bool parseAnimFrameName(const char* name, int& outIndex, uint32_t& outDelayMs) {
    if (!name) return false;
    if (!endsWithNoCase(name, ".png")) return false;

    const char* prefix = "frame_";
    size_t prefixLen = strlen(prefix);
    if (strncmp(name, prefix, prefixLen) != 0) return false;

    const char* p = name + prefixLen;
    if (*p < '0' || *p > '9') return false;
    char* endIdx = nullptr;
    long idx = std::strtol(p, &endIdx, 10);
    if (endIdx == p || idx < 0) return false;

    const char* delayTag = strstr(endIdx, "delay-");
    if (!delayTag) delayTag = strstr(name, "delay-");
    if (!delayTag) return false;
    delayTag += strlen("delay-");
    char* endDelay = nullptr;
    double secs = std::strtod(delayTag, &endDelay);
    if (endDelay == delayTag || secs <= 0.0) return false;

    uint32_t ms = (uint32_t)(secs * 1000.0 + 0.5);
    if (ms < 1) ms = 1;
    outIndex = (int)idx;
    outDelayMs = ms;
    return true;
}

static void bleedTextureAlpha(Texture* t) {
    if (!t || !t->data || t->width <= 0 || t->height <= 0) return;
    uint32_t* px = (uint32_t*)t->data;
    const int w = t->width;
    const int h = t->height;
    const int s = t->stride;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t c = px[y * s + x];
            if ((c >> 24) != 0) continue;

            uint32_t neighbor = 0;
            if (x > 0) {
                neighbor = px[y * s + (x - 1)];
                if ((neighbor >> 24) != 0) { px[y * s + x] = (c & 0xFF000000) | (neighbor & 0x00FFFFFF); continue; }
            }
            if (x + 1 < w) {
                neighbor = px[y * s + (x + 1)];
                if ((neighbor >> 24) != 0) { px[y * s + x] = (c & 0xFF000000) | (neighbor & 0x00FFFFFF); continue; }
            }
            if (y > 0) {
                neighbor = px[(y - 1) * s + x];
                if ((neighbor >> 24) != 0) { px[y * s + x] = (c & 0xFF000000) | (neighbor & 0x00FFFFFF); continue; }
            }
            if (y + 1 < h) {
                neighbor = px[(y + 1) * s + x];
                if ((neighbor >> 24) != 0) { px[y * s + x] = (c & 0xFF000000) | (neighbor & 0x00FFFFFF); continue; }
            }
        }
    }
}

static bool loadAnimationFrames(const std::string& dir,
                                std::vector<MBAnimFrame>& outFrames,
                                unsigned long long& outMinDelayUs) {
    outMinDelayUs = 0;
    std::vector<AnimFileInfo> files;
    forEachEntry(dir, [&](const SceIoDirent& e){
        if (FIO_S_ISDIR(e.d_stat.st_mode)) return;
        int idx = 0;
        uint32_t delayMs = 0;
        if (!parseAnimFrameName(e.d_name, idx, delayMs)) return;
        AnimFileInfo info;
        info.path = joinDirFile(dir, e.d_name);
        info.index = idx;
        info.delayMs = delayMs;
        files.push_back(info);
    });

    if (files.empty()) return false;
    std::sort(files.begin(), files.end(),
              [](const AnimFileInfo& a, const AnimFileInfo& b){ return a.index < b.index; });

    outFrames.clear();
    outFrames.reserve(files.size());
    uint32_t minDelayMs = 0;
    for (const auto& f : files) {
        Texture* tex = texLoadPNG(f.path.c_str());
        if (!tex || !tex->data) { if (tex) texFree(tex); continue; }
        bleedTextureAlpha(tex);
        outFrames.push_back({tex, f.delayMs});
        if (minDelayMs == 0 || f.delayMs < minDelayMs) minDelayMs = f.delayMs;
    }

    if (outFrames.empty()) return false;
    if (minDelayMs == 0) minDelayMs = 100;
    outMinDelayUs = (unsigned long long)minDelayMs * 1000ULL;
    return true;
}

static void freeAnimationFrames(std::vector<MBAnimFrame>& frames) {
    for (auto& f : frames) {
        if (f.tex) { texFree(f.tex); f.tex = nullptr; }
    }
    frames.clear();
}

static uint32_t popAnimRand() {
    static uint32_t seed = 0;
    if (seed == 0) {
        seed = (uint32_t)sceKernelGetSystemTimeWide();
        if (seed == 0) seed = 1;
    }
    seed = seed * 1664525u + 1013904223u;
    return seed;
}

static void shufflePopAnimOrder() {
    gPopAnimOrder.clear();
    gPopAnimOrder.reserve(gPopAnimDirs.size());
    for (size_t i = 0; i < gPopAnimDirs.size(); ++i) gPopAnimOrder.push_back(i);
    for (size_t i = gPopAnimOrder.size(); i > 1; --i) {
        size_t j = popAnimRand() % i;
        std::swap(gPopAnimOrder[i - 1], gPopAnimOrder[j]);
    }
    gPopAnimOrderIndex = 0;
}

static const std::string* nextPopAnimDir() {
    if (gPopAnimDirs.empty()) return nullptr;
    if (gPopAnimOrder.empty() || gPopAnimOrderIndex >= gPopAnimOrder.size()) {
        shufflePopAnimOrder();
    }
    if (gPopAnimOrder.empty()) return nullptr;
    size_t idx = gPopAnimOrder[gPopAnimOrderIndex++];
    return &gPopAnimDirs[idx];
}

static bool ensurePopAnimLoaded(const std::string& dir) {
    if (!gPopAnimLoadedDir.empty() && gPopAnimLoadedDir == dir && !gPopAnimFrames.empty()) {
        return true;
    }
    freeAnimationFrames(gPopAnimFrames);
    gPopAnimMinDelayUs = 0;
    if (!loadAnimationFrames(dir, gPopAnimFrames, gPopAnimMinDelayUs)) {
        gPopAnimLoadedDir.clear();
        return false;
    }
    gPopAnimLoadedDir = dir;
    return true;
}

static unsigned long long frameDelayUs(const MBAnimFrame& f, unsigned long long fallbackUs) {
    if (f.delayMs > 0) return (unsigned long long)f.delayMs * 1000ULL;
    if (fallbackUs > 0) return fallbackUs;
    return 100000ULL;
}

static void collectHomeAnimations(const std::string& animRoot) {
    gHomeAnimEntries.clear();
    if (!dirExists(animRoot)) return;

    forEachEntry(animRoot, [&](const SceIoDirent& e){
        if (!FIO_S_ISDIR(e.d_stat.st_mode)) return;
        std::string dir = joinDirFile(animRoot, e.d_name);
        bool hasFrame = false;
        std::string credit;
        forEachEntry(dir, [&](const SceIoDirent& f){
            if (FIO_S_ISDIR(f.d_stat.st_mode)) return;
            int idx = 0; uint32_t delayMs = 0;
            if (parseAnimFrameName(f.d_name, idx, delayMs)) {
                hasFrame = true;
            } else if (credit.empty() && endsWithNoCase(f.d_name, ".txt")) {
                credit = f.d_name;
            }
        });
        if (hasFrame) gHomeAnimEntries.push_back({dir, credit});
    });

    std::sort(gHomeAnimEntries.begin(), gHomeAnimEntries.end(),
              [](const HomeAnimEntry& a, const HomeAnimEntry& b){
                  return strcasecmp(basenameOf(a.dir).c_str(), basenameOf(b.dir).c_str()) < 0;
              });
}

static bool loadHomeAnimationAt(int index) {
    if (index < 0 || index >= (int)gHomeAnimEntries.size()) return false;

    std::vector<MBAnimFrame> frames;
    unsigned long long minDelay = 0;
    if (!loadAnimationFrames(gHomeAnimEntries[index].dir, frames, minDelay) || frames.empty()) {
        freeAnimationFrames(frames);
        return false;
    }

    freeAnimationFrames(gHomeAnimFrames);
    gHomeAnimFrames.swap(frames);
    gHomeAnimMinDelayUs = minDelay;
    gHomeAnimFrameIndex = 0;
    gHomeAnimIndex = index;

    unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
    gHomeAnimNextUs = now + frameDelayUs(gHomeAnimFrames[0], gHomeAnimMinDelayUs);
    return true;
}

static void pickHomeAnimation(int preferredIndex) {
    if (gHomeAnimEntries.empty()) return;
    int count = (int)gHomeAnimEntries.size();
    int start = preferredIndex % count;
    if (start < 0) start += count;

    for (int i = 0; i < count; ++i) {
        int idx = (start + i) % count;
        if (loadHomeAnimationAt(idx)) return;
    }

    gHomeAnimIndex = -1;
    gHomeAnimFrameIndex = 0;
    gHomeAnimNextUs = 0;
    gHomeAnimMinDelayUs = 0;
    freeAnimationFrames(gHomeAnimFrames);
}

static void initHomeAnimations(const std::string& animRoot) {
    collectHomeAnimations(animRoot);
    if (gHomeAnimEntries.empty()) return;
    int start = (int)(popAnimRand() % gHomeAnimEntries.size());
    pickHomeAnimation(start);
}

static void cycleHomeAnimation(int dir) {
    if (gHomeAnimEntries.empty()) return;
    if (gHomeAnimIndex < 0) {
        pickHomeAnimation(0);
        return;
    }
    int count = (int)gHomeAnimEntries.size();
    int next = (gHomeAnimIndex + dir) % count;
    if (next < 0) next += count;
    pickHomeAnimation(next);
}

static void advanceHomeAnimationFrame() {
    if (gHomeAnimFrames.empty()) return;
    if (gHomeAnimFrameIndex >= gHomeAnimFrames.size()) gHomeAnimFrameIndex = 0;
    unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
    if (gHomeAnimNextUs == 0 || now >= gHomeAnimNextUs) {
        gHomeAnimFrameIndex = (gHomeAnimFrameIndex + 1) % gHomeAnimFrames.size();
        gHomeAnimNextUs = now + frameDelayUs(gHomeAnimFrames[gHomeAnimFrameIndex], gHomeAnimMinDelayUs);
    }
}

static std::string currentHomeAnimCredit() {
    if (gHomeAnimIndex >= 0 && gHomeAnimIndex < (int)gHomeAnimEntries.size()) {
        std::string credit = gHomeAnimEntries[gHomeAnimIndex].creditFile;
        if (!credit.empty()) {
            if (endsWithNoCase(credit, ".txt")) {
                credit.erase(credit.size() - 4);
            }
            return credit;
        }
        return basenameOf(gHomeAnimEntries[gHomeAnimIndex].dir);
    }
    return std::string("Unknown");
}

// Dominant icon color calc (unchanged)
static uint32_t computeDominantColorABGRFromTexture(const Texture* t) {
    if (!t || !t->data || t->width <= 0 || t->height <= 0) return 0xFF000000;
    const int BITS = 4, SHIFT = 8 - BITS, BUCKETS = 1 << (BITS * 3);
    const uint32_t MIN_ALPHA = 8, MIN_LUMA  = 20, MIN_SAT = 28;
    static uint32_t counts[BUCKETS];
    static uint64_t rsum[BUCKETS], gsum[BUCKETS], bsum[BUCKETS];
    memset(counts, 0, sizeof(counts));
    memset(rsum,   0, sizeof(rsum));
    memset(gsum,   0, sizeof(gsum));
    memset(bsum,   0, sizeof(bsum));

    const int w = t->width, h = t->height, s = t->stride;
    const uint32_t* base = (const uint32_t*)t->data;
    uint64_t allAlpha = 0, brightAlpha = 0;

    for (int y = 0; y < h; ++y) {
        const uint32_t* row = base + y * s;
        for (int x = 0; x < w; ++x) {
            uint32_t c = row[x];
            uint32_t a = (c >> 24) & 0xFF;
            if (a < MIN_ALPHA) continue;
            allAlpha += a;
            uint32_t r =  c        & 0xFF;
            uint32_t g = (c >>  8) & 0xFF;
            uint32_t b = (c >> 16) & 0xFF;
            uint32_t y8 = (54*r + 183*g + 18*b + 128) >> 8;
            if (y8 < MIN_LUMA) continue;
            brightAlpha += a;
            uint32_t mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
            uint32_t mn = r; if (g < mn) mn = g; if (b < mn) mn = b;
            uint32_t sat = (mx == 0) ? 0 : (255u * (mx - mn)) / mx;
            if (sat < MIN_SAT) continue;
            uint32_t wgt = (a * (32u + sat)) >> 5;

            int idx = ( (r >> SHIFT)        ) |
                      ( (g >> SHIFT) <<  BITS) |
                      ( (b >> SHIFT) << (BITS*2));
            counts[idx] += wgt;
            rsum[idx]   += (uint64_t)r * wgt;
            gsum[idx]   += (uint64_t)g * wgt;
            bsum[idx]   += (uint64_t)b * wgt;
        }
    }
    if (brightAlpha == 0 || (brightAlpha * 20 < allAlpha)) return 0xFF000000;
    int best = -1; uint32_t bestCount = 0;
    for (int i = 0; i < BUCKETS; ++i) if (counts[i] > bestCount) { bestCount = counts[i]; best = i; }
    if (best < 0 || bestCount == 0) return 0xFF000000;
    uint32_t r = (uint32_t)(rsum[best] / bestCount);
    uint32_t g = (uint32_t)(gsum[best] / bestCount);
    uint32_t b = (uint32_t)(bsum[best] / bestCount);
    return (0xFFu << 24) | (b << 16) | (g << 8) | r;
}


// ---------------------------------------------------------------
// PARAM.SFO / PBP / ISO helpers (titles)
// ---------------------------------------------------------------
#pragma pack(push,1)
struct SFOHeader {
    uint32_t magic;            // 'PSF\0' = 0x46535000 LE
    uint32_t version;          // 0x00000101
    uint32_t keyTableOffset;   // from start
    uint32_t dataTableOffset;  // from start
    uint32_t indexCount;
};
struct SFOIndex {
    uint16_t keyOffset;        // from key table start
    uint8_t  dataFmt;          // not used here
    uint8_t  pad;
    uint32_t dataLen;
    uint32_t dataMaxLen;
    uint32_t dataOffset;       // from data table start
};
#pragma pack(pop)

static bool readAll(SceUID fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        int r = sceIoRead(fd, p + got, (uint32_t)(n - got));
        if (r <= 0) return false;
        got += r;
    }
    return true;
}
static bool readAt(SceUID fd, uint32_t off, void* buf, size_t n) {
    if (sceIoLseek32(fd, (int)off, PSP_SEEK_SET) < 0) return false;
    return readAll(fd, buf, n);
}

bool sfoExtractTitle(const uint8_t* data, size_t size, std::string& outTitle) {
    if (!data || size < sizeof(SFOHeader)) return false;
    const SFOHeader* h = (const SFOHeader*)data;
    if (h->magic != 0x46535000) return false; // 'PSF\0'
    if (sizeof(SFOHeader) + h->indexCount * sizeof(SFOIndex) > size) return false;

    const SFOIndex* idx = (const SFOIndex*)(data + sizeof(SFOHeader));
    const char* keys = (const char*)(data + h->keyTableOffset);
    const uint8_t* vals = data + h->dataTableOffset;

    for (uint32_t i=0;i<h->indexCount;i++) {
        const char* key = keys + idx[i].keyOffset;
        if (!key) continue;
        if (strcmp(key, "TITLE") == 0) {
            const uint8_t* v = vals + idx[i].dataOffset;
            uint32_t len = idx[i].dataLen;
            if ((size_t)(v - data) + len <= size) {
                std::string s((const char*)v, (const char*)v + len);
                while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
                outTitle = s;
                sanitizeTitleInPlace(outTitle);
                return !outTitle.empty();
            }
        }
    }
    return false;
}

static std::string findFileCaseInsensitive(const std::string& dirNoSlash, const char* wantName) {
    std::string dpath = dirNoSlash;
    if (!dpath.empty() && dpath.back() == '/') dpath.pop_back();
    SceUID d = kfeIoOpenDir(dpath.c_str());
    if (d < 0) return {};
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    std::string out;
    while (kfeIoReadDir(d, &ent) > 0) {
        trimTrailingSpaces(ent.d_name);
        if (!FIO_S_ISDIR(ent.d_stat.st_mode) && strcasecmp(ent.d_name, wantName) == 0) {
            out = joinDirFile(dpath, ent.d_name);
            break;
        }
        memset(&ent, 0, sizeof(ent));
    }
    kfeIoCloseDir(d);
    return out;
}

// Read title from folder
static bool getFolderTitle(const std::string& folderNoSlash, std::string& outTitle) {
    std::string sfoPath = findFileCaseInsensitive(folderNoSlash, "PARAM.SFO");
    if (!sfoPath.empty()) {
        SceUID fd = sceIoOpen(sfoPath.c_str(), PSP_O_RDONLY, 0);
        if (fd >= 0) {
            SceIoStat st{}; if (sceIoGetstat(sfoPath.c_str(), &st) >= 0 && st.st_size > 0 && st.st_size < 1*1024*1024) {
                std::vector<uint8_t> buf((size_t)st.st_size);
                if (readAll(fd, buf.data(), buf.size())) {
                    sceIoClose(fd);
                    if (sfoExtractTitle(buf.data(), buf.size(), outTitle)) return true;
                } else sceIoClose(fd);
            } else sceIoClose(fd);
        }
    }
    std::string eboot = findEbootCaseInsensitive(folderNoSlash);
    if (!eboot.empty()) {
        SceUID fd = sceIoOpen(eboot.c_str(), PSP_O_RDONLY, 0);
        if (fd >= 0) {
            uint8_t hdr[4 + 4 + 8*4];
            if (readAll(fd, hdr, sizeof(hdr))) {
                bool isPBP = (memcmp(hdr, "\0PBP", 4) == 0) || (memcmp(hdr, "PBP\0", 4) == 0);
                if (isPBP) {
                    auto r32 = [](const uint8_t* p)->uint32_t {
                        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    };
                    uint32_t offs[8];
                    for (int i = 0; i < 8; ++i) offs[i] = r32(hdr + 8 + i*4);

                    SceIoStat st{}; sceIoGetstat(eboot.c_str(), &st);
                    uint32_t fileSize = (uint32_t)st.st_size;

                    uint32_t start = offs[0]; // PARAM.SFO
                    if (start < sizeof(hdr) || start >= fileSize) start = sizeof(hdr);

                    uint32_t end = fileSize;
                    for (int i = 1; i < 8; ++i) if (offs[i] && offs[i] > start && offs[i] < end) end = offs[i];

                    const uint32_t MAX_SCAN = 1024*1024;
                    if (end - start > MAX_SCAN) end = start + MAX_SCAN;
                    if (end > fileSize) end = fileSize;
                    if (end <= start) { sceIoClose(fd); return false; }

                    std::vector<uint8_t> buf(end - start);
                    if (!readAt(fd, start, buf.data(), buf.size())) { sceIoClose(fd); return false; }
                    sceIoClose(fd);

                    if (sfoExtractTitle(buf.data(), buf.size(), outTitle)) return true;

                    static const uint8_t PSF_MAGIC[4] = { 'P','S','F','\0' };
                    for (size_t i = 0; i + 4 <= buf.size(); i += 4) {
                        if (memcmp(&buf[i], PSF_MAGIC, 4) == 0) {
                            if (sfoExtractTitle(buf.data() + i, buf.size() - i, outTitle)) return true;
                        }
                    }
                }
                sceIoClose(fd);
            }
            sceIoClose(fd);
        }
    }
    return false;
}

// Minimal ISO9660 reader for ICON (unchanged)
struct IsoDirRec { uint32_t lba; uint32_t size; uint8_t flags; };
static bool isoReadDirRec(const uint8_t* p, size_t n, size_t off, IsoDirRec& out, std::string& name, bool& isDir) {
    if (off + 1 > n) return false;
    uint8_t len = p[off + 0];
    if (len == 0) return false;
    if (off + len > n) return false;
    const uint8_t* r = p + off;
    auto le32 = [](const uint8_t* q)->uint32_t{ return (uint32_t)q[0] | ((uint32_t)q[1]<<8) | ((uint32_t)q[2]<<16) | ((uint32_t)q[3]<<24); };
    uint32_t lba = le32(r + 2);
    uint32_t size = le32(r + 10);
    uint8_t flags = r[25];
    uint8_t nameLen = r[32];
    const char* nm = (const char*)(r + 33);
    name.assign(nm, nm + nameLen);
    if (nameLen == 1 && (nm[0] == 0 || nm[0] == 1)) name = "";
    size_t sc = name.find(';'); if (sc != std::string::npos) name.resize(sc);
    isDir = (flags & 0x02) != 0;
    out.lba = lba; out.size = size; out.flags = flags;
    return true;
}
static bool isoFindEntry(SceUID fd, const IsoDirRec& dir, const char* target, IsoDirRec& out) {
    uint32_t bytes = ((dir.size + ISO_SECTOR - 1)/ISO_SECTOR)*ISO_SECTOR;
    std::vector<uint8_t> buf(bytes);
    if (!readAt(fd, dir.lba * ISO_SECTOR, buf.data(), bytes)) return false;

    size_t pos = 0;
    while (pos < bytes) {
        if (buf[pos] == 0) {
            pos = ((pos / ISO_SECTOR) + 1) * ISO_SECTOR;
            continue;
        }
        IsoDirRec r{}; std::string nm; bool isDir=false;
        if (!isoReadDirRec(buf.data(), bytes, pos, r, nm, isDir)) break;
        if (!nm.empty() && strcasecmp(nm.c_str(), target) == 0) { out = r; return true; }
        pos += buf[pos];
    }
    return false;
}


// CSO / ZSO (v1 and v2)
static Texture* loadCompressedIsoIconPNG(const std::string& path) {
    std::vector<uint8_t> png;
    if (ExtractIcon0PNG(path, png) && !png.empty())
        return texLoadPNGFromMemory(png.data(), (int)png.size());
    return nullptr;
}


// === ICON0 helpers ===================================================
static Texture* loadIconFromPBP(const std::string& ebootPath) {
    SceUID fd = sceIoOpen(ebootPath.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return nullptr;
    uint8_t hdr[4 + 4 + 8*4];
    if (!readAll(fd, hdr, sizeof(hdr))) { sceIoClose(fd); return nullptr; }
    bool isPBP = (memcmp(hdr, "\0PBP", 4) == 0) || (memcmp(hdr, "PBP\0", 4) == 0);
    if (!isPBP) { sceIoClose(fd); return nullptr; }
    auto r32 = [](const uint8_t* p)->uint32_t {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    };
    uint32_t offs[8]; for (int i=0;i<8;i++) offs[i] = r32(hdr + 8 + i*4);
    SceIoStat st{}; if (sceIoGetstat(ebootPath.c_str(), &st) < 0) { sceIoClose(fd); return nullptr; }
    uint32_t fileSize = (uint32_t)st.st_size;
    uint32_t start = offs[1]; // ICON0.PNG
    if (start == 0 || start >= fileSize) { sceIoClose(fd); return nullptr; }
    uint32_t end = fileSize;
    for (int i=2;i<8;i++) if (offs[i] && offs[i] > start && offs[i] < end) end = offs[i];
    if (end <= start || end - start > 1024*1024) { sceIoClose(fd); return nullptr; }
    std::vector<uint8_t> buf(end - start);
    if (!readAt(fd, start, buf.data(), buf.size())) { sceIoClose(fd); return nullptr; }
    sceIoClose(fd);
    return texLoadPNGFromMemory(buf.data(), (int)buf.size());
}

// Uncompressed ISO
static Texture* loadIsoIconPNG(const std::string& isoPath) {
    SceUID fd = sceIoOpen(isoPath.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return nullptr;
    uint8_t pvd[ISO_SECTOR];
    if (!readAt(fd, 16 * ISO_SECTOR, pvd, sizeof(pvd))) { sceIoClose(fd); return nullptr; }
    if (!(pvd[0]==1 && memcmp(pvd+1,"CD001",5)==0 && pvd[6]==1)) { sceIoClose(fd); return nullptr; }
    IsoDirRec root{}; { std::string nm; bool isDir=false;
        if (!isoReadDirRec(pvd, sizeof(pvd), 156, root, nm, isDir)) { sceIoClose(fd); return nullptr; } }
    IsoDirRec pspGame{}; if (!isoFindEntry(fd, root, "PSP_GAME", pspGame)) { sceIoClose(fd); return nullptr; }
    IsoDirRec icon{};    if (!isoFindEntry(fd, pspGame, "ICON0.PNG", icon)) { sceIoClose(fd); return nullptr; }
    if (!icon.size || icon.size > 1024*1024) { sceIoClose(fd); return nullptr; }
    std::vector<uint8_t> png(icon.size);
    if (!readAt(fd, icon.lba * ISO_SECTOR, png.data(), (uint32_t)png.size())) { sceIoClose(fd); return nullptr; }
    sceIoClose(fd);
    return texLoadPNGFromMemory(png.data(), (int)png.size());
}

// Returns how many "games" are inside a category folder (across ISO and GAME roots)
// on the given device. Category names may or may not have a CAT_ prefix.
// Games = ISO-like files (.iso/.cso/.zso/.dax/.jso) in ISO roots,
//      or folders under PSP/GAME.../<category> that contain an EBOOT.PBP (case-insensitive).
// Count games in a named <category> across both ISO and EBOOT schemes (case-insensitive).
static int countGamesInCategory(const std::string& device, const std::string& cat) {
    const char* isoRoots[]  = {"ISO/"};                // drop ISO/PSP/ as a root
    const char* gameRoots[] = {"PSP/GAME/","PSP/GAME150/"}; // drop PSX/ and Utility/ as roots

    int count = 0;

    // ISO-like files
    for (auto r : isoRoots) {
        std::string base = device + std::string(r) + cat + "/";
        if (!dirExists(base)) continue;
        forEachEntry(base, [&](const SceIoDirent& e){
            if (!FIO_S_ISDIR(e.d_stat.st_mode)) {

                std::string n = e.d_name;
                if (isIsoLike(n)) count++;
            }
        });
    }

    // EBOOT folders (only immediate children of the category directory)
    for (auto r : gameRoots) {
        std::string base = device + std::string(r) + cat + "/";
        if (!dirExists(base)) continue;
        forEachEntry(base, [&](const SceIoDirent& e){
            if (FIO_S_ISDIR(e.d_stat.st_mode)) {
                std::string child = base + e.d_name;
                std::string eboot = findEbootCaseInsensitive(child);
                if (!eboot.empty()) count++;
            }
        });
    }
    return count;
}


// Create the CAT_ folder across standard ISO/GAME roots (silently skips parents that don't exist)
// Create the CAT_ folder tree across ISO/GAME roots (skips parents that don't exist)
static void createCategoryDirs(const std::string& device, const std::string& cat) {
    const char* isoRoots[]  = {"ISO/"};                // drop ISO/PSP/ as a root
    const char* gameRoots[] = {"PSP/GAME/","PSP/GAME150/"}; // drop PSX/ and Utility/ as roots
    for (auto r : isoRoots) {
        std::string p = device + std::string(r) + cat;
        if (dirExists(parentOf(p))) sceIoMkdir(p.c_str(), 0777);
    }
    for (auto r : gameRoots) {
        std::string p = device + std::string(r) + cat;
        if (dirExists(parentOf(p))) sceIoMkdir(p.c_str(), 0777);
    }
}

// Remove the CAT_ folder tree across ISO/GAME roots (recursive)
static void deleteCategoryDirs(const std::string& device, const std::string& cat) {
    const char* isoRoots[]  = {"ISO/"};                // drop ISO/PSP/ as a root
    const char* gameRoots[] = {"PSP/GAME/","PSP/GAME150/"}; // drop PSX/ and Utility/ as roots
    for (auto r : isoRoots)  { std::string p = device + std::string(r) + cat; if (dirExists(p)) removeDirRecursive(p); }
    for (auto r : gameRoots) { std::string p = device + std::string(r) + cat; if (dirExists(p)) removeDirRecursive(p); }
}




// ---------------------------------------------------------------
// EBOOT.PBP Category Reader (for PS1 vs Homebrew detection)
// ---------------------------------------------------------------
static std::string readEbootCategory(const std::string& ebootPath) {
    SceUID fd = sceIoOpen(ebootPath.c_str(), PSP_O_RDONLY, 0777);
    if (fd < 0) return "";

    // Read PBP header magic
    char magic[4];
    if (sceIoRead(fd, magic, 4) != 4 || memcmp(magic, "\x00PBP", 4) != 0) {
        sceIoClose(fd);
        return "";
    }

    // Read SFO offset at offset 8
    sceIoLseek(fd, 8, PSP_SEEK_SET);
    uint32_t sfoOffset;
    if (sceIoRead(fd, &sfoOffset, 4) != 4) {
        sceIoClose(fd);
        return "";
    }

    // Go to SFO and check magic
    sceIoLseek(fd, sfoOffset, PSP_SEEK_SET);
    char sfoMagic[4];
    if (sceIoRead(fd, sfoMagic, 4) != 4 || memcmp(sfoMagic, "\x00PSF", 4) != 0) {
        sceIoClose(fd);
        return "";
    }

    // Read SFO header
    sceIoLseek(fd, sfoOffset + 8, PSP_SEEK_SET);
    uint32_t keyTableOffset, dataTableOffset, numEntries;
    sceIoRead(fd, &keyTableOffset, 4);
    sceIoRead(fd, &dataTableOffset, 4);
    sceIoRead(fd, &numEntries, 4);

    // Read entries to find CATEGORY
    for (uint32_t i = 0; i < numEntries; i++) {
        sceIoLseek(fd, sfoOffset + 20 + i * 16, PSP_SEEK_SET);
        uint16_t keyOffset;
        uint16_t dataFmt;
        uint32_t dataLen;
        uint32_t dataMax;
        uint32_t dataOffset;

        sceIoRead(fd, &keyOffset, 2);
        sceIoRead(fd, &dataFmt, 2);
        sceIoRead(fd, &dataLen, 4);
        sceIoRead(fd, &dataMax, 4);
        sceIoRead(fd, &dataOffset, 4);

        // Read key name
        sceIoLseek(fd, sfoOffset + keyTableOffset + keyOffset, PSP_SEEK_SET);
        char keyName[32] = {0};
        for (int j = 0; j < 31; j++) {
            char c;
            if (sceIoRead(fd, &c, 1) != 1 || c == 0) break;
            keyName[j] = c;
        }

        if (strcmp(keyName, "CATEGORY") == 0) {
            // Read CATEGORY value
            sceIoLseek(fd, sfoOffset + dataTableOffset + dataOffset, PSP_SEEK_SET);
            char category[16] = {0};
            sceIoRead(fd, category, std::min(dataLen, (uint32_t)15));
            sceIoClose(fd);
            return std::string(category);
        }
    }

    sceIoClose(fd);
    return "";
}

// Per-frame cache to avoid reading same EBOOT multiple times in one frame
static std::map<std::string, std::string> categoryCache;

// ---------------------------------------------------------------
// Model types + label mode
// ---------------------------------------------------------------
struct GameItem {
    enum Kind { ISO_FILE, EBOOT_FOLDER } kind;
    std::string    label;      // filename/folder name (default)
    std::string    title;      // app title (if found)
    std::string    path;       // ISO file OR ***EBOOT PARENT FOLDER PATH*** (no trailing slash)
    ScePspDateTime time{};     // the time we sort by (folder for EBOOT, file for ISO)
    std::string    sortKey;    // legacy sort string (desc)
    uint64_t       sizeBytes = 0;  // <--- NEW: bytes for size column
};


// Verbose, unified "need" calculator for Move/Copy
static uint64_t bytesNeededForOp(const std::vector<std::string>& srcPaths,
                                 const std::vector<GameItem::Kind>& kinds,
                                 const std::string& dstDev,
                                 bool isCopy)
{
    uint64_t need = 0;
    for (size_t i = 0; i < srcPaths.size(); ++i) {
        const bool sameDev = sameDevice(srcPaths[i], dstDev);

        // Move: same-device needs 0 extra space; Copy: counts regardless of device
        if (!isCopy && sameDev) {
            logf("need: %s -> %s (same device MOVE) need+=0",
                 srcPaths[i].c_str(), dstDev.c_str());
            continue;
        }

        if (kinds[i] == GameItem::ISO_FILE) {
            SceIoStat st{};
            if (sceIoGetstat(srcPaths[i].c_str(), &st) >= 0) {
                need += (uint64_t)st.st_size;
                logf("need: ISO %s size=%llu total=%llu",
                     srcPaths[i].c_str(),
                     (unsigned long long)st.st_size,
                     (unsigned long long)need);
            } else {
                logf("need: ISO %s stat FAIL", srcPaths[i].c_str());
            }
        } else {
            uint64_t before = need;
            if (!sumDirBytes(srcPaths[i], need)) {
                logf("need: DIR %s sum FAIL", srcPaths[i].c_str());
            } else {
                logf("need: DIR %s added=%llu total=%llu",
                     srcPaths[i].c_str(),
                     (unsigned long long)(need - before),
                     (unsigned long long)need);
            }
        }
    }
    logf("need: FINAL need=%llu for dstDev=%s (mode=%s)",
         (unsigned long long)need, dstDev.c_str(), isCopy ? "COPY" : "MOVE");
    return need;
}

// Back-compat wrapper: preserves your original function name/signature for MOVE
static __attribute__((unused))
uint64_t bytesNeededForMove(const std::vector<std::string>& srcPaths,
                            const std::vector<GameItem::Kind>& kinds,
                            const std::string& dstDev) {
    return bytesNeededForOp(srcPaths, kinds, dstDev, /*isCopy=*/false);
}

class KernelFileExplorer;

struct KfeFileOps {
    static bool sameDevice(const std::string& a, const std::string& b) ;
    static bool ensureDir(const std::string& path) ;
    static bool ensureDirRecursive(const std::string& full) ;
    static bool isDirectoryPath(const std::string& path) ;
    static bool copyFile(const std::string& src, const std::string& dst, KernelFileExplorer* self) ;
    static bool removeDirRecursive(const std::string& dir) ;
    static bool copyDirRecursive(const std::string& src, const std::string& dst, KernelFileExplorer* self) ;
    static std::string subrootFor(const std::string& path, GameItem::Kind kind) ;
    static bool parseCategoryFromPath(const std::string& pathAfterSubroot, std::string& outCat, std::string& outLeaf) ;
    static std::string afterSubroot(const std::string& full, const std::string& subroot) ;
    static std::string buildDestPath(const std::string& srcPath,
                                     GameItem::Kind kind,
                                     const std::string& destDevice,
                                     const std::string& destCategory /* "" for Uncategorized */) ;
    static int kfeFastMoveDevctl(const char* src, const char* dst) ;
    static bool moveOne(const std::string& src, const std::string& dst, GameItem::Kind kind, KernelFileExplorer* self) ;
    static bool removeFileWithProgress(const std::string& path, KernelFileExplorer* self) ;
    static bool removeDirRecursiveProgress(const std::string& dir, KernelFileExplorer* self) ;
    static bool deleteOne(const std::string& path, GameItem::Kind kind, KernelFileExplorer* self) ;
    static void performDelete(KernelFileExplorer* self) ;
    static bool copyOne(const std::string& src, const std::string& dst, GameItem::Kind kind, KernelFileExplorer* self) ;
};




struct ClockGuard {
    int cpu, bus;
    ClockGuard() {
        cpu = scePowerGetCpuClockFrequencyInt();
        bus = scePowerGetBusClockFrequencyInt();
    }
    void boost333() { scePowerSetClockFrequency(333, 333, 166); }
    ~ClockGuard()   { scePowerSetClockFrequency(cpu, cpu, bus); }
};

// Slightly higher priority while OSK is active (lower number = higher prio)
struct ThreadPrioGuard {
    SceUID th{};
    int old{};
    ThreadPrioGuard(int newPrio = 0x10) {
        th = sceKernelGetThreadId();
        SceKernelThreadInfo ti{}; ti.size = sizeof(ti);
        if (sceKernelReferThreadStatus(th, &ti) >= 0) old = ti.currentPriority;
        sceKernelChangeThreadPriority(th, newPrio);
    }
    ~ThreadPrioGuard() {
        if (old) sceKernelChangeThreadPriority(th, old);
    }
};

#ifdef HAVE_SCEPOWERLOCK
struct PowerLockGuard {
    bool locked = false;
    PowerLockGuard()  { if (scePowerLock(0)   == 0) locked = true; }
    ~PowerLockGuard() { if (locked) scePowerUnlock(0); }
};
#endif

static void sortLikeLegacy(std::vector<GameItem>& v){
    std::sort(v.begin(), v.end(),
              [](const GameItem& a, const GameItem& b){ return a.sortKey > b.sortKey; }); // descending
}

// Case-insensitive A→Z sort of the working list.
void sortWorkingListAlpha(bool byTitle,
                          std::vector<GameItem>& workingList,
                          int& selectedIndex,
                          int& scrollOffset,
                          int visibleRows) {
    if (workingList.empty()) return;

    std::string keepPath;
    if (selectedIndex >= 0 && selectedIndex < (int)workingList.size())
        keepPath = workingList[selectedIndex].path;

    std::stable_sort(workingList.begin(), workingList.end(),
        [byTitle](const GameItem& a, const GameItem& b) {
            const std::string& sa = (byTitle && !a.title.empty()) ? a.title : a.label;
            const std::string& sb = (byTitle && !b.title.empty()) ? b.title : b.label;

            int c = strcasecmp(sa.c_str(), sb.c_str());
            if (c != 0) return c < 0;

            int c2 = strcasecmp(a.label.c_str(), b.label.c_str());
            if (c2 != 0) return c2 < 0;
            return a.path < b.path;
        });

    if (!keepPath.empty()) {
        for (int i = 0; i < (int)workingList.size(); ++i) {
            if (workingList[i].path == keepPath) { selectedIndex = i; break; }
        }
        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
        if (selectedIndex >= scrollOffset + visibleRows)
            scrollOffset = selectedIndex - visibleRows + 1;
        if (scrollOffset < 0) scrollOffset = 0;
    }
}

// ---------------------------------------------------------------
// App
// ---------------------------------------------------------------
static const char *gExecPath = nullptr;

static std::string getBaseDir(const char* execPath) {
    if (!execPath) return std::string("ms0:/");
    std::string p(execPath);
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return p + "/";
    return p.substr(0, pos + 1);
}

// -------- Simple File Ops Menu (modal) --------
struct FileOpsItem { const char* label; bool disabled; };

class FileOpsMenu {
public:
    FileOpsMenu(const char* title, const std::vector<FileOpsItem>& items,
                int screenW, int screenH, int w, int h)
    : _title(title ? title : ""), _items(items), _screenW(screenW), _screenH(screenH) {
        _w = w; _h = h; _x = (_screenW - _w)/2; _y = (_screenH - _h)/2;
    }
    void primeButtons(unsigned buttons) { _lastButtons = buttons; }
    
    bool update() {
        if (!_visible) return false;
        SceCtrlData pad{}; sceCtrlReadBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~_lastButtons;
        _lastButtons = pad.Buttons;

        if (pressed & PSP_CTRL_UP) {
            do { _sel = (_sel + (int)_items.size() - 1) % (int)_items.size(); } while (_items[_sel].disabled && _hasEnabled());
        } else if (pressed & PSP_CTRL_DOWN) {
            do { _sel = (_sel + 1) % (int)_items.size(); } while (_items[_sel].disabled && _hasEnabled());
        } else if (pressed & PSP_CTRL_CIRCLE) {
            _choice = -1; _visible = false;
        } else if (pressed & PSP_CTRL_CROSS) {
            if (!_items[_sel].disabled) { _choice = _sel; _visible = false; }
        }
        return _visible;
    }

    void render(intraFont* font) {
        if (!_visible) return;

        sceGuDisable(GU_DEPTH_TEST);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        _rect(0, 0, _screenW, _screenH, 0x88000000);

        const unsigned COLOR_PANEL  = 0xD0303030;
        const unsigned COLOR_BORDER = 0xFFFFFFFF;
        const unsigned COLOR_GLOW   = 0x80FFFFFF;
        _rect(_x-1, _y-1, _w+2, _h+2, COLOR_BORDER);
        _rect(_x,   _y,   _w,   _h,   COLOR_PANEL);

        if (font) {
            const int textOffsetY = 4;
            const int titleOffsetY = 2;
            const int controlsOffsetY = 0;
            const int padX = 10;
            const float titleScale = 0.9f;
            const float itemScale  = 0.8f;

            // Title
            const int titleY = _y + 12 + textOffsetY + titleOffsetY;
            intraFontSetStyle(font, titleScale, COLOR_WHITE, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            intraFontPrint(font, (float)(_x + padX), (float)titleY, _title.c_str());

            // Divider directly under the title (no subtitle block).
            const int hrY = titleY + 12;
            _hFadeLine(_x + padX, hrY, _w - (padX * 2), 1, 0x90, 16, 0x00C0C0C0);

            const int startY = hrY + 23;
            const int lineH  = 18;
            for (int i = 0; i < (int)_items.size(); ++i) {
                bool sel = (i == _sel);
                unsigned col = _items[i].disabled ? COLOR_GRAY : COLOR_WHITE;
                unsigned shadow = (sel && !_items[i].disabled) ? COLOR_GLOW : 0;
                intraFontSetStyle(font, itemScale, col, shadow, 0.f, INTRAFONT_ALIGN_LEFT);
                float itemY = (float)(startY + i*lineH);
                intraFontPrint(font, (float)(_x + 16), itemY, _items[i].label);
                if (sel && !_items[i].disabled) {
                    intraFontPrint(font, (float)(_x + 17), itemY, _items[i].label);
                }
            }

            // Controls
            const float iconH = 15.0f;
            const float controlsY = (float)(_y + _h - 18 + textOffsetY + controlsOffsetY);
            const float controlsTextY = controlsY + 1.0f;
            float cx = (float)(_x + padX);
            _drawTextureScaled(okIconTexture, cx, controlsY - 11.0f, iconH, 0xFFFFFFFF);
            cx += iconH + 6.0f;
            intraFontSetStyle(font, 0.7f, 0xFFBBBBBB, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            const char* selectLabel = "Select";
            intraFontPrint(font, cx - 2.0f, controlsTextY, selectLabel);
            cx += _measureText(font, 0.7f, selectLabel) + 12.0f;
            _drawTextureScaled(circleIconTexture, cx, controlsY - 11.0f, iconH, 0xFFFFFFFF);
            cx += iconH + 6.0f;
            intraFontSetStyle(font, 0.7f, 0xFFBBBBBB, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            intraFontPrint(font, cx - 2.0f, controlsTextY, "Close");
        }
    }

    bool visible() const { return _visible; }
    int  choice()  const { return _choice; }

private:
    static void _rect(int x, int y, int w, int h, unsigned color) {
        struct V { unsigned color; short x,y,z; };
        V* v = (V*)sceGuGetMemory(2*sizeof(V));
        v[0] = { color, (short)x, (short)y, 0 };
        v[1] = { color, (short)(x+w), (short)(y+h), 0 };
        sceGuDisable(GU_TEXTURE_2D);
        sceGuShadeModel(GU_FLAT);
        sceGuAmbientColor(0xFFFFFFFF);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    }
    bool _hasEnabled() const {
        for (auto& it : _items) if (!it.disabled) return true;
        return false;
    }

    static float _measureText(intraFont* font, float size, const char* s) {
        if (!s) return 0.0f;
        if (!font) return (float)(strlen(s) * 8) * size;
        intraFontSetStyle(font, size, COLOR_WHITE, 0, 0.f, INTRAFONT_ALIGN_LEFT);
        return intraFontMeasureText(font, s);
    }
    static void _drawTextureScaled(Texture* t, float x, float y, float targetH, unsigned color) {
        if (!t || !t->data || targetH <= 0.0f) return;
        const int w = t->width;
        const int h = t->height;
        const int tbw = t->stride;
        if (w <= 0 || h <= 0) return;
        float s = targetH / (float)h;
        float dw = (float)w * s;
        float dh = targetH;

        sceKernelDcacheWritebackRange(t->data, tbw * h * 4);
        sceGuTexFlush();
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, tbw, tbw, t->data);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuEnable(GU_TEXTURE_2D);

        struct V { float u, v; unsigned color; float x, y, z; };
        V* vtx = (V*)sceGuGetMemory(2 * sizeof(V));
        vtx[0].u = 0.0f;         vtx[0].v = 0.0f;
        vtx[0].x = x;            vtx[0].y = y;            vtx[0].z = 0.0f; vtx[0].color = color;
        vtx[1].u = (float)w;     vtx[1].v = (float)h;
        vtx[1].x = x + dw;       vtx[1].y = y + dh;       vtx[1].z = 0.0f; vtx[1].color = color;
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 |
                                  GU_VERTEX_32BITF  | GU_TRANSFORM_2D, 2, nullptr, vtx);
        sceGuDisable(GU_TEXTURE_2D);
    }
    static void _hFadeLine(int x, int y, int w, int h, uint8_t midAlpha, int fadePx, uint32_t rgb) {
        if (w <= 0 || h <= 0) return;
        if (fadePx * 2 > w) fadePx = w / 2;
        if (fadePx < 1) fadePx = 1;

        const int steps = 6;
        auto colWithAlpha = [&](uint8_t a)->unsigned { return (unsigned(a) << 24) | (rgb & 0x00FFFFFF); };

        for (int i = 0; i < steps; ++i) {
            int x0 = x + (fadePx * i) / steps;
            int x1 = x + (fadePx * (i + 1)) / steps;
            int segW = x1 - x0;
            if (segW <= 0) continue;
            uint8_t a = (uint8_t)((midAlpha * (i + 1)) / steps);
            _rect(x0, y, segW, h, colWithAlpha(a));
        }

        int midW = w - (fadePx * 2);
        if (midW > 0) _rect(x + fadePx, y, midW, h, colWithAlpha(midAlpha));

        for (int i = 0; i < steps; ++i) {
            int x0 = x + w - fadePx + (fadePx * i) / steps;
            int x1 = x + w - fadePx + (fadePx * (i + 1)) / steps;
            int segW = x1 - x0;
            if (segW <= 0) continue;
            uint8_t a = (uint8_t)((midAlpha * (steps - i)) / steps);
            _rect(x0, y, segW, h, colWithAlpha(a));
        }
    }

    std::string _title;
    std::vector<FileOpsItem> _items;
    int _screenW{}, _screenH{};
    int _x{}, _y{}, _w{}, _h{};
    int _sel = 0;
    bool _visible = true;
    int  _choice  = -1;
    unsigned _lastButtons = 0;
};
// -----------------------------------------------

// -------- Generic Option List Menu (modal with title + description) --------
struct OptionItem { const char* label; bool disabled; };

class OptionListMenu {
public:
    OptionListMenu(const char* title,
                   const char* description,
                   const std::vector<OptionItem>& items,
                   int screenW, int screenH)
    : _title(title), _desc(description), _items(items),
      _screenW(screenW), _screenH(screenH) {
        // A bit taller to fit the description
        _w = 340; _h = 160; _x = (_screenW - _w)/2; _y = (_screenH - _h)/2;
    }

    void primeButtons(unsigned buttons) { _lastButtons = buttons; }
    void setOptionsOffsetAdjust(int dy) { _optionsOffsetAdjust = dy; }
    void setMinVisibleRows(int rows) { _minVisibleRows = rows; }
    void setAllowTriangleDelete(bool allow) { _allowTriangleDelete = allow; }
    bool deleteRequested() const { return _deleteRequested; }

    // NEW: pre-position the highlight on the currently-selected option
    void setSelected(int idx) {
        if (idx >= 0 && idx < (int)_items.size() && !_items[idx].disabled) {
            _sel = idx;
        }
    }

    bool update() {
        if (!_visible) return false;
        _deleteRequested = false;
        SceCtrlData pad{}; sceCtrlReadBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~_lastButtons;
        _lastButtons = pad.Buttons;


        if (pressed & PSP_CTRL_UP) {
            do { _sel = (_sel + (int)_items.size() - 1) % (int)_items.size(); } while (_items[_sel].disabled && _hasEnabled());
        } else if (pressed & PSP_CTRL_DOWN) {
            do { _sel = (_sel + 1) % (int)_items.size(); } while (_items[_sel].disabled && _hasEnabled());
        } else if (pressed & PSP_CTRL_CIRCLE) {
            _choice = -1; _visible = false;
        } else if (pressed & PSP_CTRL_CROSS) {
            if (!_items[_sel].disabled) { _choice = _sel; _visible = false; }
        } else if ((pressed & PSP_CTRL_TRIANGLE) && _allowTriangleDelete) {
            if (!_items[_sel].disabled && _sel > 0) {
                _choice = _sel; _deleteRequested = true; _visible = false;
            }
        }
        if (!_items.empty()) {
            _ensureSelVisible(_lastVisibleRows > 0 ? _lastVisibleRows : (int)_items.size());
        }
        return _visible;
    }

    void render(intraFont* font) {
        if (!_visible) return;

        sceGuDisable(GU_DEPTH_TEST);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        _rect(0, 0, _screenW, _screenH, 0x88000000);

        const unsigned COLOR_PANEL  = 0xD0303030;
        const unsigned COLOR_BORDER = 0xFFFFFFFF;
        const unsigned COLOR_DESC   = 0xFFBBBBBB;
        const unsigned COLOR_GLOW   = 0x80FFFFFF;

        _rect(_x-1, _y-1, _w+2, _h+2, COLOR_BORDER);
        _rect(_x,   _y,   _w,   _h,   COLOR_PANEL);

        if (font) {
            const int textOffsetY = 4;
            const int titleOffsetY = 2;
            const int hrOffsetY = -9;
            const int optionsOffsetY = 6 + _optionsOffsetAdjust;
            const int controlsOffsetY = 3;
            const int padX = 10;
            const float titleScale = 0.9f;
            const float descScale  = 0.7f;
            const float itemScale  = 0.8f;

            // Title
            intraFontSetStyle(font, titleScale, COLOR_WHITE, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            intraFontPrint(font, (float)(_x + padX), (float)(_y + 12 + textOffsetY + titleOffsetY), _title ? _title : "");

            // Description
            int descY = _y + 30 + textOffsetY;
            const int descX = _x + padX;
            const int descW = _w - (padX * 2);
            std::vector<std::string> descLines;
            _wrapText(font, descScale, _desc, descW, descLines);
            const int descLineH = (int)(22.0f * descScale + 0.5f);
            intraFontSetStyle(font, descScale, COLOR_DESC, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            for (const auto& line : descLines) {
                intraFontPrint(font, (float)descX, (float)descY, line.c_str());
                descY += descLineH;
            }

            // Divider
            int hrY = descY + 4 + hrOffsetY;
            _hFadeLine(_x + padX, hrY, _w - (padX * 2), 1, 0x90, 16, 0x00C0C0C0);

            const int startY = descY + 10 + optionsOffsetY;
            int lineH  = 18;
            const float iconH = 15.0f;
            const float controlsY = (float)(_y + _h - 18 + textOffsetY + controlsOffsetY);
            const float controlsTextY = controlsY + 1.0f;
            const int listBottom = (int)(controlsY - 6.0f);
            const int availH = listBottom - startY;
            if (_minVisibleRows > 0 && availH > 0) {
                int testRows = availH / lineH;
                if (testRows < _minVisibleRows) {
                    int adj = availH / _minVisibleRows;
                    if (adj < 14) adj = 14;
                    lineH = adj;
                }
            }
            int visibleRows = (availH > 0) ? (availH / lineH) : 1;
            if (_minVisibleRows > 0 && visibleRows < _minVisibleRows) visibleRows = _minVisibleRows;
            if (visibleRows < 1) visibleRows = 1;
            _lastVisibleRows = visibleRows;
            _ensureSelVisible(visibleRows);

            // Items
            const int startIdx = _scroll;
            const int endIdx = std::min((int)_items.size(), startIdx + visibleRows);
            for (int i = startIdx; i < endIdx; ++i) {
                bool sel = (i == _sel);
                bool disabled = _items[i].disabled;
                unsigned col = disabled ? COLOR_GRAY : COLOR_WHITE;
                unsigned shadow = (sel && !disabled) ? COLOR_GLOW : 0;
                intraFontSetStyle(font, itemScale, col, shadow, 0.f, INTRAFONT_ALIGN_LEFT);
                float itemY = (float)(startY + (i - startIdx) * lineH);
                intraFontPrint(font, (float)(_x + 16), itemY, _items[i].label);
                if (sel && !disabled) intraFontPrint(font, (float)(_x + 17), itemY, _items[i].label);
            }

            if ((int)_items.size() > visibleRows) {
                const float trackX = (float)(_x + _w - 6);
                const float trackY = (float)startY;
                const float trackH = (float)(visibleRows * lineH);
                _rect((int)trackX, (int)trackY, 2, (int)trackH, 0x40000000);
                float thumbH = trackH * ((float)visibleRows / (float)_items.size());
                if (thumbH < 6.0f) thumbH = 6.0f;
                int maxScroll = (int)_items.size() - visibleRows;
                if (maxScroll < 0) maxScroll = 0;
                const float t = (maxScroll > 0) ? ((float)_scroll / (float)maxScroll) : 0.0f;
                const float thumbY = trackY + t * (trackH - thumbH);
                _rect((int)trackX, (int)thumbY, 2, (int)thumbH, 0xFFBBBBBB);
            }

            // Controls
            float cx = (float)(_x + padX);
            _drawTextureScaled(okIconTexture, cx, controlsY - 11.0f, iconH, 0xFFFFFFFF);
            cx += iconH + 6.0f;
            intraFontSetStyle(font, 0.7f, 0xFFBBBBBB, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            const char* selectLabel = "Select";
            intraFontPrint(font, cx - 2.0f, controlsTextY, selectLabel);
            cx += _measureText(font, 0.7f, selectLabel) + 12.0f;
            if (_allowTriangleDelete) {
                _drawTextureScaled(triangleIconTexture, cx, controlsY - 11.0f, iconH, 0xFFFFFFFF);
                cx += iconH + 6.0f;
                intraFontSetStyle(font, 0.7f, 0xFFBBBBBB, 0, 0.f, INTRAFONT_ALIGN_LEFT);
                const char* delLabel = "Delete";
                intraFontPrint(font, cx - 2.0f, controlsTextY, delLabel);
                cx += _measureText(font, 0.7f, delLabel) + 12.0f;
            }
            _drawTextureScaled(circleIconTexture, cx, controlsY - 11.0f, iconH, 0xFFFFFFFF);
            cx += iconH + 6.0f;
            intraFontSetStyle(font, 0.7f, 0xFFBBBBBB, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            intraFontPrint(font, cx - 2.0f, controlsTextY, "Close");
        }
    }

    bool visible() const { return _visible; }
    int  choice()  const { return _choice; }

private:
    void _ensureSelVisible(int visible) {
        if (visible < 1) visible = 1;
        int maxScroll = (int)_items.size() - visible;
        if (maxScroll < 0) maxScroll = 0;
        if (_sel < _scroll) _scroll = _sel;
        if (_sel >= _scroll + visible) _scroll = _sel - visible + 1;
        if (_scroll < 0) _scroll = 0;
        if (_scroll > maxScroll) _scroll = maxScroll;
    }
    static float _measureText(intraFont* font, float size, const char* s) {
        if (!s) return 0.0f;
        if (!font) return (float)(strlen(s) * 8) * size;
        intraFontSetStyle(font, size, COLOR_WHITE, 0, 0.f, INTRAFONT_ALIGN_LEFT);
        return intraFontMeasureText(font, s);
    }
    static void _wrapText(intraFont* font, float size, const char* text, int maxW, std::vector<std::string>& out) {
        out.clear();
        if (!text || !*text) return;
        const char* p = text;
        std::string line;
        while (*p) {
            if (*p == '\n') {
                out.push_back(line);
                line.clear();
                ++p;
                continue;
            }
            while (*p == ' ') ++p;
            if (!*p) break;
            const char* start = p;
            while (*p && *p != ' ' && *p != '\n') ++p;
            std::string word(start, p);

            std::string candidate = line.empty() ? word : (line + " " + word);
            float w = _measureText(font, size, candidate.c_str());
            if (w <= (float)maxW || line.empty()) {
                line = candidate;
            } else {
                out.push_back(line);
                line = word;
            }
        }
        if (!line.empty()) out.push_back(line);
    }
    static void _rect(int x, int y, int w, int h, unsigned color) {
        struct V { unsigned color; short x,y,z; };
        V* v = (V*)sceGuGetMemory(2*sizeof(V));
        v[0] = { color, (short)x, (short)y, 0 };
        v[1] = { color, (short)(x+w), (short)(y+h), 0 };
        sceGuDisable(GU_TEXTURE_2D);
        sceGuShadeModel(GU_FLAT);
        sceGuAmbientColor(0xFFFFFFFF);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    }
    static void _drawTextureScaled(Texture* t, float x, float y, float targetH, unsigned color) {
        if (!t || !t->data || targetH <= 0.0f) return;
        const int w = t->width;
        const int h = t->height;
        const int tbw = t->stride;
        if (w <= 0 || h <= 0) return;
        float s = targetH / (float)h;
        float dw = (float)w * s;
        float dh = targetH;

        sceKernelDcacheWritebackRange(t->data, tbw * h * 4);
        sceGuTexFlush();
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, tbw, tbw, t->data);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuEnable(GU_TEXTURE_2D);

        struct V { float u, v; unsigned color; float x, y, z; };
        V* vtx = (V*)sceGuGetMemory(2 * sizeof(V));
        vtx[0].u = 0.0f;         vtx[0].v = 0.0f;
        vtx[0].x = x;            vtx[0].y = y;            vtx[0].z = 0.0f; vtx[0].color = color;
        vtx[1].u = (float)w;     vtx[1].v = (float)h;
        vtx[1].x = x + dw;       vtx[1].y = y + dh;       vtx[1].z = 0.0f; vtx[1].color = color;

        sceGuDrawArray(GU_SPRITES,
                       GU_TEXTURE_32BITF | GU_VERTEX_32BITF |
                       GU_COLOR_8888    | GU_TRANSFORM_2D,
                       2, nullptr, vtx);
        sceGuDisable(GU_TEXTURE_2D);
    }
    static void _hFadeLine(int x, int y, int w, int h, uint8_t midAlpha, int fadePx, uint32_t rgb) {
        if (w <= 0 || h <= 0) return;
        if (fadePx * 2 > w) fadePx = w / 2;
        if (fadePx < 1) fadePx = 1;

        const int steps = 6;
        auto colWithAlpha = [&](uint8_t a)->unsigned { return (unsigned(a) << 24) | (rgb & 0x00FFFFFF); };

        for (int i = 0; i < steps; ++i) {
            int x0 = x + (fadePx * i) / steps;
            int x1 = x + (fadePx * (i + 1)) / steps;
            int segW = x1 - x0;
            if (segW <= 0) continue;
            uint8_t a = (uint8_t)((midAlpha * (i + 1)) / steps);
            _rect(x0, y, segW, h, colWithAlpha(a));
        }

        int midW = w - (fadePx * 2);
        if (midW > 0) _rect(x + fadePx, y, midW, h, colWithAlpha(midAlpha));

        for (int i = 0; i < steps; ++i) {
            int x0 = x + w - fadePx + (fadePx * i) / steps;
            int x1 = x + w - fadePx + (fadePx * (i + 1)) / steps;
            int segW = x1 - x0;
            if (segW <= 0) continue;
            uint8_t a = (uint8_t)((midAlpha * (steps - i)) / steps);
            _rect(x0, y, segW, h, colWithAlpha(a));
        }
    }
    bool _hasEnabled() const {
        for (auto& it : _items) if (!it.disabled) return true;
        return false;
    }

    const char* _title{};
    const char* _desc{};
    std::vector<OptionItem> _items;

    int _screenW{}, _screenH{};
    int _x{}, _y{}, _w{}, _h{};
    int _sel = 0;
    bool _visible = true;
    int  _choice  = -1;
    unsigned _lastButtons = 0;
    int _optionsOffsetAdjust = 0;
    int _minVisibleRows = 0;
    int _scroll = 0;
    int _lastVisibleRows = 0;
    bool _allowTriangleDelete = false;
    bool _deleteRequested = false;
};
// --------------------------------------------------------------------------
