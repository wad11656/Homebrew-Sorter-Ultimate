// main.cpp
// ----------------------------------------------------------------
// KernelFileExplorer — Folder-time-only sorter (XMB-accurate)
// Reorder flow: X = Pick/Drop, while picked use ↑/↓ to swap.
// START commits mtimes for the *current visual list*
//   - Bottom item gets "start" time, then +10s per step up,
//   - EBOOT entries: update **only the parent folder mtime**
//   - ISO entries:   update the ISO file mtime
//   - Initial list order = descending by chosen timestamp (folder time).
// HOME menu via exit callback.
//
// Debug UI:
//   - □ toggles timestamp overlay (shows the folder/file mtime used to sort).
// Tips:
//   - Hold ↑ or ↓ to fast-scroll (after a short delay).
//   - △ toggles label: File/Folder vs App Title
//   - Saving shows a passive "Saving..." modal overlay.
//   - Lower-right: shows ICON0.PNG for the selected entry (EBOOT or ISO-like).
//
// Extras in this build:
//   - Clean ICON0 rendering (no initial gray lines at bottom).
//   - Fallback to /resources/icon0.png when embedded ICON0 is missing.
//   - Do not keep retrying failed ICON0 loads for the same item (session-sticky).
//   - Never draw ICON0 on device list or over modals.
//   - **L trigger: Rename (CAT_ folders, files, and EBOOT folders)**
//   - **OSK loop clears/swap each tick to prevent alpha stacking (no sceUtilityOskDraw)**.
//   - **OSK speed tweaks: minimal backdrop clear, thread-priority boost,
//      optional power lock, restricted input mask, vblank CB, reusable buffer.**
//
// New in this update:
//   - **Triangle → File ops menu → Move is fully implemented** with the rules you specified:
//       • PSP Go running from Memory Stick (ms0): first select destination device (ms0/ef0),
//         then (if categories exist on that destination) select a destination category, else go straight to confirm.
//       • Non-PSP Go or PSP Go running off ef0: Move is enabled only if categories exist;
//         selecting Move goes straight to category selection on the current device.
//       • In Move mode you cannot browse into file listings; selecting a category triggers a confirmation dialog,
//         then moves either the currently-highlighted item (if nothing is checked) or all checked items.
//       • Destination paths are computed as requested, preserving the source subroot (e.g., PSP/GAME vs ISO vs ISO/PSP),
//         applying CAT_ folders when a category is chosen, or omitting them for Uncategorized.
//       • Same-device moves prefer sceIoRename(); cross-device moves use copy-then-delete (recursive for folders).
// ----------------------------------------------------------------

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspdebug.h>
#include <psppower.h>
#include <psputility.h>
#include <psputility_osk.h>
#include <pspthreadman.h>   // thread priority tweaks
#include <kubridge.h>
#include <intraFont.h>
#include <pspusb.h>
#include <pspusbstor.h>

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <stdint.h>
#include <map>
#include <unordered_set>
#include <stdarg.h>

#include "Texture.h"
#include "MessageBox.h"
#include "iso_titles_extras.h"
// Load the mass-storage stack in safe order. Always ms0; add ef0 on PSP Go.
static int LoadStartKMod(const char* path);
static bool DeviceExists(const char* root);

static void EnsureUsbKernelModules() {
    // Core sync + mass-storage core
    LoadStartKMod("flash0:/kd/semawm.prx");
    LoadStartKMod("flash0:/kd/usbstor.prx");
    LoadStartKMod("flash0:/kd/usbstormgr.prx");

    // PSP Go internal storage backend (ef0) only if present
    if (DeviceExists("ef0:/")) {
        LoadStartKMod("flash0:/kd/usbstoreflash.prx");
    }

    // Memory Stick backend
    LoadStartKMod("flash0:/kd/usbstorms.prx");
}


static int LoadStartKMod(const char* path) {
    SceUID mod = kuKernelLoadModule(path, 0, NULL);
    if (mod >= 0) {
        int status;
        sceKernelStartModule(mod, 0, NULL, &status, NULL);
    }
    return mod;
}
static bool DeviceExists(const char* root) {
    SceUID fd = sceIoDopen(root);
    if (fd >= 0) { sceIoDclose(fd); return true; }
    return false;
}

static int LoadStartKMod(const char* path);
static bool DeviceExists(const char* root);



PSP_MODULE_INFO("KernelFileExplorer", 0x800, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(4096);

// Human-readable byte formatter (SI: kB/MB/GB) or binary (KiB/MiB/GiB)
#define HUMAN_BYTES_SI 1  // 1 = kB/MB/GB (1000), 0 = KiB/MiB/GiB (1024)

static std::string humanBytes(uint64_t b) {
    char buf[32];

#if HUMAN_BYTES_SI
    const double KB = 1000.0, MB = 1000.0*1000.0, GB = 1000.0*1000.0*1000.0;
    if (b >= (uint64_t)GB) {
        snprintf(buf, sizeof(buf), "%.2f GB", b / GB);
    } else if (b >= (uint64_t)MB) {
        snprintf(buf, sizeof(buf), "%.2f MB", b / MB);
    } else if (b >= (uint64_t)KB) {
        snprintf(buf, sizeof(buf), "%.2f kB", b / KB);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    }
#else
    const double KiB = 1024.0, MiB = 1024.0*1024.0, GiB = 1024.0*1024.0*1024.0;
    if (b >= (uint64_t)GiB) {
        snprintf(buf, sizeof(buf), "%.2f GiB", b / GiB);
    } else if (b >= (uint64_t)MiB) {
        snprintf(buf, sizeof(buf), "%.2f MiB", b / MiB);
    } else if (b >= (uint64_t)KiB) {
        snprintf(buf, sizeof(buf), "%.2f KiB", b / KiB);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    }
#endif

    return std::string(buf);
}

// --- ultra-compact human size (<=3 digits), units: B,K,M,G (1000 base) ---
static std::string humanSize3(uint64_t bytes) {
    const char unit[4] = {'B','K','M','G'};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000.0 && u < 3) { v /= 1000.0; ++u; }

    char buf[16];
    if (v >= 100.0)      snprintf(buf, sizeof(buf), "%.0f%c",  v, unit[u]); // 100..999
    else if (v >= 10.0)  snprintf(buf, sizeof(buf), "%.1f%c",  v, unit[u]); // 10..99.9
    else                 snprintf(buf, sizeof(buf), "%.2f%c",  v, unit[u]); // 0..9.99

    // Trim trailing ".0" (e.g., "10.0K" -> "10K")
    int n = (int)strlen(buf);
    if (n >= 3 && buf[n-2] == '.' && buf[n-1] == '0') { buf[n-2] = buf[n-1]; buf[n-1] = '\0'; }
    return std::string(buf);
}



// Stub out PSP IO functions so plugin builds
extern "C" {
    int pspIoOpenDir(const char *dirname);
    int pspIoReadDir(SceUID dir, SceIoDirent *dirent);
    int pspIoCloseDir(SceUID dir);
    int pspIoGetstat(const char *file, SceIoStat *stat);
    int pspIoChstat(const char *file, SceIoStat *stat, int bits);
    int sceIoOpen(const char *file, int flags, SceMode mode);
    int sceIoClose(SceUID fd);
    int sceIoRemove(const char *file);
    int sceIoRename(const char *oldname, const char *newname);
    int sceIoMkdir(const char *dir, SceMode mode);
    int sceIoRmdir(const char *dir);
    int sceIoRead(SceUID fd, void* data, SceSize size);
    int sceIoWrite(SceUID fd, const void* data, SceSize size);
    int sceIoLseek32(SceUID fd, int offset, int whence);
    int sceIoDopen(const char* dir);
    int sceIoDread(SceUID dd, SceIoDirent* dir);
    int sceIoDclose(SceUID dd);
    int pspIoDevctl(const char* dev, unsigned int cmd,
                    void* indata, int inlen,
                    void* outdata, int outlen);
}

// Path split helpers
static std::string dirnameOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    if (s == std::string::npos) return "";
    return p.substr(0, s);
}
static std::string basenameOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? p : p.substr(s+1);
}
static std::string fileExtOf(const std::string& name) {
    size_t d = name.find_last_of('.');
    return (d == std::string::npos) ? "" : name.substr(d);
}
static bool dirExists(const std::string& path){
    SceUID d = pspIoOpenDir(path.c_str());
    if (d >= 0){ pspIoCloseDir(d); return true; }
    return false;
}
static std::string joinDirFile(const std::string& dir, const char* fname){
    if (!dir.empty() && dir[dir.size()-1]=='/') return dir + fname;
    return dir + "/" + fname;
}

// ---------- OSK speed-tuning toggles ----------
#define OSK_MINIMAL_BACKDROP   1
#define OSK_USE_VBLANK_CB      1
#ifdef HAVE_SCEPOWERLOCK
extern "C" int scePowerLock(int);
extern "C" int scePowerUnlock(int);
#endif
// ---------------------------------------------

// ===== Optional: run the entire app at 333 MHz =====
//   0 = keep stock clock except where guarded by ClockGuard
//   1 = set CPU=333, BUS=166 for the whole app session
#define FORCE_APP_333  1

#define SCREEN_WIDTH   480
#define SCREEN_HEIGHT  272
#define LIST_START_Y    50
#define ITEM_HEIGHT     12
#define MAX_DISPLAY     16

// ABGR colors
#define COLOR_BG       0xFF202020
#define COLOR_WHITE    0xFFFFFFFF
#define COLOR_YELLOW   0xFF00FFFF
#define COLOR_CYAN     0xFFFFFF00
#define COLOR_GRAY     0xFF808080
#define COLOR_BLACK    0xFF000000
#define COLOR_GREEN    0xFF00FF00
#define COLOR_RED      0xFF0000FF

// OSK backdrop color: #205068 (RGB) => 0xFF685020 (ABGR)
static uint32_t gOskBgColorABGR = 0xFF685020;

// Key repeat params (microseconds)
#define REPEAT_DELAY_US          250000ULL
#define REPEAT_INTERVAL_US        50000ULL
#define REPEAT_ACCEL_AFTER_US    800000ULL
#define REPEAT_INTERVAL_FAST_US   16000ULL

// USB state
static bool gUsbActive = false;
static MessageBox* gUsbBox = nullptr;


static bool gUsbShownConnected = false;
// USB helpers
static int UsbStartStacked() {
    EnsureUsbKernelModules();
    (void)sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
    (void)sceUsbStart(PSP_USBSTOR_DRIVERNAME, 0, 0);
    return 0;}
static void UsbStopStacked() {
    sceUsbDeactivate(0x1c8);
    sceUsbStop(PSP_USBSTOR_DRIVERNAME, 0, 0);
    sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);
}
static void UsbActivate()   { sceUsbActivate(0x1c8); }
static void UsbDeactivate() { sceUsbDeactivate(0x1c8); }


// ISO constants
#define ISO_SECTOR 2048

#define REPLACE_ON_MOVE 1

static unsigned int __attribute__((aligned(16))) list[262144];

static Texture* backgroundTexture = nullptr;
static Texture* okIconTexture = nullptr;
static Texture* placeholderIconTexture = nullptr;
// Checkbox icons (12x12 recommended)
static Texture* checkTexUnchecked = nullptr;
static Texture* checkTexChecked   = nullptr;
static const int CHECKBOX_PX      = 11;

// Reserve ~4–5 chars for sizes (e.g., "123M") so it clears the left tag.
static const int SIZE_FIELD_RIGHT_X = 72;               // <--- moved right

// Push checkbox to the right of the size field (add a little gap)
static const int CHECKBOX_X       = SIZE_FIELD_RIGHT_X + 10; // ~136px   <--- moved right
static const int CHECKBOX_Y_NUDGE = -6;

// Start filename a bit after the checkbox
static const int NAME_TEXT_X      = CHECKBOX_X + 14;     // ~154px     <--- new

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

static SceUID gLogFd = -1;
static void logInit() {
    if (gLogFd >= 0) return;
    gLogFd = sceIoOpen("ms0:/KFE_move.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    if (gLogFd < 0) gLogFd = sceIoOpen("ef0:/KFE_move.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
}
static void logWrite(const char* s) { if (gLogFd >= 0) sceIoWrite(gLogFd, s, (int)strlen(s)); }
static void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logWrite(buf); logWrite("\r\n");
}
static void logClose(){ if (gLogFd >= 0) { sceIoClose(gLogFd); gLogFd = -1; } }


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
    SceUID d = pspIoOpenDir(dir.c_str());
    if (d < 0) return true;
    bool empty = true;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (pspIoReadDir(d, &ent) > 0) {
        if (strcmp(ent.d_name, ".") && strcmp(ent.d_name, "..")) { empty = false; break; }
        memset(&ent, 0, sizeof(ent));
    }
    pspIoCloseDir(d);
    return empty;
}

// --- remove recursively (you already have a version; keep one) ---
static bool removeDirRecursive(const std::string& dir) {
    SceUID d = pspIoOpenDir(dir.c_str());
    if (d < 0) return sceIoRmdir(dir.c_str()) >= 0;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (pspIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
        std::string child = joinDirFile(dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) removeDirRecursive(child);
        else sceIoRemove(child.c_str());
        memset(&ent, 0, sizeof(ent));
        sceKernelDelayThread(0);
    }
    pspIoCloseDir(d);
    return sceIoRmdir(dir.c_str()) >= 0;
}

// --- fast per-file rename based folder move (same device, no data copy) ---
static bool fastMoveDirByRenames(const std::string& srcDir, const std::string& dstDir) {
    logf("fastMoveDirByRenames: %s -> %s", srcDir.c_str(), dstDir.c_str());
    if (!ensureDirRecursive(dstDir)) { logf("  ensureDirRecursive(dst) FAILED"); return false; }

    SceUID d = pspIoOpenDir(srcDir.c_str());
    if (d < 0) { logf("  open src failed %d", d); return false; }

    bool ok = true;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (ok && pspIoReadDir(d, &ent) > 0) {
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
    pspIoCloseDir(d);
    logf("fastMoveDirByRenames: %s", ok ? "OK" : "FAIL");
    return ok;
}

// --- size calculators (for preflight) ---
static bool sumDirBytes(const std::string& dir, uint64_t& out) {
    logf("sumDirBytes: enter %s (start=%llu)", dir.c_str(), (unsigned long long)out);
    SceUID d = pspIoOpenDir(dir.c_str()); if (d < 0) return false;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (pspIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name,".") || !strcmp(ent.d_name,"..")) { memset(&ent,0,sizeof(ent)); continue; }
        std::string p = joinDirFile(dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) { if (!sumDirBytes(p, out)) { pspIoCloseDir(d); return false; } }
        else out += (uint64_t)ent.d_stat.st_size;
        memset(&ent, 0, sizeof(ent));
    }
    pspIoCloseDir(d);
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
static const char* rootDisplayName(const char* r) {
    if (!r) return "";
    if (!strcmp(r, "__USB_MODE__")) return "USB Mode";
    if (!strcmp(r, "ms0:/")) return "ms0:/ (Memory Stick)";
    if (!strcmp(r, "ef0:/")) return "ef0:/ (Internal)";
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
    SceUID d = pspIoOpenDir(dpath.c_str());
    if (d < 0) return {};
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (pspIoReadDir(d, &ent) > 0) {
        if (!FIO_S_ISDIR(ent.d_stat.st_mode) && strcasecmp(ent.d_name, "EBOOT.PBP") == 0) {
            std::string full = joinDirFile(dpath, ent.d_name); pspIoCloseDir(d); return full;
        }
        memset(&ent, 0, sizeof(ent));
    }
    pspIoCloseDir(d);
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

// dir iterator
template<typename F>
static void forEachEntry(const std::string& dir, F f){
    std::string dpath = dir;
    if (!dpath.empty() && dpath[dpath.size()-1]=='/') dpath.erase(dpath.size()-1);
    SceUID d = pspIoOpenDir(dpath.c_str());
    if (d < 0) return;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (pspIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
        if (isJunkHidden(ent.d_name))                                 { memset(&ent,0,sizeof(ent)); continue; }
        f(ent);
        memset(&ent, 0, sizeof(ent));
    }
    pspIoCloseDir(d);
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
    SceUID d = pspIoOpenDir(dpath.c_str());
    if (d < 0) return {};
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    std::string out;
    while (pspIoReadDir(d, &ent) > 0) {
        if (!FIO_S_ISDIR(ent.d_stat.st_mode) && strcasecmp(ent.d_name, wantName) == 0) {
            out = joinDirFile(dpath, ent.d_name);
            break;
        }
        memset(&ent, 0, sizeof(ent));
    }
    pspIoCloseDir(d);
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

// Returns how many "games" are inside the CAT_ across ISO and GAME roots on the given device.
// Games = ISO-like files (.iso/.cso/.zso/.dax/.jso) in ISO roots,
//      or folders under PSP/GAME.../CAT_* that contain an EBOOT.PBP (case-insensitive).
static int countGamesInCategory(const std::string& device, const std::string& cat) {
    if (!startsWithCAT(cat.c_str())) return 0;

    const char* isoRoots[]  = {"ISO/","ISO/PSP/"};
    const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};

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

    // EBOOT folders (only immediate children of the CAT_ directory)
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
static void createCategoryDirs(const std::string& device, const std::string& cat) {
    const char* isoRoots[]  = {"ISO/","ISO/PSP/"};
    const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};
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
    const char* isoRoots[]  = {"ISO/","ISO/PSP/"};
    const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};
    for (auto r : isoRoots)  { std::string p = device + std::string(r) + cat; if (dirExists(p)) removeDirRecursive(p); }
    for (auto r : gameRoots) { std::string p = device + std::string(r) + cat; if (dirExists(p)) removeDirRecursive(p); }
}


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
                          int& scrollOffset) {
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
        if (selectedIndex >= scrollOffset + MAX_DISPLAY)
            scrollOffset = selectedIndex - MAX_DISPLAY + 1;
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
    FileOpsMenu(const std::vector<FileOpsItem>& items, int screenW, int screenH)
    : _items(items), _screenW(screenW), _screenH(screenH) {
        _w = 280; _h = 120; _x = (_screenW - _w)/2; _y = (_screenH - _h)/2;
    }

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
        _rect(_x-1, _y-1, _w+2, _h+2, COLOR_BORDER);
        _rect(_x,   _y,   _w,   _h,   COLOR_PANEL);

        if (font) {
            intraFontSetStyle(font, 0.9f, COLOR_WHITE, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            intraFontPrint(font, (float)(_x + 10), (float)(_y + 12), "File operations");

            const int startY = _y + 36;
            const int lineH  = 18;
            for (int i = 0; i < (int)_items.size(); ++i) {
                bool sel = (i == _sel);
                unsigned col = _items[i].disabled ? COLOR_GRAY : COLOR_WHITE;
                if (sel) {
                    _rect(_x + 8, startY + i*lineH - 2, _w - 16, lineH + 4, 0x40FFFFFF);
                }
                intraFontSetStyle(font, 0.8f, col, 0, 0.f, INTRAFONT_ALIGN_LEFT);
                intraFontPrint(font, (float)(_x + 16), (float)(startY + i*lineH), _items[i].label);
            }
            intraFontSetStyle(font, 0.7f, COLOR_GRAY, 0, 0.f, INTRAFONT_ALIGN_LEFT);
            intraFontPrint(font, (float)(_x + 10), (float)(_y + _h - 16), "X: Select   O: Close");
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

    std::vector<FileOpsItem> _items;
    int _screenW{}, _screenH{};
    int _x{}, _y{}, _w{}, _h{};
    int _sel = 0;
    bool _visible = true;
    int  _choice  = -1;
    unsigned _lastButtons = 0;
};
// -----------------------------------------------


class KernelFileExplorer {
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

    enum View { View_Categories, View_CategoryContents, View_AllFlat } view = View_AllFlat;
    std::string currentCategory;

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


    // Per-row flags (roots view)
    std::vector<uint8_t> rowFlags;
    static constexpr uint8_t ROW_DISABLED = 1 << 0;

    // NEW: reason + numbers for warning text in roots view
    enum RowDisableReason { RD_NONE = 0, RD_RUNNING_FROM_EF0 = 1, RD_NO_SPACE = 2 };
    std::vector<uint64_t> rowFreeBytes;
    std::vector<RowDisableReason> rowReason;   // <--- add
    std::vector<uint64_t>        rowNeedBytes; // <--- add

    int selectedIndex = 0;
    int scrollOffset  = 0;

    // Paths currently checked
    std::unordered_set<std::string> checked;

    // Pick/drop state
    bool moving = false;

    intraFont*  font   = nullptr;
    MessageBox* msgBox = nullptr;
    FileOpsMenu* fileMenu = nullptr;
    bool inputWaitRelease = false;

    // Track which kind of menu is open (content vs categories)
    enum MenuContext { MC_ContentOps, MC_CategoryOps };
    MenuContext menuContext = MC_ContentOps;

    // Debug overlay
    bool showDebugTimes = false; // toggle with Square
    // Label mode
    bool showTitles = false;     // toggle with Triangle

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


    void exitOpMode() {
        actionMode = AM_None;
        opPhase    = OP_None;
        opSrcPaths.clear();
        opSrcKinds.clear();
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


    static void replaceCatSegmentInPath(const std::string& oldCat, const std::string& newCat,
                                        std::string& pathInOut) {
        size_t pos = pathInOut.find(oldCat);
        if (pos != std::string::npos) pathInOut.replace(pos, oldCat.size(), newCat);
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
    }


    void cachePatchRenameItem(const std::string& oldPath, const std::string& newPath, GameItem::Kind k) {
        auto apply = [&](ScanSnapshot& s){
            snapErasePath(s, oldPath);
            const std::string dstCat = deriveCategoryFromPath(newPath);
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
        const std::string dstCat = deriveCategoryFromPath(dst);
        snapUpsertItem(dstSnap, gi, dstCat);
    }


    void rebuildFlatFromCache() {
        // Same as openDevice(...) else-branch when hasCategories == false
        // workingList ← flatAll, sort, set view, clear UI, populate rows, hide roots.
        workingList = flatAll;
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
    void drawText(float x,float y,const char* s,unsigned col) {
        if (font) {
            intraFontSetStyle(font,0.5f,col,0,0.0f,INTRAFONT_ALIGN_LEFT);
            intraFontPrint(font,x,y,s);
        } else {
            pspDebugScreenSetXY(int(x/8), int(y/8));
            pspDebugScreenSetTextColor(col);
            pspDebugScreenPrintf("%s", s);
        }
    }
    void drawHeader() {
        char head[256];
        snprintf(head, sizeof(head), "Kernel File Explorer - Timestamp Editor  |  Sort: Folder mtime  |  Debug: %s",
                 showDebugTimes ? "ON" : "OFF");
        drawText(10,10,head,COLOR_YELLOW);

        if (showRoots) {
            if (actionMode != AM_None) {
                const char* v = (actionMode == AM_Copy) ? "Copy" : "Move";
                std::string msg = std::string("Select Destination Storage (") + v + " Mode)";
                drawText(10, 25, msg.c_str(), COLOR_WHITE);
            }
            else drawText(10,25,"Select Storage Device",COLOR_WHITE);
        } else {
            char buf[256];
            const char* lbl = showTitles ? "App Title" : "File/Folder";
            if (view == View_Categories) {
                if (actionMode != AM_None) {
                    snprintf(buf, sizeof(buf), "Move: Pick Destination Category — %s", rootDisplayName(currentDevice.c_str()));
                } else {
                    snprintf(buf, sizeof(buf), "Categories — %s  | Label: %s", rootDisplayName(currentDevice.c_str()), lbl);
                }
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

    Texture* loadIconForGameItem(const GameItem& gi) {
        if (gi.kind == GameItem::EBOOT_FOLDER) {
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
        if (t) selectionIconTex = t;
        else { selectionIconTex = placeholderIconTexture; noIconPaths.insert(key); }
        selectionIconKey = key;
    }


    void drawSelectedIconLowerRight() {
        if (!selectionIconTex || !selectionIconTex->data) return;

        const int boxW = 144, boxH = 80;
        const int controlsTop = SCREEN_HEIGHT - 30;

        const int w   = selectionIconTex->width;
        const int h   = selectionIconTex->height;
        const int tbw = selectionIconTex->stride;

        float sx = (float)boxW / (float)w;
        float sy = (float)boxH / (float)h;
        float s  = (sx < sy) ? sx : sy;
        int dw = (int)(w * s), dh = (int)(h * s);

        int x = SCREEN_WIDTH - 12 - dw;
        int y = controlsTop - 6 - dh;

        drawRect(x-2, y-2, dw+4, dh+4, 0xFF000000);
        drawRect(x-1, y-1, dw+2, dh+2, 0xFF404040);

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

    void drawFileList() {
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

            // NEW: when picking a destination category, gray out categories marked as disabled
            if (!showRoots && view==View_Categories && actionMode!=AM_None && opPhase==OP_SelectCategory) {
                bool disabledCat = (opDisabledCategories.find(entries[i].d_name) != opDisabledCategories.end());
                if (disabledCat) labelCol = COLOR_GRAY;
            }

            drawText(10,y+2, isDir ? "[DIR]" : (isMoveRow ? "[MOVE]" : "FILE"), labelCol);

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
        int y = SCREEN_HEIGHT-30;
        const char* mode = showTitles ? "Title" : "Name";
        if (actionMode != AM_None) {
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


        if (showRoots) {
            drawText(10,y,"X: Select Device",COLOR_WHITE);
        } else if (view == View_Categories) {
            drawText(10,y,"X: Open Category | L: Rename CAT_ | O: Back to Devices | △: Label Title/Name",COLOR_WHITE);
        } else if (view == View_CategoryContents || view == View_AllFlat) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "%s | START: Save | L: Rename | O: Back | □: Debug | △: Label (%s) | SELECT: A→Z (%s) | Hold ↑/↓: Fast",
                moving ? "X: Drop | ↑/↓: Swap" : "X: Pick | ↑/↓: Swap",
                mode, mode);
            drawText(10,y, buf, COLOR_WHITE);
        }
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
        data.outtextlength = maxChars;
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
            out = sanitizeFilename(utf16ToUtf8(out16.data()));
            return true;
        }
        return false;
    }

    // ===================== Rename logic (unchanged) =====================
    void beginRenameSelected() {
        if (showRoots || moving) return;

        if (view == View_Categories) {
            if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;
            std::string oldName = entries[selectedIndex].d_name;
            if (!strcasecmp(oldName.c_str(), "Uncategorized")) return;
            if (!startsWithCAT(oldName.c_str())) return;

            std::string typed;
            if (!promptTextOSK("Rename Category", oldName.c_str(), 64, typed)) return;
            if (!startsWithCAT(typed.c_str())) typed = std::string("CAT_") + typed;
            if (typed == oldName) return;

            msgBox = new MessageBox("Renaming...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
            renderOneFrame();

            const char* isoRoots[]  = {"ISO/","ISO/PSP/"};
            const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};
            bool anyOk=false, anyFail=false;

            for (auto r : isoRoots) {
                std::string base = currentDevice + std::string(r);
                std::string from = base + oldName;
                std::string to   = base + typed;
                if (dirExists(from)) {
                    int rc = sceIoRename(from.c_str(), to.c_str());
                    (rc >= 0) ? anyOk=true : anyFail=true;
                }
            }
            for (auto r : gameRoots) {
                std::string base = currentDevice + std::string(r);
                std::string from = base + oldName;
                std::string to   = base + typed;
                if (dirExists(from)) {
                    int rc = sceIoRename(from.c_str(), to.c_str());
                    (rc >= 0) ? anyOk=true : anyFail=true;
                }
            }

            // Patch cache & UI while modal is still up
            cachePatchRenameCategory(oldName, typed);
            buildCategoryRows();
            // reselect as needed, possibly show a short "Renamed" toast
            drawMessage("Renamed", COLOR_GREEN);
            sceKernelDelayThread(600*1000);

            // NOW dismiss the modal
            delete msgBox; msgBox = nullptr;
            renderOneFrame();
            for (int i=0;i<(int)entries.size();++i)
                if (!strcmp(entries[i].d_name, typed.c_str())) { selectedIndex=i; break; }

            drawMessage(anyOk && !anyFail ? "Category renamed" : (anyOk ? "Some renamed" : "Rename failed"),
                        anyOk ? COLOR_GREEN : COLOR_RED);
            sceKernelDelayThread(600*1000);
            return;
        }

        if (view == View_AllFlat || view == View_CategoryContents) {
            if (selectedIndex < 0 || selectedIndex >= (int)workingList.size()) return;
            GameItem gi = workingList[selectedIndex];
            std::string dir  = dirnameOf(gi.path);
            std::string base = basenameOf(gi.path);

            std::string typed;
            if (!promptTextOSK("Rename", base.c_str(), 64, typed)) return;
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
                drawMessage("Name exists", COLOR_RED);
                sceKernelDelayThread(700*1000);
                return;
            }

            bool wasChecked = (checked.find(gi.path) != checked.end());

            msgBox = new MessageBox("Renaming...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
            renderOneFrame();

                int rc = sceIoRename(gi.path.c_str(), newPath.c_str());
                if (rc < 0) {
                    delete msgBox; msgBox = nullptr;
                    drawMessage("Rename failed", COLOR_RED);
                    sceKernelDelayThread(700*1000);
                    return;
                }

                // If we were showing an ICON0 for this exact item, carry it across the path change.
                // (Don't carry the placeholder; only a real texture.)
                if (selectionIconTex && selectionIconTex != placeholderIconTexture && selectionIconKey == gi.path) {
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

                // Patch cache instead of rescanning
                cachePatchRenameItem(gi.path, newPath, gi.kind);

                // Refresh just the current view
                if (view == View_CategoryContents) {
                    openCategory(currentCategory);
                } else if (view == View_AllFlat) {
                    rebuildFlatFromCache();
                } else {
                    buildCategoryRows();
                }

                // Re-select the renamed item; ensureSelectionIcon() will snap the carried ICON0 into place.
                selectByPath(keepPath);
                drawMessage("Renamed", COLOR_GREEN);
                sceKernelDelayThread(600*1000);

                // NOW dismiss the "Renaming..." modal
                delete msgBox; msgBox = nullptr;
                renderOneFrame();

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

        ensureSelectionIcon();
        drawSelectedIconLowerRight();

        if (msgBox)  msgBox->render(font);
        if (gUsbBox) gUsbBox->render(font);
        if (fileMenu) fileMenu->render(font);

        // (MessageBox input is handled exclusively in run().)
        if (gUsbBox) {
            if (!gUsbBox->update()) {
                // User pressed Circle → disconnect and close
                UsbDeactivate();
                gUsbActive = false;
                delete gUsbBox; gUsbBox = nullptr;
                inputWaitRelease = true;
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
                if (selectedIndex >= scrollOffset + MAX_DISPLAY)
                    scrollOffset = selectedIndex - MAX_DISPLAY + 1;
                return;
            }
        }
    }

    // --- add near other small helpers in the class (optional, but tidy) ---
    void bulkSelect(int dir) {
        if (showRoots) return;
        if (!(view == View_AllFlat || view == View_CategoryContents)) return;
        if (selectedIndex < 0 || selectedIndex >= (int)workingList.size()) return;

        // Always mark the current row first
        const std::string& cur = workingList[selectedIndex].path;
        checked.insert(cur);

        // dir = -1 for up, +1 for down
        int j = selectedIndex + dir;
        while (j >= 0 && j < (int)workingList.size()) {
            const std::string& p = workingList[j].path;
            // Stop at the first row that's already checked (barrier)
            if (checked.find(p) != checked.end()) break;
            checked.insert(p);
            j += dir;
        }
    }


    // -----------------------------------------------------------
    // Root detection & scanning
    // -----------------------------------------------------------
    void detectRoots() {
        roots.clear();
        runningFromEf0 = (gExecPath && strncmp(gExecPath, "ef0:", 4) == 0);

        { SceUID d = pspIoOpenDir("ms0:/"); if (d >= 0) { pspIoCloseDir(d); roots.push_back("ms0:/"); } }
        { SceUID d = pspIoOpenDir("ef0:/"); if (d >= 0) { pspIoCloseDir(d); roots.push_back("ef0:/"); } }

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

    void scanIsoRootDir(const std::string& base){
        if (!dirExists(base)) return;
        forEachEntry(base, [&](const SceIoDirent &e){
            std::string name = e.d_name;
            if (FIO_S_ISDIR(e.d_stat.st_mode)) {
                if (startsWithCAT(name.c_str())){
                    hasCategories = true;

                    // NEW: ensure the category is created/listed even if empty or contains no ISO-like files
                    categories[name];  // creates empty vector if not present

                    std::string catDir = base + name;
                    forEachEntry(catDir, [&](const SceIoDirent &ee){
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
                                    gi.sizeBytes= (uint64_t)st.st_size;   // <--- NEW
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
                            }
                        }
                    });
                }
            } else {
                if (isIsoLike(name)){
                    GameItem gi; gi.kind = GameItem::ISO_FILE;
                    gi.label = name;
                    gi.path  = joinDirFile(base, name.c_str());
                    SceIoStat st;
                    if (getStat(gi.path, st)){
                        gi.time     = st.sce_st_mtime;
                        gi.sortKey  = buildLegacySortKey(gi.time);
                        gi.sizeBytes= (uint64_t)st.st_size;   // <--- NEW
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
                }
            }
        });
    }

    void scanGameRootDir(const std::string& base){
        if (!dirExists(base)) return;
        forEachEntry(base, [&](const SceIoDirent &e){
            if (!FIO_S_ISDIR(e.d_stat.st_mode)) return;
            std::string name = e.d_name;
            if (startsWithCAT(name.c_str())){
                hasCategories = true;

                // NEW: ensure the category is created/listed even if no EBOOT children are found
                categories[name];  // creates empty vector if not present

                std::string catDir = base + name;
                forEachEntry(catDir, [&](const SceIoDirent &sub){
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
                                gi.sizeBytes = folderBytes;    // <--- NEW

                                std::string t; if (getFolderTitle(gi.path, t)) gi.title = t;
                                categories[name].push_back(gi);

                            }
                        }
                    }
                });
            } else {
                std::string folderNoSlash = joinDirFile(base, name.c_str());
                if (dirExists(folderNoSlash)){
                    if (!findEbootCaseInsensitive(folderNoSlash).empty()){
                        GameItem gi; gi.kind = GameItem::EBOOT_FOLDER;
                        gi.label = name;
                        gi.path  = folderNoSlash;
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
                        uncategorized.push_back(gi);  // <--- FIX: do NOT create a category
                    }
                }
            }
        });
    }

    void scanDevice(const std::string& dev){
        resetLists();

        const char* isoRoots[]  = {"ISO/","ISO/PSP/"};
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/"};

        for (size_t i=0;i<sizeof(isoRoots)/sizeof(isoRoots[0]);++i)  scanIsoRootDir(dev + std::string(isoRoots[i]));
        for (size_t i=0;i<sizeof(gameRoots)/sizeof(gameRoots[0]);++i) scanGameRootDir(dev + std::string(gameRoots[i]));

        if (!categories.empty()) hasCategories = true;

        if (!hasCategories){
            flatAll = uncategorized;
        } else {
            for (auto &kv : categories) categoryNames.push_back(kv.first);
            std::sort(categoryNames.begin(), categoryNames.end(),
                      [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
            if (!uncategorized.empty()) categories["Uncategorized"]; // flag presence
        }
    }

    void clearUI(){
        rowFreeBytes.clear();
        rowReason.clear();      // <--- add
        rowNeedBytes.clear();   // <--- add
        entries.clear(); entryPaths.clear(); entryKinds.clear();
        rowFlags.clear(); rowFreeBytes.clear();
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
            int maxScroll = (int)entries.size() - MAX_DISPLAY;
            if (maxScroll < 0) maxScroll = 0;
            if (oldScroll > maxScroll) oldScroll = maxScroll;
            if (oldScroll < 0) oldScroll = 0;
            scrollOffset = oldScroll;
        }
        showRoots = false;
        freeSelectionIcon();
    }

    void buildRootRows(){
        clearUI();
        int preselect = -1;
        const uint64_t HEADROOM = (4ull << 20); // keep ~4 MiB headroom

        // Add a synthetic "USB Mode" row only when NOT picking a device (Move/Copy)
        if (opPhase != OP_SelectDevice) {
            SceIoDirent ue{}; strncpy(ue.d_name, "__USB_MODE__", sizeof(ue.d_name) - 1);
            ue.d_stat.st_mode = FIO_S_IFDIR;
            entries.push_back(ue);
            entryPaths.emplace_back("");
            entryKinds.push_back(GameItem::ISO_FILE);
            rowFlags.push_back(0);
            rowFreeBytes.push_back(0);
            rowReason.push_back(RD_NONE);
            rowNeedBytes.push_back(0);
        }

        for (auto &r : roots){
            SceIoDirent e{}; strncpy(e.d_name, r.c_str(), sizeof(e.d_name)-1);
            e.d_stat.st_mode = FIO_S_IFDIR;
            entries.push_back(e);
            entryPaths.emplace_back("");
            entryKinds.push_back(GameItem::ISO_FILE);

            uint8_t flags = 0;
            RowDisableReason reason = RD_NONE;
            uint64_t needB = 0, freeB = 0;

            needB = 0;
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
            if (!sameDeviceMoveRow &&
                opPhase == OP_SelectDevice &&
                (actionMode == AM_Move || actionMode == AM_Copy)) {
                needB = bytesNeededForOp(opSrcPaths, opSrcKinds, r, /*isCopy=*/(actionMode == AM_Copy));
                if (getFreeBytesCMF(r.c_str(), freeB)) {
                    if (needB > 0 && freeB + HEADROOM < needB) {
                        flags |= ROW_DISABLED;
                        reason = RD_NO_SPACE;
                    }
                } else {
                    freeB = 0; // unknown; UI will say "probing"
                }
            }

            rowFlags.push_back(flags);
            rowFreeBytes.push_back(freeB);
            rowReason.push_back(reason);
            rowNeedBytes.push_back(needB);

            if (r == currentDevice) preselect = (int)entries.size() - 1;
        }

        showRoots = true; moving = false;

        selectedIndex = 0; scrollOffset = 0;
        if (preselect >= 0 && (rowFlags[preselect] & ROW_DISABLED) == 0) {
            selectedIndex = preselect;
        } else {
            for (int i = 0; i < (int)entries.size(); ++i)
                if ((rowFlags[i] & ROW_DISABLED) == 0) { selectedIndex = i; break; }
        }
    }


    void buildRootRowsForDevicePicker() {
        // Keep this a thin alias so it always uses the full Move-mode logic.
        opPhase = OP_SelectDevice;  // ensure buildRootRows() sees we’re in device-pick phase
        buildRootRows();            // this computes need/free, sets reasons, disables rows, and prints yellow text
    }

    void buildCategoryRows(){
        clearUI(); moving = false;
        std::vector<std::string> catsSorted = categoryNames;
        bool hasUnc = (categories.find("Uncategorized") != categories.end());
        if (hasUnc) catsSorted.push_back("Uncategorized");

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

        selectedIndex = (preselect >= 0) ? preselect : 0;
        scrollOffset  = (selectedIndex >= MAX_DISPLAY) ? (selectedIndex - MAX_DISPLAY + 1) : 0;
    }

    static std::string parseCategoryFromFullPath(const std::string& full, GameItem::Kind kind) {
        std::string sub = subrootFor(full, kind);
        std::string tail = afterSubroot(full, sub);
        std::string cat, leaf;
        parseCategoryFromPath(tail, cat, leaf);
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
        std::sort(catsSorted.begin(), catsSorted.end(),
                [](const std::string& a, const std::string& b){
                    return strcasecmp(a.c_str(), b.c_str()) < 0;
                });

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
        view = View_Categories;

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

        // Decide based on cache dirtiness AND presence of a real snapshot
        std::string key = rootPrefix(currentDevice);
        auto &dc = deviceCache[key];                    // creates entry if missing

        // A snapshot is "present" only if it actually holds content/metadata we can render.
        const bool hasSnap =
            (!dc.snap.flatAll.empty()) ||
            (!dc.snap.uncategorized.empty()) ||
            (!dc.snap.categories.empty()) ||
            (!dc.snap.categoryNames.empty()) ||
            (dc.snap.hasCategories); // flag alone isn’t enough, but counts as a hint

        const bool needsScan = dc.dirty || !hasSnap;   // first time OR explicitly dirtied

        if (needsScan) {
            msgBox = new MessageBox("Populating...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
            renderOneFrame();

            scanDevice(currentDevice);                  // real, slow scan
            snapshotCurrentScan(dc.snap);               // cache the fresh results
            dc.dirty = false;

            moving = false;
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

        if (hasCategories) {
            buildCategoryRows();
        } else {
            workingList = flatAll;
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

        msgBox = new MessageBox("Saving...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
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


        drawMessage("Order saved", COLOR_GREEN);
        sceKernelDelayThread(700 * 1000);
    }



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


    // -----------------------------------------------------------
    // New: Move operation helpers
    // -----------------------------------------------------------
    // (rootPrefix removed — we already have this earlier in the class)
    static bool sameDevice(const std::string& a, const std::string& b) {
        return strncasecmp(a.c_str(), b.c_str(), 5) == 0;
    }
    static bool ensureDir(const std::string& path) {
        // create single level
        if (dirExists(path)) return true;
        return sceIoMkdir(path.c_str(), 0777) >= 0;
    }
    static bool ensureDirRecursive(const std::string& full) {
        // make every segment after "ms0:/" or "ef0:/"
        if (full.size() < 5) return false;
        size_t i = 5; // skip "ms0:/"
        while (i < full.size()) {
            size_t j = full.find('/', i);
            if (j == std::string::npos) j = full.size();
            std::string sub = full.substr(0, j);
            if (!dirExists(sub)) {
                if (sceIoMkdir(sub.c_str(), 0777) < 0) return false;
            }
            i = j + 1;
        }
        return true;
    }
    static bool isDirectoryPath(const std::string& path) {
        SceIoStat st{}; if (sceIoGetstat(path.c_str(), &st) < 0) return false;
        return FIO_S_ISDIR(st.st_mode);
    }
    // Replace your copyFile with this hardened version.
    // NOTE: signature unchanged from your current integration that passes `this`.
    static bool copyFile(const std::string& src, const std::string& dst, KernelFileExplorer* self) {
        logf("copyFile: %s -> %s", src.c_str(), dst.c_str());

        SceUID in = sceIoOpen(src.c_str(), PSP_O_RDONLY, 0);
        if (in < 0) { logf("  open src failed %d", in); return false; }

        // Ensure parent
        std::string parent = parentOf(dst);
        if (!ensureDirRecursive(parent)) {
            logf("  ensureDirRecursive(%s) failed", parent.c_str());
            sceIoClose(in);
            return false;
        }

        SceUID out = sceIoOpen(dst.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
        if (out < 0) { logf("  open dst failed %d", out); sceIoClose(in); return false; }

        uint64_t fileSize = 0;
        { SceIoStat st{}; if (sceIoGetstat(src.c_str(), &st) >= 0) fileSize = (uint64_t)st.st_size; if (!fileSize) fileSize = 1; }

        if (self && self->msgBox) { self->msgBox->showProgress(basenameOf(src).c_str(), 0, fileSize); self->renderOneFrame(); }

        const int READ_BUF = 512 * 1024;
        int maxWriteChunk  = 64  * 1024;   // start at 64 KiB, we may shrink on trouble
        const int MIN_WRITE_CHUNK = 4 * 1024;

        std::vector<uint8_t> buf(READ_BUF);
        bool ok = true; uint64_t total = 0; int lastErr = 0;

        auto destDev = std::string(dst.substr(0, 4)); // "ms0:" / "ef0:" (dst is "ef0:/...")
        for (;;) {
            int r = sceIoRead(in, buf.data(), READ_BUF);
            if (r < 0) { lastErr = r; logf("  read err %d", r); ok = false; break; }
            if (r == 0) break;

            int off = 0;
            while (off < r) {
                int chunk = r - off;
                if (chunk > maxWriteChunk) chunk = maxWriteChunk;

                int w = sceIoWrite(out, buf.data() + off, chunk);
                if (w <= 0) {
                    // If 0 or negative, try shrinking the chunk a few times before giving up
                    int attemptChunk = chunk;
                    for (int tries = 0; tries < 4 && w <= 0 && attemptChunk > MIN_WRITE_CHUNK; ++tries) {
                        attemptChunk >>= 1; // half it
                        sceKernelDelayThread(500);
                        w = sceIoWrite(out, buf.data() + off, attemptChunk);
                        if (w > 0) {
                            maxWriteChunk = attemptChunk;
                            break;
                        }
                    }

                    if (w <= 0) {
                        uint64_t freeBytes = 0;
                        if (getFreeBytesCMF(destDev.c_str(), freeBytes)) {
                            logf("  write err %d (chunk=%d) with free=%llu bytes on %s",
                                w, attemptChunk, (unsigned long long)freeBytes, canonicalDev(destDev.c_str()));
                        } else {
                            logf("  write err %d (chunk=%d); CMF free space query failed", w, attemptChunk);
                        }
                        lastErr = w;
                        ok = false;
                        break;
                    }
                }

                off   += w;
                total += (uint64_t)w;

                if (self && self->msgBox) { self->msgBox->updateProgress(total, fileSize); self->renderOneFrame(); }
            }

            if (!ok) break;
            sceKernelDelayThread(0);
        }

        sceIoClose(in);
        sceIoClose(out);

        if (!ok) {
            logf("copyFile: FAIL after %llu/%llu bytes (err=%d)",
                (unsigned long long)total, (unsigned long long)fileSize, lastErr);
            sceIoRemove(dst.c_str()); // remove partial
            if (self && self->msgBox) { self->msgBox->updateProgress(total, fileSize); self->renderOneFrame(); }
            return false;
        }

        if (self && self->msgBox) { self->msgBox->updateProgress(fileSize, fileSize); self->renderOneFrame(); }
        logf("copyFile: OK %llu bytes", (unsigned long long)total);
        return true;
    }


    static bool removeDirRecursive(const std::string& dir) {
        SceUID d = pspIoOpenDir(dir.c_str());
        if (d < 0) { logf("removeDirRecursive: open %s failed %d", dir.c_str(), d); return false; }
        SceIoDirent ent; memset(&ent, 0, sizeof(ent));
        while (pspIoReadDir(d, &ent) > 0) {
            if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
            std::string child = joinDirFile(dir, ent.d_name);
            if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
                removeDirRecursive(child);
                sceIoRmdir(child.c_str());
            } else {
                sceIoRemove(child.c_str());
            }
            memset(&ent, 0, sizeof(ent));
            sceKernelDelayThread(0); // yield
        }
        pspIoCloseDir(d);
        return sceIoRmdir(dir.c_str()) >= 0;
    }
    static bool copyDirRecursive(const std::string& src, const std::string& dst, KernelFileExplorer* self) {
        logf("copyDirRecursive: %s -> %s", src.c_str(), dst.c_str());
        if (!ensureDirRecursive(dst)) { logf("  ensureDirRecursive failed"); return false; }
        SceUID d = pspIoOpenDir(src.c_str());
        if (d < 0) { logf("  open src failed %d", d); return false; }

        SceIoDirent ent; memset(&ent, 0, sizeof(ent));
        bool ok = true;
        while (ok && pspIoReadDir(d, &ent) > 0) {
            if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
            std::string s = joinDirFile(src, ent.d_name);
            std::string t = joinDirFile(dst, ent.d_name);

            if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
                ok = ensureDir(t) && copyDirRecursive(s, t, self);
            } else {
                logf("  file: %s -> %s (%ld bytes)", s.c_str(), t.c_str(), (long)ent.d_stat.st_size);
                ok = copyFile(s, t, self);
            }
            memset(&ent, 0, sizeof(ent));
            sceKernelDelayThread(0); // yield
        }
        pspIoCloseDir(d);
        logf("copyDirRecursive: %s", ok ? "OK" : "FAIL");
        return ok;
    }

    // Determine subroot for a given item path (preserve source tree)
    static std::string subrootFor(const std::string& path, GameItem::Kind kind) {
        // EBOOT trees to check
        const char* gameSubs[] = {"PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/","PSP/GAME/"};
        // ISO trees to check
        const char* isoSubs[]  = {"ISO/PSP/","ISO/"};
        size_t pos = 0;
        if (path.size() >= 5) pos = 5; // after "ms0:/" or "ef0:/"
        if (kind == GameItem::EBOOT_FOLDER) {
            for (auto s : gameSubs) {
                if (path.size() >= pos + strlen(s) && strncasecmp(path.c_str()+pos, s, strlen(s))==0)
                    return std::string(s);
            }
            return "PSP/GAME/"; // default fallback
        } else {
            for (auto s : isoSubs) {
                if (path.size() >= pos + strlen(s) && strncasecmp(path.c_str()+pos, s, strlen(s))==0)
                    return std::string(s);
            }
            return "ISO/"; // default fallback
        }
    }

    static bool parseCategoryFromPath(const std::string& pathAfterSubroot, std::string& outCat, std::string& outLeaf) {
        // pathAfterSubroot = e.g. "CAT_02PS1/SLUS12345"  or "SLUS12345"  or "CAT_01PSP/Game.iso"
        size_t slash = pathAfterSubroot.find('/');
        if (slash == std::string::npos) {
            // no category
            outCat.clear();
            outLeaf = pathAfterSubroot;
            return false;
        }
        std::string first = pathAfterSubroot.substr(0, slash);
        if (startsWithCAT(first.c_str())) {
            outCat  = first;
            outLeaf = pathAfterSubroot.substr(slash+1);
            return true;
        } else {
            outCat.clear();
            outLeaf = pathAfterSubroot.substr(0); // leaf starts immediately
            return false;
        }
    }

    static std::string afterSubroot(const std::string& full, const std::string& subroot) {
        // full: "ms0:/PSP/GAME/CAT_foo/Leaf", subroot: "PSP/GAME/"
        if (full.size() < 5) return "";
        size_t pos = 5;
        if (full.size() >= pos + subroot.size() && strncasecmp(full.c_str()+pos, subroot.c_str(), subroot.size()) == 0) {
            return full.substr(pos + subroot.size());
        }
        // if not matching (weird), return whatever is after root
        return full.substr(5);
    }

    // Build destination path given a source and a destination device/category
    static std::string buildDestPath(const std::string& srcPath,
                                     GameItem::Kind kind,
                                     const std::string& destDevice,
                                     const std::string& destCategory /* "" for Uncategorized */) {
        std::string sub = subrootFor(srcPath, kind);
        std::string tail = afterSubroot(srcPath, sub);
        std::string srcCat, leaf;
        parseCategoryFromPath(tail, srcCat, leaf); // leaf is basename for folder/file
        std::string dest = destDevice + sub;
        if (!destCategory.empty() && strcasecmp(destCategory.c_str(),"Uncategorized")!=0) {
            dest = joinDirFile(dest, destCategory.c_str());
        }
        return joinDirFile(dest, leaf.c_str());
    }

    static int kfeFastMoveDevctl(const char* src, const char* dst) {
        // Must be the same device: e.g. "ms0:/..." → both match up to ':'
        const char* c1 = strchr(src,  ':');
        const char* c2 = strchr(dst,  ':');
        if (!c1 || !c2) return -1;
        if ((c1 - src) != (c2 - dst)) return -1;
        for (const char* a = src, *b = dst; a <= c1; ++a, ++b) {
            if (*a != *b) return -1;
        }

        // Build device string "ms0:" or "ef0:"
        char dev[8]; size_t n = 0;
        do {
            if (n + 1 >= sizeof(dev)) return -1;
            dev[n] = src[n];
        } while (src[n++] != ':');
        dev[n] = '\0';

        // Devctl expects two pointers to the path parts AFTER the colon
        uint32_t data[2];
        data[0] = (uint32_t)(c1 + 1);
        data[1] = (uint32_t)(c2 + 1);

        // 0x02415830 = FAT intra-volume move/rename (instant)
        return pspIoDevctl(dev, 0x02415830, data, sizeof(data), nullptr, 0);
    }

    // Execute move for one path
    static bool moveOne(const std::string& src, const std::string& dst, GameItem::Kind kind, KernelFileExplorer* self) {
        logf("moveOne: src=%s", src.c_str());
        logf("        dst=%s", dst.c_str());
        logf("        kind=%s", (kind==GameItem::ISO_FILE)?"ISO":"EBOOT");

        if (!strcasecmp(src.c_str(), dst.c_str())) {
            logf("  src == dst; skip");
            return true;
        }

        // Make sure the destination parent exists
        std::string dstParent = parentOf(dst);
        if (!ensureDirRecursive(dstParent)) { logf("  ensureDirRecursive(parent) FAILED"); return false; }

        // Replace policy: if a destination exists and we're replacing, clear it first
        SceIoStat dstSt{};
        if (pathExists(dst, &dstSt) && REPLACE_ON_MOVE) {
            if (isDirMode(dstSt)) { logf("  dst exists (dir) -> removing"); removeDirRecursive(dst); }
            else                  { logf("  dst exists (file)-> removing"); sceIoRemove(dst.c_str()); }
        }

        // Same device? Prefer instant operations.
        if (sameDevice(src, dst)) {
            int rc = kfeFastMoveDevctl(src.c_str(), dst.c_str());
            if (rc >= 0) return true;

            if (kind == GameItem::ISO_FILE) {
                int rr = sceIoRename(src.c_str(), dst.c_str());
                if (rr >= 0) return true;
                // Fallback: same-device copy+delete -> show progress
                bool ok = copyFile(src, dst, self);
                if (ok) sceIoRemove(src.c_str());
                return ok;
            } else {
                int rr = sceIoRename(src.c_str(), dst.c_str());
                if (rr >= 0) return true;

                if (fastMoveDirByRenames(src, dst)) { sceIoRmdir(src.c_str()); return true; }

                // Last resort: show progress per file while copying the tree
                bool ok = copyDirRecursive(src, dst, self) && removeDirRecursive(src);
                return ok;
            }
        }

        // Cross-device: always copy+delete, with progress
        if (kind == GameItem::ISO_FILE) {
            bool ok = copyFile(src, dst, self);
            if (ok) sceIoRemove(src.c_str());
            return ok;
        } else {
            bool ok = copyDirRecursive(src, dst, self) && removeDirRecursive(src);
            return ok;
        }
    }

    // --- Delete helpers: show filename + a simple per-item progress ---

    // Per-file delete with progress label
    static bool removeFileWithProgress(const std::string& path, KernelFileExplorer* self) {
        if (self && self->msgBox) {
            self->msgBox->showProgress(basenameOf(path).c_str(), 0, 1);
            self->renderOneFrame();
        }
        int rc = sceIoRemove(path.c_str());
        if (self && self->msgBox) {
            self->msgBox->updateProgress(1, 1);
            self->renderOneFrame();
        }
        return rc >= 0;
    }

    // Recursive folder delete that shows the current child name as we go
    static bool removeDirRecursiveProgress(const std::string& dir, KernelFileExplorer* self) {
        SceUID d = pspIoOpenDir(dir.c_str());
        if (d < 0) {
            // If open fails, try removing the directory itself (may already be empty/inaccessible)
            if (self && self->msgBox) {
                self->msgBox->showProgress(basenameOf(dir).c_str(), 0, 1);
                self->renderOneFrame();
            }
            bool ok = (sceIoRmdir(dir.c_str()) >= 0);
            if (self && self->msgBox) {
                self->msgBox->updateProgress(1, 1);
                self->renderOneFrame();
            }
            return ok;
        }

        bool ok = true;
        SceIoDirent ent; memset(&ent, 0, sizeof(ent));
        while (ok && pspIoReadDir(d, &ent) > 0) {
            if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) {
                memset(&ent, 0, sizeof(ent));
                continue;
            }
            std::string child = joinDirFile(dir, ent.d_name);

            // Update label to current child before removing
            if (self && self->msgBox) {
                self->msgBox->showProgress(ent.d_name, 0, 1);
                self->renderOneFrame();
            }

            if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
                // Recurse into subdir first
                ok = removeDirRecursiveProgress(child, self);
                if (ok) ok = (sceIoRmdir(child.c_str()) >= 0);
            } else {
                ok = (sceIoRemove(child.c_str()) >= 0);
            }

            // Mark this item finished
            if (self && self->msgBox) {
                self->msgBox->updateProgress(1, 1);
                self->renderOneFrame();
            }

            memset(&ent, 0, sizeof(ent));
            sceKernelDelayThread(0); // yield
        }
        pspIoCloseDir(d);

        // Finally remove the now-empty parent directory itself (show its name as a cue)
        if (ok) {
            if (self && self->msgBox) {
                self->msgBox->showProgress(basenameOf(dir).c_str(), 0, 1);
                self->renderOneFrame();
            }
            ok = (sceIoRmdir(dir.c_str()) >= 0);
            if (self && self->msgBox) {
                self->msgBox->updateProgress(1, 1);
                self->renderOneFrame();
            }
        }

        return ok;
    }

    // Delete one path (file or folder) with UI updates
    static bool deleteOne(const std::string& path, GameItem::Kind kind, KernelFileExplorer* self) {
        if (kind == GameItem::ISO_FILE) {
            return removeFileWithProgress(path, self);
        } else {
            // EBOOT folder or any directory-like entry
            return removeDirRecursiveProgress(path, self);
        }
    }

    // Full delete executor: mirrors performMove()/performCopy() style and refresh rules
    void performDelete() {
        ClockGuard cg; cg.boost333();

        // Open progress box (no icon, centered bar + two text lines)
        msgBox = new MessageBox("Deleting...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
        renderOneFrame();

        int ok = 0, fail = 0;
        for (size_t i = 0; i < opSrcPaths.size(); ++i) {
            const std::string& p = opSrcPaths[i];
            const GameItem::Kind k = opSrcKinds[i];

            // NEW: show the cached game title (headline above the progress bar).
            // Falls back to basename when no title is cached.
            if (msgBox) {
                std::string title = getCachedTitleForPath(p);
                if (title.empty()) title = basenameOf(p);
                msgBox->setProgressTitle(title.c_str());
                renderOneFrame();
            }

            // Filename detail is already handled inside deleteOne(...) via showProgress/updateProgress.
            bool okOne = deleteOne(p, k, this);
            if (okOne) {
                ok++;
                checked.erase(p);
            } else {
                fail++;
            }
            sceKernelDelayThread(0);
        }

        delete msgBox; msgBox = nullptr;

        // Mutate the cache and stay right where we are — instantly.
        // 1) Ensure cache entry exists (scan once if truly missing)
        auto &dc = deviceCache[rootPrefix(currentDevice)];
        if (dc.snap.flatAll.empty() && dc.snap.uncategorized.empty()
            && dc.snap.categories.empty() && dc.snap.categoryNames.empty()) {
            scanDevice(currentDevice);              // one-time build
            snapshotCurrentScan(dc.snap);
        }
        // 2) Remove each source path from the snapshot
        for (const auto& p : opSrcPaths) {
            snapErasePath(dc.snap, p);
        }
        dc.dirty = false;                           // snapshot is authoritative

        // 3) Repaint from snapshot without any scan
        restoreScan(dc.snap);
        if (hasCategories) {
            if (view == View_CategoryContents) openCategory(currentCategory);
            else buildCategoryRows();
        } else {
            rebuildFlatFromCache();
        }


        // Feedback toast, like Move/Copy
        char res[64];
        if (fail == 0) {
            snprintf(res, sizeof(res), "Deleted %d item(s)", ok);
            drawMessage(res, COLOR_GREEN);
        } else if (ok == 0) {
            snprintf(res, sizeof(res), "Delete failed (%d)", fail);
            drawMessage(res, COLOR_RED);
        } else {
            snprintf(res, sizeof(res), "Deleted %d, failed %d", ok, fail);
            drawMessage(res, COLOR_YELLOW);
        }
        sceKernelDelayThread(800 * 1000);

        // Clear sentinel/op state used to piggyback confirm close
        opDestDevice.clear();
        opSrcPaths.clear(); opSrcKinds.clear();
        opPhase = OP_None;
    }



    static bool copyOne(const std::string& src, const std::string& dst, GameItem::Kind kind, KernelFileExplorer* self) {
        logf("copyOne: %s -> %s (%s)", src.c_str(), dst.c_str(), (kind==GameItem::ISO_FILE)?"ISO":"EBOOT");
        std::string dstParent = parentOf(dst);
        if (!ensureDirRecursive(dstParent)) return false;

        SceIoStat dstSt{};
        if (pathExists(dst, &dstSt) && REPLACE_ON_MOVE) {
            if (isDirMode(dstSt)) removeDirRecursive(dst);
            else sceIoRemove(dst.c_str());
        }

        if (kind == GameItem::ISO_FILE) {
            return copyFile(src, dst, self);
        } else {
            return copyDirRecursive(src, dst, self);
        }
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

        int okCount = 0, failCount = 0;
        for (size_t i = 0; i < opSrcPaths.size(); ++i) {
            const std::string& src = opSrcPaths[i];
            const GameItem::Kind k = opSrcKinds[i];
            std::string dst = buildDestPath(src, k, opDestDevice.empty() ? currentDevice : opDestDevice, opDestCategory);

            // Show the game title as the progress headline
            if (msgBox) {
                std::string title = getCachedTitleForPath(src);
                if (title.empty()) title = basenameOf(src);
                msgBox->setProgressTitle(title.c_str());
                renderOneFrame();
            }

            // Filename detail is already shown by copyFile() via showProgress/updateProgress
            bool ok = copyOne(src, dst, k, this);
            if (ok) okCount++; else failCount++;
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

        // Insert each copied item into destination snapshot
        for (size_t i = 0; i < opSrcPaths.size(); ++i) {
            const std::string& s = opSrcPaths[i];
            const GameItem::Kind k = opSrcKinds[i];
            const std::string d = buildDestPath(s, k, opDestDevice.empty() ? currentDevice : opDestDevice, opDestCategory);
            cacheApplyMoveOrCopy(dstEntry.snap, dstEntry.snap, s, d, k, /*isMove*/false);
        }
        dstEntry.dirty = false;

        // Jump to destination view immediately (match Move behavior)
        showDestinationCategoryNow(dstDev, opDestCategory);

        // optional: focus the last copied item
        if (!opSrcPaths.empty()) {
            const std::string lastDst = buildDestPath(opSrcPaths.back(), opSrcKinds.back(),
                opDestDevice.empty() ? currentDevice : opDestDevice, opDestCategory);
            selectByPath(lastDst);
        }

        // clear op state
        actionMode = AM_None;
        opPhase    = OP_None;
        opSrcPaths.clear(); opSrcKinds.clear();
        opDestDevice.clear(); opDestCategory.clear();

        char res[64];
        if (failCount == 0) { snprintf(res, sizeof(res), "Copied %d item(s)", okCount); drawMessage(res, COLOR_GREEN); }
        else if (okCount == 0) { snprintf(res, sizeof(res), "Copy failed (%d)", failCount); drawMessage(res, COLOR_RED); }
        else { snprintf(res, sizeof(res), "Copied %d, failed %d", okCount, failCount); drawMessage(res, COLOR_YELLOW); }
        sceKernelDelayThread(800*1000);
    }



    // --- helper (NEW): quick probe for any CAT_ on a device ---
    bool deviceHasAnyCategory(const std::string& dev) const {
        const char* isoRoots[]  = {"ISO/","ISO/PSP/"};
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
        actionMode = mode;
        opPhase    = OP_None;
        opSrcPaths.clear();
        opSrcKinds.clear();
        opDestDevice.clear();
        opDestCategory.clear();

        if (showRoots || !(view == View_AllFlat || view == View_CategoryContents)) {
            msgBox = new MessageBox("Open a file list to use file operations.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
            actionMode = AM_None;
            return;
        }

        // Snapshot selection
        if (!checked.empty()) {
            for (auto &p : checked) {
                opSrcPaths.push_back(p);
                GameItem::Kind k = GameItem::ISO_FILE;
                for (auto &gi : workingList) if (gi.path == p) { k = gi.kind; break; }
                opSrcKinds.push_back(k);
            }
        } else {
            if (selectedIndex < 0 || selectedIndex >= (int)workingList.size()) {
                msgBox = new MessageBox("No item selected.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                actionMode = AM_None;
                return;
            }
            opSrcPaths.push_back(workingList[selectedIndex].path);
            opSrcKinds.push_back(workingList[selectedIndex].kind);
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

        if (mode == AM_Move || mode == AM_Copy) {
            if (goMs0Mode) {
                opPhase = OP_SelectDevice;

                // NEW: transient feedback while we do the first free-space probe
                msgBox = new MessageBox("Calculating free space...", nullptr,
                                        SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "",
                                        16, 18, 8, 14);
                renderOneFrame();

                buildRootRows();  // (will probe free space as needed)

                // close the transient overlay
                delete msgBox; msgBox = nullptr;
            } else {
                if (!hasCategories) {
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
        opDestDevice.clear();
        opDestCategory.clear();

        // Restore UI state
        scanDevicePreferCache(preOpDevice);
        if (hasCategories) {
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
        char buf[128];
        const char* devName = rootDisplayName(opDestDevice.c_str());
        const char* catName = (opDestCategory.empty() || !strcasecmp(opDestCategory.c_str(),"Uncategorized")) ? "Uncategorized" : opDestCategory.c_str();
        const char* verb    = (actionMode == AM_Copy) ? "Copy" : "Move";
        snprintf(buf, sizeof(buf), "%s %d item(s) to %s — %s\nPress X to confirm.",
                verb, (int)opSrcPaths.size(), devName, catName);
        msgBox = new MessageBox(buf, okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
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

        int okCount = 0, failCount = 0;
        for (size_t i = 0; i < opSrcPaths.size(); ++i) {
            const std::string& src = opSrcPaths[i];
            const GameItem::Kind k = opSrcKinds[i];
            std::string dst = buildDestPath(src, k, opDestDevice.empty() ? currentDevice : opDestDevice, opDestCategory);

            // Game title as headline
            if (msgBox) {
                std::string title = getCachedTitleForPath(src);
                if (title.empty()) title = basenameOf(src);
                msgBox->setProgressTitle(title.c_str());
                renderOneFrame();
            }

            if (!sameDevice(src, dst)) didCross = true;

            // Filename detail appears from the underlying copy/move implementation
            bool ok = moveOne(src, dst, k, this);
            if (ok) { okCount++; checked.erase(src); }
            else    { failCount++; }
            sceKernelDelayThread(0);
        }


        delete msgBox; msgBox = nullptr;
        logf("=== performMove: done ok=%d fail=%d ===", okCount, failCount);
        logClose();

        // didCross already computed inside the loop

        // --- Patch caches and jump instantly to destination ---

        const std::string srcDev = preOpDevice;                                  // e.g., "ms0:/"
        const std::string dstDev = opDestDevice.empty() ? srcDev : opDestDevice; // same-device if empty

        // 1) Ensure we have cache entries (created if missing)
        auto &srcEntry = deviceCache[rootPrefix(srcDev)];
        auto &dstEntry = deviceCache[rootPrefix(dstDev)];

        // 2) If we have no snapshots yet (first time ever), build them once and store.
        //    Otherwise we will patch them below.
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

        // 3) Patch both snapshots with the results (remove from src; add/rename into dst)
        for (size_t i = 0; i < opSrcPaths.size(); ++i) {
            const std::string& src = opSrcPaths[i];
            const GameItem::Kind k = opSrcKinds[i];
            const std::string   dst = buildDestPath(src, k,
                                    opDestDevice.empty() ? currentDevice : opDestDevice, opDestCategory);
            cacheApplyMoveOrCopy(srcEntry.snap, dstEntry.snap, src, dst, k, /*isMove*/true);
        }
        // Keep them valid for instant reuse
        srcEntry.dirty = false;
        dstEntry.dirty = false;

        // Select the destination category and repaint full contents
        showDestinationCategoryNow(dstDev, opDestCategory);

        // Focus the last moved item so it’s highlighted
        if (!opSrcPaths.empty() && okCount > 0) {
            const std::string lastDst = buildDestPath(
                opSrcPaths.back(),
                opSrcKinds.back(),
                opDestDevice.empty() ? currentDevice : opDestDevice,
                opDestCategory
            );
            selectByPath(lastDst);
        }

        // Toast
        char res[64];
        if (failCount == 0)      snprintf(res, sizeof(res), "Moved %d item(s)", okCount);
        else if (okCount == 0)   snprintf(res, sizeof(res), "Move failed (%d)", failCount);
        else                     snprintf(res, sizeof(res), "Moved %d, failed %d", okCount, failCount);
        drawMessage(res, (failCount ? (okCount ? COLOR_YELLOW : COLOR_RED) : COLOR_GREEN));
        sceKernelDelayThread(800 * 1000);

    }

    // -----------------------------------------------------------
    // Input handling
    // -----------------------------------------------------------
public:
    KernelFileExplorer(){ detectRoots(); buildRootRows(); }
    ~KernelFileExplorer(){
        if (font) intraFontUnload(font);
        freeSelectionIcon();
        if (placeholderIconTexture) { texFree(placeholderIconTexture); placeholderIconTexture = nullptr; }
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
        font = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_MED);
        if (!font) pspDebugScreenInit();

        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

        // Prepare USB bus + mass-storage once at boot
        sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
        sceUsbStart(PSP_USBSTOR_DRIVERNAME, 0, 0);


        // Prepare USB drivers (faster activate later)
        UsbStartStacked();

    }


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
                // Rebuild the message to reflect state
                if (gUsbBox) { delete gUsbBox; gUsbBox = nullptr; }
                gUsbBox = new MessageBox(
                    connected ? "Connected to PC...\nPress \xE2\x97\xAF to disconnect."
                            : "Connect to PC...\nPress \xE2\x97\xAF to disconnect.",
                    nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14,
                    PSP_CTRL_CIRCLE);
                gUsbShownConnected = connected;
            }
        }


        
// While USB Mode is active, drive the connection and UI
if (gUsbActive) {
    int s = sceUsbGetState();
    if ((s & PSP_USB_CABLE_CONNECTED) && !(s & PSP_USB_ACTIVATED)) {
        sceUsbActivate(0x1c8);
    }
    const bool connected = (s & PSP_USB_CONNECTION_ESTABLISHED);
    if (connected != gUsbShownConnected) {
        if (gUsbBox) { delete gUsbBox; gUsbBox = nullptr; }
        gUsbBox = new MessageBox(
            connected ? "Connected to PC...\nPress \xE2\x97\xAF to disconnect."
                      : "Connect to PC...\nPress \xE2\x97\xAF to disconnect.",
            nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
        gUsbShownConnected = connected;
    }
}
bool analogUpNow = (pad.Ly <= 30);
        if (analogUpNow && !analogUpHeld) {
            showDebugTimes = !showDebugTimes;
        }
        analogUpHeld = analogUpNow;

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
                bulkSelect(-1);
                return;  // swallow navigation while painting
            }
            // Square + Down  => Bulk select downward from current row until a checked barrier
            if ((pressed & PSP_CTRL_DOWN) || repeatDown) {
                bulkSelect(+1);
                return;  // swallow navigation while painting
            }
        }

        // ---------------------------
        // While an op is active (Move), restrict navigation per spec
        // ---------------------------
        // ---------------------------
        // While an op is active (Move/Copy), restrict navigation per spec
        // ---------------------------
        if (actionMode != AM_None) {
            // Movement in lists (Up/Down)
            if ((pressed & PSP_CTRL_UP) || repeatUp) {
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
                        if (selectedIndex >= scrollOffset + MAX_DISPLAY) scrollOffset = selectedIndex - MAX_DISPLAY + 1;
                    }
                }
            }

            // Confirm/cancel flow
            if (pressed & PSP_CTRL_CIRCLE) {
                // Show immediate exit overlay, restore view, then close overlay.
                const char* actWord = (actionMode == AM_Move) ? "Move" : "Copy";
                char exitingMsg[64];
                snprintf(exitingMsg, sizeof(exitingMsg), "Exiting %s operation...", actWord);

                // Show a passive overlay
                msgBox = new MessageBox(exitingMsg, nullptr,
                                        SCREEN_WIDTH, SCREEN_HEIGHT,
                                        1.0f, 0, "", 16, 18, 8, 14);
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

                    if (hasPreOpScan && strcasecmp(opDestDevice.c_str(), preOpDevice.c_str()) == 0) {
                        // Same device as the one we were already viewing → reuse instantly
                        restoreScan(preOpScan);

                        if (hasCategories) {
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
                            msgBox = new MessageBox("Scanning destination...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
                            renderOneFrame();
                        }
                        scanDevicePreferCache(opDestDevice);
                        if (needScan) { delete msgBox; msgBox = nullptr; }


                        if (hasCategories) {
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

        // --- OPEN (X) while just browsing categories (not in Move/Copy) ---
        if ((pressed & PSP_CTRL_CROSS) &&
            actionMode == AM_None &&
            !showRoots &&
            view == View_Categories &&
            !msgBox && !fileMenu)
        {
            // Freeze background free-space probes so the UI can react instantly
            FreeSpacePauseNow();

            if (selectedIndex >= 0 && selectedIndex < (int)entries.size()) {
                openCategory(entries[selectedIndex].d_name);
            }

            // Category view is ready — let the probe continue
            FreeSpaceResume();
            return;
        }


        // L trigger: rename
        if (pressed & PSP_CTRL_RTRIGGER) {
            beginRenameSelected();
            return;
        }

        // □ toggle checkmark on current item
        if (pressed & PSP_CTRL_SQUARE) {
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents) &&
                selectedIndex >= 0 && selectedIndex < (int)workingList.size()) {
                const std::string& p = workingList[selectedIndex].path;
                auto it = checked.find(p);
                if (it == checked.end()) checked.insert(p);
                else                     checked.erase(it);
            }
            return;
        }

        // START: save
        if (pressed & PSP_CTRL_START) {
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                commitOrderTimestamps();
            }
            return;
        }

        // Toggle label mode (Triangle → previously LTRIGGER; keep as-is)
        if (pressed & PSP_CTRL_LTRIGGER) {
            showTitles = !showTitles;
            if (!showRoots && (view==View_AllFlat || view==View_CategoryContents))
                refillRowsFromWorkingPreserveSel();
            return;
        }

        // SELECT: A→Z
        if (pressed & PSP_CTRL_SELECT) {
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                moving = false;
                sortWorkingListAlpha(showTitles, workingList, selectedIndex, scrollOffset);
                refillRowsFromWorkingPreserveSel();
            }
            return;
        }

        // △: open modal menu (content views → Move/Copy/Delete; categories → New/Delete)
        if (pressed & PSP_CTRL_TRIANGLE) {
            if (!showRoots && !fileMenu) {
                if (view == View_AllFlat || view == View_CategoryContents) {
                    const bool canCrossDevices = dualDeviceAvailableFromMs0();
                    const bool canWithinDevice = hasCategories;
                    const bool canMoveCopy     = canCrossDevices || canWithinDevice;

                    std::vector<FileOpsItem> items = {
                        { "Move",   !canMoveCopy },
                        { "Copy",   !canMoveCopy },
                        { "Delete", false }
                    };
                    menuContext = MC_ContentOps;
                    fileMenu = new FileOpsMenu(items, SCREEN_WIDTH, SCREEN_HEIGHT);
                } else if (view == View_Categories) {
                    std::vector<FileOpsItem> items = {
                        { "New",    false },
                        { "Delete", false }
                    };
                    menuContext = MC_CategoryOps;
                    fileMenu = new FileOpsMenu(items, SCREEN_WIDTH, SCREEN_HEIGHT);
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
                    if (selectedIndex > 0){
                        selectedIndex--;
                        if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
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
                    if (selectedIndex >= scrollOffset + MAX_DISPLAY) scrollOffset = selectedIndex - MAX_DISPLAY + 1;
                    refillRowsFromWorkingPreserveSel();
                } else {
                    if (selectedIndex + 1 < (int)entries.size()){
                        selectedIndex++;
                        if (selectedIndex >= scrollOffset + MAX_DISPLAY) scrollOffset = selectedIndex - MAX_DISPLAY + 1;
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
                    if (selectedIndex + 1 < (int)entries.size()){
                        selectedIndex++;
                        if (selectedIndex >= scrollOffset + MAX_DISPLAY) scrollOffset = selectedIndex - MAX_DISPLAY + 1;
                    }
                }
            }
        }

        // X / O default behaviors
        if (pressed & PSP_CTRL_CROSS) {
            if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;

            if (showRoots) {
                if (selectedIndex < (int)rowFlags.size() && (rowFlags[selectedIndex] & ROW_DISABLED)) return;

                std::string dev = entries[selectedIndex].d_name;
                if (dev == "__USB_MODE__") {
                    if (!gUsbActive) {
                        UsbActivate();
                        gUsbActive = true;
                        gUsbShownConnected = false;
                        gUsbBox = new MessageBox(
                            "Connect to PC...\nPress \xE2\x97\xAF to disconnect.",
                            nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14,
                            PSP_CTRL_CIRCLE);  // CLOSE ON CIRCLE
                    }
                    return; // don’t fall through to openDevice()
                }

                openDevice(dev);
                return;
            }


            if (view == View_Categories) {
                if (FIO_S_ISDIR(entries[selectedIndex].d_stat.st_mode)) openCategory(entries[selectedIndex].d_name);
            } else {
                moving = !moving;
            }
        }
        else if (pressed & PSP_CTRL_CIRCLE) {
            if (showRoots) {
                // nothing
            } else if (view == View_CategoryContents) {
                if (moving) {
                    moving = false;
                } else {
                    checked.clear();
                    buildCategoryRows();
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
                else buildRootRows();
            }
        }
    }

    void run(){
        init();
        while (1) {
            renderOneFrame();

            // Handle active dialogs
            if (msgBox) {
                if (!msgBox->update()) {
                    delete msgBox; msgBox = nullptr; inputWaitRelease = true;

                    // If we just closed a confirmation, perform the chosen op now.
                    if (opPhase == OP_Confirm) {
                        if (opDestDevice == "__DELETE__") {
                            ClockGuard cg; cg.boost333();
                            msgBox = new MessageBox("Deleting...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
                            renderOneFrame();
                            performDelete();
                            continue;
                        }
                        if (opDestDevice == "__DEL_CAT__") {
                            // Delete the CAT_ folder across ISO/GAME roots (all contents),
                            // per spec: non-game files don't affect the confirmation count and will just be removed.
                            ClockGuard cg; cg.boost333();
                            msgBox = new MessageBox("Deleting category...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
                            renderOneFrame();

                            std::string delCat = opDestCategory;
                            deleteCategoryDirs(currentDevice, delCat);

                            // Patch cache & refresh UI without a rescan
                            cachePatchDeleteCategory(delCat);
                            buildCategoryRows();


                            delete msgBox; msgBox = nullptr;
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

            // File ops menu (modal)
            if (fileMenu) {
                if (!fileMenu->update()) {
                    int choice = fileMenu->choice();
                    delete fileMenu; fileMenu = nullptr; inputWaitRelease = true;

                    if (menuContext == MC_ContentOps) {
                        // 0=Move,1=Copy,2=Delete
                        if (choice == 0)      startAction(AM_Move);
                        else if (choice == 1) startAction(AM_Copy);
                        else if (choice == 2) {
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
                                char buf[96];
                                snprintf(buf, sizeof(buf), "Delete %d item(s)?\nPress X to confirm.", (int)delPaths.size());
                                msgBox = new MessageBox(buf, okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                                opSrcPaths = delPaths; opSrcKinds = delKinds;
                                actionMode = AM_None;
                                opPhase    = OP_Confirm;
                                opDestDevice = "__DELETE__";
                            }
                        }
                    } else {
                        // MC_CategoryOps: 0=New, 1=Delete
                        if (choice == 0) {
                            // New category: OSK → create across roots
                            std::string typed;
                            if (!promptTextOSK("New Category", "CAT_", 64, typed)) {
                                // cancelled → nothing
                            } else {
                                if (!startsWithCAT(typed.c_str())) typed = std::string("CAT_") + typed;
                                typed = sanitizeFilename(typed);
                                createCategoryDirs(currentDevice, typed);

                                // Patch cache instead of full rescan
                                cachePatchAddCategory(typed);
                                buildCategoryRows();
                                for (int i=0;i<(int)entries.size();++i)
                                    if (!strcasecmp(entries[i].d_name, typed.c_str())) { selectedIndex = i; break; }
                                drawMessage("Category created", COLOR_GREEN);
                                sceKernelDelayThread(600*1000);
                            }
                        } else if (choice == 1) {
                            // Delete category: count games first and confirm
                            if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) {
                                msgBox = new MessageBox("No category selected.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                            } else {
                                std::string cat = entries[selectedIndex].d_name;
                                if (!startsWithCAT(cat.c_str())) {
                                    msgBox = new MessageBox("Pick a CAT_ folder.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                                } else {
                                    int games = countGamesInCategory(currentDevice, cat);
                                    char buf[128];
                                    if (games > 0) {
                                        snprintf(buf, sizeof(buf),
                                                "%d game(s) are in this folder and will be deleted.\nPress X to confirm.", games);
                                    } else {
                                        snprintf(buf, sizeof(buf),
                                                "Delete empty category?\nPress X to confirm.");
                                    }
                                    msgBox = new MessageBox(buf, okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);

                                    // Reuse the confirm-close hook, sentinel device + stash cat in opDestCategory
                                    actionMode = AM_None;
                                    opPhase    = OP_Confirm;
                                    opDestDevice   = "__DEL_CAT__";
                                    opDestCategory = cat;
                                }
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

// Load & start fs_driver.prx
int LoadStartModule(const char *path) {
    SceUID m = kuKernelLoadModule(path, 0, NULL);
    if (m>=0) { int st; sceKernelStartModule(m,0,NULL,&st,NULL); }
    return m;
}

int main(int argc, char* argv[]) {
    LoadStartModule("fs_driver.prx");
    SetupCallbacks();
    gExecPath = argv[0];

    std::string baseDir  = getBaseDir(argv[0]);
    std::string pngPath  = baseDir + "resources/bkg.png";
    std::string crossPath= baseDir + "resources/cross.png";
    std::string icon0Path= baseDir + "resources/icon0.png";
    std::string boxOffPath = baseDir + "resources/unchecked.png";
    std::string boxOnPath  = baseDir + "resources/checked.png";

    backgroundTexture      = texLoadPNG(pngPath.c_str());
    okIconTexture          = texLoadPNG(crossPath.c_str());
    placeholderIconTexture = texLoadPNG(icon0Path.c_str());
    checkTexUnchecked = texLoadPNG(boxOffPath.c_str());
    checkTexChecked   = texLoadPNG(boxOnPath.c_str());

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