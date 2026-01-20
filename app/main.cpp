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
#include <cstdlib>
#include <stdint.h>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <stdarg.h>
#include <set>

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

static SceUID kfeIoOpenDir(const char* path);
static int kfeIoReadDir(SceUID dir, SceIoDirent* ent);
static int kfeIoCloseDir(SceUID dir);

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
    SceUID d = kfeIoOpenDir(path.c_str());
    if (d >= 0){ kfeIoCloseDir(d); return true; }
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
#define COLOR_BANNER   0xAA000000

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
static Texture* circleIconTexture = nullptr;
static Texture* triangleIconTexture = nullptr;
static Texture* squareIconTexture = nullptr;
static Texture* selectIconTexture = nullptr;
static Texture* startIconTexture = nullptr;
static Texture* placeholderIconTexture = nullptr;
// Checkbox icons (12x12 recommended)
static Texture* checkTexUnchecked = nullptr;
static Texture* checkTexChecked   = nullptr;
// Root menu icons
static Texture* rootMemIcon = nullptr;
static Texture* rootInternalIcon = nullptr;
static Texture* rootUsbIcon = nullptr;
static Texture* rootCategoriesIcon = nullptr;
static Texture* rootArk4Icon = nullptr;
static Texture* rootProMeIcon = nullptr;
static Texture* rootOffBulbIcon = nullptr;
// Categories/menu icons
static Texture* catFolderIcon = nullptr;
static Texture* catFolderIconGray = nullptr;
static Texture* catSettingsIcon = nullptr;
static Texture* blacklistIcon = nullptr;
static Texture* lIconTexture = nullptr;
static Texture* rIconTexture = nullptr;
// Device icons for header (11px tall)
static Texture* memcardSmallIcon = nullptr;
static Texture* internalSmallIcon = nullptr;
static bool gEnablePopAnimations = false; // Toggle Populating animation
static std::vector<std::string> gPopAnimDirs;
static std::vector<size_t> gPopAnimOrder;
static size_t gPopAnimOrderIndex = 0;
static std::string gPopAnimLoadedDir;
static std::vector<MBAnimFrame> gPopAnimFrames;
static unsigned long long gPopAnimMinDelayUs = 0;
static const int POP_ANIM_TARGET_H = 60;
static const char* POP_ANIM_PREF = ""; // Set to a folder name to force a specific animation
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

static SceUID gLogFd = -1;
static inline void trimTrailingSpaces(char* s);
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
static const char* kCatSettingsLabel = "Game Categories settings";

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
            intraFontSetStyle(font, 0.7f, 0xFFBBBBBB, 0, 0.f, INTRAFONT_ALIGN_LEFT);
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
    static std::unordered_map<std::string, bool> gclBlacklistLoadedMap;
    static std::unordered_map<std::string, std::vector<std::string>> gclPendingUnblacklistMap;
    static std::unordered_map<std::string, std::vector<std::string>> gclFilterMap;           // key: filter root ("ms0:/", "ef0:/")
    static std::unordered_map<std::string, bool> gclFilterLoadedMap;
    static inline bool blacklistActive() { return gclCfg.prefix != 0; }

    // ---- Lightweight cache patch: update in-memory categories without rescanning disk ----
    void patchCategoryCacheFromSettings(){
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

            std::string base = stripCategoryPrefixes(key);
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

        // 4) Re-key the categories map (and each GameItem.path), carrying "Uncategorized" through unchanged.
        std::map<std::string, std::vector<GameItem>> newCats;
        std::vector<std::pair<std::string, std::string>> oldToNew; // capture display-name rewrites

        for (auto &kv : categories){
            const std::string& oldCat = kv.first;

            if (!strcasecmp(oldCat.c_str(), "Uncategorized")) {
                newCats.emplace("Uncategorized", std::move(kv.second));
                continue;
            }

            std::string base = stripCategoryPrefixes(oldCat);
            if (isBlacklistedBaseNameFor(currentDevice, base)) continue;
            std::string want = formatCategoryNameFromBase(base, assigned[base]);

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
            std::sort(categoryNames.begin(), categoryNames.end(),
                [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
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
                std::string base = stripCategoryPrefixes(disp);
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
                std::sort(snap.categoryNames.begin(), snap.categoryNames.end(),
                        [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
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
    static std::string stripCategoryPrefixes(const std::string& in){
        std::string s = in;
        if (startsWithCAT(s.c_str())) {
            s.erase(0, 4); // remove "CAT_"
        }
        if (gclCfg.catsort && hasTwoDigitsAfter(s.c_str())) {
            s.erase(0, 2);
        }
        return s;
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
    static bool isBlacklistedCategoryFolder(const std::string& rootLabel, const std::string& sub, const std::string& absRoot){
        // rootLabel is like "ISO/", "PSP/GAME/", etc.
        if (!strcasecmp(rootLabel.c_str(), "ISO/") && !strcasecmp(sub.c_str(), "VIDEO")) return true;
        if (!blacklistActive()) return false;

        std::string base = stripCategoryPrefixes(sub);
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

    // Rename “from”→“to” if it exists and differs (ignores case-only changes)
    static void renameIfExists(const std::string& root, const std::string& from, const std::string& to){
        if (!strcasecmp(from.c_str(), to.c_str())) return;
        std::string a = joinDirFile(root, from.c_str());
        std::string b = joinDirFile(root, to.c_str());
        if (dirExists(a)) sceIoRename(a.c_str(), b.c_str());
    }

    // Enforce naming/numbering for category folders on a device, obeying rules:
    // • Leave folders that already have an XX number alone, UNLESS the number > total categories (then reassign).
    // • Never produce duplicate XX across different categories; if duplicates exist, keep the first by base (A→Z).
    // • Assign numbers to unnumbered bases in alphabetical order, filling the remaining 01..N slots without gaps.
    // • Respect "Use CAT prefix" (gclCfg.prefix) and "Sort Categories" (gclCfg.catsort).
    // • Skip game folders (subdirs that contain an EBOOT.PBP) and ISO/VIDEO.
    // • Skip game folders (subdirs that contain an EBOOT.PBP) and ISO/VIDEO.
    static void enforceCategorySchemeForDevice(const std::string& dev){
        const char* isoRoots[]  = {"ISO/"};                // drop ISO/PSP/ as a root
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME150/"}; // drop PSX/ and Utility/ as roots

        // (absRoot, rootLabel)
        std::vector<std::pair<std::string,std::string>> roots;

        for (auto r : isoRoots)  roots.emplace_back(dev + std::string(r), std::string(r));
        for (auto r : gameRoots) roots.emplace_back(dev + std::string(r), std::string(r));

        // 1) Discover candidate bases (exclude EBOOT folders and blacklisted)
        std::unordered_set<std::string> baseSet;
        // Track existing numbers per base
        std::map<std::string, std::vector<int>> baseExistingNums;

        for (auto &rp : roots){
            const std::string& absRoot  = rp.first;
            const std::string& rootLabel= rp.second;
            std::vector<std::string> subs;
            listSubdirs(absRoot, subs);
            for (auto &sub : subs){
                if (isBlacklistedCategoryFolder(rootLabel, sub, absRoot)) continue;
                // Skip real game folders
                std::string subAbs = joinDirFile(absRoot, sub.c_str());
                if (!findEbootCaseInsensitive(subAbs).empty()) continue;

                std::string base = stripCategoryPrefixes(sub);
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

        // 5) For each root, rename any present variant → desired formatted name
        for (auto &rp : roots){
            const std::string& absRoot = rp.first;
            std::vector<std::string> subs;
            listSubdirs(absRoot, subs);

            for (const auto& base : baseList){
                const int idx = assigned[base];
                const std::string want = formatCategoryNameFromBase(base, idx);

                // Find existing entry for this base on this root (any variant).
                std::string found;
                for (const auto &sub : subs){
                    if (!strcasecmp(stripCategoryPrefixes(sub).c_str(), base.c_str())) { found = sub; break; }
                }
                if (found.empty()) continue; // not present on this root
                if (!strcasecmp(found.c_str(), want.c_str())) continue; // already correct

                // Double-check: skip game folders (paranoia)
                std::string subAbs = joinDirFile(absRoot, found.c_str());
                if (!findEbootCaseInsensitive(subAbs).empty()) continue;

                renameIfExists(absRoot, found, want);
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
    MessageBox* msgBox = nullptr;
    FileOpsMenu* fileMenu = nullptr;
    OptionListMenu* optMenu = nullptr;   // ← NEW: modal option picker
    std::vector<std::string> optMenuOwnedLabels; // keep dynamic labels alive for OptionListMenu
    static bool rootPickGcl;             // ← declaration only; no in-class initializer
    static bool rootKeepGclSelection;    // keep selection on "Game Categories:" after toggle
    bool inputWaitRelease = false;

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

    // Helper: is a visible row non-movable (header/footer)?
    bool isCategoryRowLocked(int row) const {
        if (row < 0 || row >= (int)entries.size()) return true;
        // Top row is Category Settings
        if (!strcasecmp(entries[row].d_name, kCatSettingsLabel)) return true;
        // Bottom row is Uncategorized (when present)
        if (!strcasecmp(entries[row].d_name, "Uncategorized"))     return true;
        return false;
    }

    static constexpr float CAT_ROW_H = 16.0f;
    static constexpr float CAT_LIST_OFFSET_Y = 5.0f;
    static constexpr float CAT_SETTINGS_GAP = 9.0f;
    int categoryVisibleRows() const {
        const float panelH = 226.0f;
        float listH = panelH - 8.0f - CAT_LIST_OFFSET_Y;
        if (listH < CAT_ROW_H) listH = CAT_ROW_H;
        int visible = (int)(listH / CAT_ROW_H);
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
                    for (auto& s : subs) {
                        if (!strcasecmp(stripCategoryPrefixes(s).c_str(), base.c_str())) {
                            // Skip real game folders (paranoia)
                            std::string subAbs = joinDirFile(abs, s.c_str());
                            if (!findEbootCaseInsensitive(subAbs).empty()) continue;
                            renameIfExists(abs, s, want);
                        }
                    }
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
                std::sort(categoryNames.begin(), categoryNames.end(),
                          [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
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
            "PSP/GAME150/"
        };                                                    // drop PSX/ and Utility/ as roots
        for (int i = 0; i < 2; ++i) {
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
    void drawTextAligned(float x,float y,const char* s,unsigned col,int align) {
        if (font) {
            intraFontSetStyle(font,0.5f,col,0,0.0f,align);
            intraFontPrint(font,x,y,s);
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
        intraFontSetStyle(font, size, COLOR_WHITE, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
        return intraFontMeasureText(font, s);
    }
    void drawTextStyled(float x, float y, const char* s, float size, unsigned col, unsigned shadow,
                        int align, bool bold) {
        if (!s) return;
        if (font) {
            intraFontSetStyle(font, size, col, shadow, 0.0f, align);
            intraFontPrint(font, x, y, s);
            if (bold) intraFontPrint(font, x + 1.0f, y, s);
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
        float s = targetH / (float)h;
        float dw = (float)w * s;
        float dh = targetH;

        sceKernelDcacheWritebackRange(t->data, tbw * h * 4);
        sceGuTexFlush();
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, tbw, tbw, t->data);
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
        float s = targetH / (float)h;
        float dw = (float)w * s;
        float dh = targetH;

        sceKernelDcacheWritebackRange(t->data, tbw * h * 4);
        sceGuTexFlush();
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexImage(0, tbw, tbw, tbw, t->data);
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
    void drawHeader() {
        const int bannerH = 15;
        const float textY = (float)(bannerH - 4);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        drawRect(0, 0, SCREEN_WIDTH, bannerH, COLOR_BANNER);

        const char* leftLabel = "HomeBrew Sorter Ultimate";
        Texture* deviceIcon = nullptr;

        if (!showRoots && (view == View_Categories || view == View_GclSettings)) {
            leftLabel = currentDeviceHeaderName();
            // Determine which device icon to use
            if (!strncasecmp(currentDevice.c_str(), "ms0:", 4)) {
                deviceIcon = memcardSmallIcon;
            } else if (!strncasecmp(currentDevice.c_str(), "ef0:", 4)) {
                deviceIcon = internalSmallIcon;
            }
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

        drawTextAligned(textX, textY, leftLabel, COLOR_WHITE, INTRAFONT_ALIGN_LEFT);

        char mid[64];
        float midX = 195.0f;
        int midAlign = INTRAFONT_ALIGN_LEFT;
        if (showRoots) {
            snprintf(mid, sizeof(mid), "Main Menu");
            midX = 215.0f;
        } else if (view == View_Categories) {
            snprintf(mid, sizeof(mid), "Categories: %d", categoryDisplayCount());
            midX = SCREEN_WIDTH / 2.0f;
            midAlign = INTRAFONT_ALIGN_CENTER;
        } else if (view == View_GclSettings) {
            snprintf(mid, sizeof(mid), "Categories settings");
            midX = SCREEN_WIDTH / 2.0f;
            midAlign = INTRAFONT_ALIGN_CENTER;
        } else {
            snprintf(mid, sizeof(mid), "Games found: %d", (int)flatAll.size());
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
            if (actionMode != AM_None) {
                const char* v = (actionMode == AM_Copy) ? "Copy" : "Move";
                std::string msg = std::string("Select Destination Storage (") + v + " Mode)";
                drawText(10, 25, msg.c_str(), COLOR_WHITE);
            }
        } else {
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

    void drawRootMenu() {
        if (entries.empty()) return;

        intraFontActivate(font);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,0,0);

        const float panelX = 5.0f;
        const float panelY = 22.0f;
        const float panelW = 280.0f;
        const float panelH = 226.0f;
        drawRect((int)panelX, (int)panelY, (int)panelW, (int)panelH, COLOR_BANNER);

        const float ctrlX = 290.0f;
        const float ctrlY = 22.0f;
        const float ctrlW = 185.0f;
        const float keyH = 27.0f;
        drawRect((int)ctrlX, (int)ctrlY, (int)ctrlW, (int)keyH, COLOR_BANNER);
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
        const float keyY = ctrlY + 16.0f;
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
        drawRect((int)ctrlX, (int)creditY, (int)ctrlW, (int)creditH, COLOR_BANNER);
        const float creditTopLineY = creditY + 14.0f;
        const float creditBottomY = creditY + creditH - 6.0f;
        std::string credit = gHomeAnimEntries.empty() ? "No animations" : currentHomeAnimCredit();
        drawTextStyled(ctrlX + ctrlW * 0.5f, creditTopLineY, "Animation by:", 0.7f, keyTextCol, 0, INTRAFONT_ALIGN_CENTER, false);
        drawTextStyled(ctrlX + ctrlW * 0.5f, creditBottomY, credit.c_str(), 0.7f, COLOR_WHITE, 0, INTRAFONT_ALIGN_CENTER, false);

        const float switchGap = 5.0f;
        const float switchY = creditY + creditH + switchGap;
        const float switchH = 42.0f;
        drawRect((int)ctrlX, (int)switchY, (int)ctrlW, (int)switchH, COLOR_BANNER);

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
        const float animH = 103.0f;
        float animY = switchY + switchH + animGap - 3.0f;
        const float footerMargin = 18.0f + 4.0f;
        float maxAnimY = SCREEN_HEIGHT - footerMargin - animH;
        if (animY > maxAnimY) animY = maxAnimY;
        const float animX = ctrlX;
        const float animW = ctrlW;
        drawRect((int)animX, (int)animY, (int)animW, (int)animH, COLOR_BANNER);

        advanceHomeAnimationFrame();
        Texture* animTex = (gHomeAnimFrameIndex < gHomeAnimFrames.size()) ? gHomeAnimFrames[gHomeAnimFrameIndex].tex : nullptr;
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
        drawRect((int)panelX, (int)panelY, (int)panelW, (int)panelH, COLOR_BANNER);

        const float ctrlX = 290.0f;
        const float ctrlY = 22.0f;
        const float ctrlW = 185.0f;
        const float ctrlHFull = 94.0f;
        const float ctrlH = catSortMode ? (ctrlHFull - 17.0f) : ctrlHFull;
        drawRect((int)ctrlX, (int)ctrlY, (int)ctrlW, (int)ctrlH, COLOR_BANNER);

        // Mode switcher block (L/R)
        const float modeY = ctrlY + ctrlHFull + 5.0f;
        const float modeH = 42.0f;
        drawRect((int)ctrlX, (int)modeY, (int)ctrlW, (int)modeH, COLOR_BANNER);

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

        const unsigned keyTextCol = 0xFFBBBBBB;
        const unsigned saveTextCol = 0xFF17D0FD;
        const unsigned pickedGlowCol = 0xFF8CE8FE;

        // Controls box
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

        float keyY = ctrlY + 16.0f;
        const float keyX = ctrlX + 5.0f;
        if (!catSortMode) {
            drawKeyRowLeft(keyX, keyY, okIconTexture, "Select", true, keyTextCol);
            drawKeyRowLeft(keyX, keyY, selectIconTexture, "Rename", false, keyTextCol);
            drawKeyRowLeft(keyX, keyY, triangleIconTexture, "Add/Del. category", true, keyTextCol);
            drawKeyRowLeft(keyX, keyY, squareIconTexture, "(Un)Hide in XMB", true, keyTextCol);
            drawKeyRowLeft(keyX, keyY, circleIconTexture, "Back", true, keyTextCol);
        } else {
            drawKeyRowLeft(keyX, keyY, okIconTexture, "Pick up/Drop", true, keyTextCol);
            drawKeyRowLeft(keyX, keyY, startIconTexture, "Save order", false, saveTextCol);
            drawKeyRowLeft(keyX, keyY, squareIconTexture, "(Un)Hide in XMB", true, keyTextCol);
            drawKeyRowLeft(keyX, keyY, circleIconTexture, "Back", true, keyTextCol);
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
        const int startRow = scrollOffset;
        const int endRow = std::min(rowCount, startRow + visibleRows);
        const float iconGap = 10.0f;

        const bool hasSettingsRow = (rowCount > 0 && !strcasecmp(entries[0].d_name, kCatSettingsLabel));
        const unsigned long long nowUs = (unsigned long long)sceKernelGetSystemTimeWide();
        if (catScrollIndex != selectedIndex) {
            catScrollIndex = selectedIndex;
            catScrollStartUs = nowUs;
        }
        for (int i = startRow; i < endRow; ++i) {
            const char* name = entries[i].d_name;
            const bool sel = (i == selectedIndex);
            const bool disabled = (actionMode!=AM_None && opPhase==OP_SelectCategory &&
                                   opDisabledCategories.find(name) != opDisabledCategories.end());

            const bool locked = isCategoryRowLocked(i);
            const bool isPicked = (catSortMode && catPickActive && i == catPickIndex && !locked);

            unsigned baseCol = disabled ? COLOR_GRAY : COLOR_WHITE;
            unsigned textCol = sel ? COLOR_BLACK : baseCol;
            unsigned shadowCol = sel ? COLOR_WHITE : 0x40000000;

            (void)sel; // no row background highlight in categories list

            const int rowIndex = i - startRow;
            const float extraGap = (startRow == 0 && hasSettingsRow && rowIndex > 0) ? CAT_SETTINGS_GAP : 0.0f;
            const float centerY = top + rowH * (rowIndex + 0.5f) + extraGap;
            const float scale = 0.5f;
            const float lineH = 16.0f * scale;
            const float baseline = centerY + (lineH * 0.25f) - 2.0f;

            std::string label = name;
            if (isPicked) {
                shadowCol = pickedGlowCol;
            }

            const float textLeftX = panelX + 32.0f; // left aligned
            const float textRightX = panelX + panelW - 12.0f;
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
                const bool isUncRow = (strcasecmp(name, "Uncategorized") == 0);
                const bool isFiltered = (!isUncRow && isFilteredBaseName(stripCategoryPrefixes(name)));
                Texture* folderIcon = (isFiltered && catFolderIconGray) ? catFolderIconGray : catFolderIcon;
                if (folderIcon && folderIcon->data && folderIcon->height > 0) {
                    float iconW = (float)folderIcon->width * (iconHCat / (float)folderIcon->height);
                    float iconX = textLeftX - iconGap - iconW + 5.0f;
                    float iconY = centerY - (iconHCat * 0.5f) - 4.0f;
                    drawTextureScaled(folderIcon, iconX, iconY, iconHCat, disabled ? 0x66FFFFFF : 0xFFFFFFFF);
                    // Overlay C
                    const float cX = (float)(int)(iconX + (iconW * 0.25f) - 4.0f + 0.5f);
                    const float cY = (float)(int)(iconY + 13.0f + 0.5f);
                    drawTextStyled(cX, cY, "C",
                                   0.5f, disabled ? COLOR_GRAY : COLOR_BLACK, 0, INTRAFONT_ALIGN_LEFT, false);
                }
            }

            auto clipTextToWidth = [&](const std::string& s, float maxW, float offsetPx)->std::string {
                if (s.empty() || maxW <= 0.0f) return std::string();
                float skip = (offsetPx > 0.0f) ? offsetPx : 0.0f;
                float used = 0.0f;
                std::string out;
                out.reserve(s.size());
                for (size_t ci = 0; ci < s.size(); ++ci) {
                    char buf[2] = { s[ci], '\0' };
                    float cw = measureTextWidth(scale, buf);
                    if (skip > 0.0f) {
                        if (skip >= cw) { skip -= cw; continue; }
                        skip = 0.0f;
                        continue; // drop partially skipped char
                    }
                    if (used + cw > maxW) break;
                    out.push_back(s[ci]);
                    used += cw;
                }
                return out;
            };

            float textOffsetX = 0.0f;
            float textOverflow = 0.0f;
            if (textAvailW > 0.0f) {
                const float textW = measureTextWidth(scale, label.c_str());
                textOverflow = textW - textAvailW;
            }
            if (sel && textOverflow > 2.0f) {
                const float speed = 120.0f; // px/sec
                const double elapsed = (double)(nowUs - catScrollStartUs) / 1000000.0;
                textOffsetX = (float)(elapsed * speed);
                if (textOffsetX > textOverflow) textOffsetX = textOverflow;
            }

            std::string drawLabel = label;
            if (textAvailW > 0.0f) {
                const float clipOffset = (textOffsetX > 0.0f) ? textOffsetX : 0.0f;
                drawLabel = clipTextToWidth(label, textAvailW, clipOffset);
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

            if (isSettingsRow && rowIndex == 0) {
                const int lineY = (int)(top + rowH + 0.0f);
                const int lineX = (int)(panelX + 20.0f);
                const int lineW = (int)(panelW - 40.0f);
                drawHFadeLine(lineX, lineY, lineW, 1, 0xA0, 20, 0x00C0C0C0);
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

    void drawGclSettingsMenu() {
        if (entries.empty()) return;

        const float panelX = 5.0f;
        const float panelY = 22.0f;
        const float panelW = 280.0f;
        const float panelH = 226.0f;
        drawRect((int)panelX, (int)panelY, (int)panelW, (int)panelH, COLOR_BANNER);

        const float ctrlX = 290.0f;
        const float ctrlY = 22.0f;
        const float ctrlW = 185.0f;
        const float ctrlH = 95.0f;
        drawRect((int)ctrlX, (int)ctrlY, (int)ctrlW, (int)ctrlH, COLOR_BANNER);

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
            y += 17.0f;
        };

        float keyY = ctrlY + 16.0f;
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

    void drawFileList() {
        if (!showRoots && view == View_GclSettings) {
            drawGclSettingsMenu();
            return;
        }
        if (!showRoots && view == View_Categories) {
            drawCategoryMenu();
            return;
        }
        if (showRoots && opPhase != OP_SelectDevice) {
            drawRootMenu();
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
            std::string oldDisplay = entries[selectedIndex].d_name;
            if (!strcasecmp(oldDisplay.c_str(), kCatSettingsLabel) || !strcasecmp(oldDisplay.c_str(), "__GCL_SETTINGS__")) return;
            if (!strcasecmp(oldDisplay.c_str(), "Uncategorized")) return;

            std::string typed;
            if (!promptTextOSK("Rename Category", oldDisplay.c_str(), 64, typed)) return;

            // Treat user input as a BASE name (strip CAT_/XX if they typed it)
            typed = sanitizeFilename(stripCategoryPrefixes(typed));
            // If base didn’t change, nothing to do
            if (!strcasecmp(stripCategoryPrefixes(oldDisplay).c_str(), typed.c_str())) return;
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

            // First, rename the existing folder (whatever its current prefix) to the BASE on each root
            for (auto r : isoRoots) {
                std::string base = currentDevice + std::string(r);
                std::string from = base + oldDisplay;
                std::string to   = base + typed;
                if (dirExists(from)) {
                    int rc = sceIoRename(from.c_str(), to.c_str());
                    (rc >= 0) ? anyOk=true : anyFail=true;
                }
            }
            for (auto r : gameRoots) {
                std::string base = currentDevice + std::string(r);
                std::string from = base + oldDisplay;
                std::string to   = base + typed;
                if (dirExists(from)) {
                    int rc = sceIoRename(from.c_str(), to.c_str());
                    (rc >= 0) ? anyOk=true : anyFail=true;
                }
            }

            enforceCategorySchemeForDevice(currentDevice);
            // Old display name was captured earlier in this function as 'oldDisplay'

            // Compute the *display* name we will now use for this category
            std::string newDisplay = findDisplayNameForCategoryBase(currentDevice, typed);

            // Patch in-memory caches (icon paths, no-icon memo set, selection key) to follow the rename
            cachePatchRenameCategory(oldDisplay, newDisplay);
            updateFilterOnCategoryRename(oldDisplay, newDisplay);
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
        // Clear category cache every few frames to avoid stale data
        static int frameCount = 0;
        frameCount++;
        if (frameCount % 60 == 0) {  // Clear every 60 frames (~1 second at 60fps)
            categoryCache.clear();
        }

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
                    // Sorting disabled: alphabetical for stable browsing
                    std::sort(categoryNames.begin(), categoryNames.end(),
                            [](const std::string& a, const std::string& b){ return strcasecmp(a.c_str(), b.c_str()) < 0; });
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
        rowFlags.clear(); rowFreeBytes.clear(); rowReason.clear(); rowNeedBytes.clear();

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

        add(std::string("Category mode: ")      + gclModeLabel(gclCfg.mode));
        add(std::string("Category prefix: ")    + gclPrefixLabel(gclCfg.prefix));
        add(std::string("Show uncategorized: ") + gclUncatLabel(gclCfg.uncategorized, isPspGo()));
        add(std::string("Sort categories: ")    + gclSortLabel(gclCfg.catsort));
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
            optMenu = new OptionListMenu("Category mode", "Choose how you want your categories to be presented in the XMB.", items, SCREEN_WIDTH, SCREEN_HEIGHT);
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
            optMenu = new OptionListMenu("Category prefix", "Require the \"CAT_\" prefix on folders meant to act as categories.", items, SCREEN_WIDTH, SCREEN_HEIGHT);
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
            optMenu = new OptionListMenu("Show uncategorized", "Enable the \"Uncategorized\" category for games not placed in a category subfolder.", items, SCREEN_WIDTH, SCREEN_HEIGHT);
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
            optMenu = new OptionListMenu("Sort categories", "Sort categories by using a \"##\" prefix. E.g.: \"01MyCategory\", \"CAT_02MyCategory\"", items, SCREEN_WIDTH, SCREEN_HEIGHT);
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
        };

        auto addDeviceRow = [&](const std::string& r, bool present){
            SceIoDirent e{}; strncpy(e.d_name, r.c_str(), sizeof(e.d_name)-1);
            e.d_stat.st_mode = FIO_S_IFDIR;
            entries.push_back(e);
            entryPaths.emplace_back("");
            entryKinds.push_back(GameItem::ISO_FILE);

            uint8_t flags = 0;
            RowDisableReason reason = RD_NONE;
            uint64_t needB = 0, freeB = 0;

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
            }

            rowFlags.push_back(flags);
            rowFreeBytes.push_back(freeB);
            rowReason.push_back(reason);
            rowNeedBytes.push_back(needB);

            if (present && r == currentDevice) preselect = (int)entries.size() - 1;
        };

        if (opPhase != OP_SelectDevice) {
            addDeviceRow("ms0:/", hasMs);
            addDeviceRow("ef0:/", hasEf);
            addSimpleRow("__USB_MODE__");
            addSimpleRow("__GCL_TOGGLE__");
        } else {
            if (hasMs) addDeviceRow("ms0:/", true);
            if (hasEf) addDeviceRow("ef0:/", true);
        }

        showRoots = true; moving = false;

        selectedIndex = 0; scrollOffset = 0;
        if (preselect >= 0 && (rowFlags[preselect] & ROW_DISABLED) == 0) {
            selectedIndex = preselect;
        } else {
            for (int i = 0; i < (int)entries.size(); ++i)
                if ((rowFlags[i] & ROW_DISABLED) == 0) { selectedIndex = i; break; }
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
        SceUID d = kfeIoOpenDir(dir.c_str());
        if (d < 0) { logf("removeDirRecursive: open %s failed %d", dir.c_str(), d); return false; }
        SceIoDirent ent; memset(&ent, 0, sizeof(ent));
        while (kfeIoReadDir(d, &ent) > 0) {
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
        kfeIoCloseDir(d);
        return sceIoRmdir(dir.c_str()) >= 0;
    }
    static bool copyDirRecursive(const std::string& src, const std::string& dst, KernelFileExplorer* self) {
        logf("copyDirRecursive: %s -> %s", src.c_str(), dst.c_str());
        if (!ensureDirRecursive(dst)) { logf("  ensureDirRecursive failed"); return false; }
        SceUID d = kfeIoOpenDir(src.c_str());
        if (d < 0) { logf("  open src failed %d", d); return false; }

        SceIoDirent ent; memset(&ent, 0, sizeof(ent));
        bool ok = true;
        while (ok && kfeIoReadDir(d, &ent) > 0) {
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
        kfeIoCloseDir(d);
        logf("copyDirRecursive: %s", ok ? "OK" : "FAIL");
        return ok;
    }

    // Determine subroot for a given item path (preserve source tree)
    static std::string subrootFor(const std::string& path, GameItem::Kind kind) {
        // EBOOT trees to check
        const char* gameSubs[] = {"PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/","PSP/GAME/"};
        // ISO trees to check
        const char* isoSubs[]  = {"ISO/"};
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
        // pathAfterSubroot examples:
        //   "CAT_02PS1/SLUS12345", "Emulators/CPS1 (Capcom Play System 1)", "SLUS12345", "CAT_01PSP/Game.iso"
        size_t slash = pathAfterSubroot.find('/');
        if (slash == std::string::npos) {
            // No category component
            outCat.clear();
            outLeaf = pathAfterSubroot;
            return false;
        }
        // Treat the first segment as the category regardless of prefix
        outCat  = pathAfterSubroot.substr(0, slash);
        outLeaf = pathAfterSubroot.substr(slash + 1);
        return true;
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
        SceUID d = kfeIoOpenDir(dir.c_str());
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
        while (ok && kfeIoReadDir(d, &ent) > 0) {
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
        kfeIoCloseDir(d);

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



    // --- helper (NEW): “any device has categories?” quick probe for any CAT_ on a device ---
    bool deviceHasAnyCategory(const std::string& dev) const {
        const char* isoRoots[]  = {"ISO/"};                // drop ISO/PSP/ as a root
        const char* gameRoots[] = {"PSP/GAME/","PSP/GAME150/"}; // drop PSX/ and Utility/ as roots

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
                // Show filename detail + progress bar, even for instant renames
                msgBox->showProgress(basenameOf(src).c_str(), 0, 1);
                renderOneFrame();
            }

            if (!sameDevice(src, dst)) didCross = true;

            // Filename detail appears from the underlying copy/move implementation
            bool ok = moveOne(src, dst, k, this);
            if (msgBox) {
                msgBox->updateProgress(1, 1);
                renderOneFrame();
            }
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
        if (!gPopAnimFrames.empty()) { freeAnimationFrames(gPopAnimFrames); gPopAnimMinDelayUs = 0; }
        if (!gHomeAnimFrames.empty()) { freeAnimationFrames(gHomeAnimFrames); gHomeAnimMinDelayUs = 0; }
        gHomeAnimEntries.clear();
        gHomeAnimIndex = -1;
        gHomeAnimFrameIndex = 0;
        gHomeAnimNextUs = 0;
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
        font = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_MED);
        if (!font) pspDebugScreenInit();

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
                    : "Connect to PC...\nOn PSP Go, Bluetooth must be turned off in the System Settings.";
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




        // R trigger: toggle Sort mode on Categories; rename elsewhere
        if (pressed & PSP_CTRL_RTRIGGER) {
            if (showRoots) {
                cycleHomeAnimation(+1);
                return;
            }
            if (!showRoots && view == View_Categories) {
                if (!gclCfg.catsort) {
                    msgBox = new MessageBox(
                        "Sorting not enabled\n"
                        "To enter Sort mode, select \"Game Categories settings\" at the top of the list, then turn on the \"Sort categories\" option.",
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
            } else {
                beginRenameSelected();
            }
            return;
        }

        // □ toggle checkmark on current item
        if (pressed & PSP_CTRL_SQUARE) {
            if (!showRoots && view == View_Categories) {
                if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) return;
                const char* nm = entries[selectedIndex].d_name;
                if (!strcasecmp(nm, kCatSettingsLabel) || !strcasecmp(nm, "__GCL_SETTINGS__")) return;
                if (!strcasecmp(nm, "Uncategorized")) return;
                toggleGclFilterForCategory(nm);
                return;
            }
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
                        "To enter Sort mode, select \"Game Categories settings\" at the top of the list, then turn on the \"Sort categories\" option.",
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

        // SELECT: Categories → Rename; Content views → A→Z
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

            // Content views: quick A→Z
            if (!showRoots && (view == View_AllFlat || view == View_CategoryContents)) {
                moving = false;
                sortWorkingListAlpha(showTitles, workingList, selectedIndex, scrollOffset);
                refillRowsFromWorkingPreserveSel();
                return;
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

                    // NEW: Prime & debounce
                    SceCtrlData now{}; sceCtrlReadBufferPositive(&now, 1);
                    fileMenu->primeButtons(now.Buttons);
                    inputWaitRelease = true;
                } else if (view == View_Categories) {
                    std::vector<FileOpsItem> items = {
                        { "New",    false },
                        { "Delete", false }
                    };
                    menuContext = MC_CategoryOps;
                    fileMenu = new FileOpsMenu(items, SCREEN_WIDTH, SCREEN_HEIGHT);

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
                    if (view == View_GclSettings) {
                        if (selectedIndex + 1 < (int)entries.size()) {
                            const char* cur = entries[selectedIndex].d_name;
                            if (!strncasecmp(cur, "Sort categories:", 16) && gclCfg.prefix == 0) {
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
                        if (selectedIndex + 1 < (int)entries.size()){
                            selectedIndex++;
                            const int visible = (!showRoots && view == View_Categories) ? categoryVisibleRows() : MAX_DISPLAY;
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
                    // Preselect current state
                    int sel = (!gclArkOn && !gclProOn) ? 0 : (gclArkOn ? 1 : 2);
                    optMenu->setSelected(sel);

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
                        gUsbBox = new MessageBox(
                            "Connect to PC...\nOn PSP Go, Bluetooth must be turned off in the System Settings.",
                            circleIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 15, "Disconnect",
                            10, 18, 60, 9, 280, 110, PSP_CTRL_CIRCLE);
                        gUsbBox->setOkAlignLeft(true);
                        gUsbBox->setOkPosition(10, 7);
                        gUsbBox->setOkStyle(0.7f, 0xFFBBBBBB);
                        gUsbBox->setOkTextOffset(-2, -1);
                        gUsbBox->setSubtitleStyle(0.7f, 0xFFBBBBBB);
                        gUsbBox->setSubtitleGapAdjust(-8);
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

            // Global debounce: wait for full release before any modal eats input
            if (inputWaitRelease) {
                SceCtrlData pad{}; sceCtrlReadBufferPositive(&pad, 1);
                if (pad.Buttons != 0) continue;   // keep waiting
                inputWaitRelease = false;          // buttons now released
            }

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

            // NEW: Categories Lite option picker (modal)
            // NEW: Categories Lite option picker (modal)
            else if (optMenu) {
                if (!optMenu->update()) {
                    const int pick = optMenu->choice();   // -1 if canceled
                    const bool deleteReq = optMenu->deleteRequested();
                    const GclSettingKey pending = gclPending;  // capture BEFORE clearing
                    const bool wasRootPick = rootPickGcl;       // capture BEFORE clearing
                    delete optMenu; optMenu = nullptr;
                    optMenuOwnedLabels.clear();
                    gclPending = GCL_SK_None;
                    if (wasRootPick) rootPickGcl = false;

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
                            bool needArkFile = false;
                            bool needProFile = false;
                            if (wantPro) {
                                targetSeplugins = vshSe;
                                needProFile = true;
                            } else if (wantArk) {
                                targetSeplugins = pluginsSe;
                                needArkFile = true;
                            }
                            if (targetSeplugins.empty()) {
                                gclDevice = gclPickDeviceRoot();
                                targetSeplugins = gclSepluginsDirForRoot(gclDevice);
                                if (wantPro) {
                                    needProFile = true;
                                    vsh = joinDirFile(targetSeplugins, "VSH.txt");
                                } else if (wantArk) {
                                    needArkFile = true;
                                    plugins = joinDirFile(targetSeplugins, "PLUGINS.txt");
                                }
                            } else {
                                gclDevice = rootPrefix(targetSeplugins);
                            }
                            if (!dirExists(targetSeplugins)) sceIoMkdir(targetSeplugins.c_str(), 0777);

                            // Ensure the PRX is present if enabling either mode
                            if ((wantArk || wantPro) && !gclEnsurePrxPresent(targetSeplugins)) {
                                msgBox = new MessageBox("Could not install category_lite.prx from /resources.\nMake sure resources/category_lite.prx exists.",
                                                        nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14, PSP_CTRL_CIRCLE);
                            } else {
                                const bool pluginsExists = !plugins.empty() && pathExists(plugins);
                                const bool vshExists = !vsh.empty() && pathExists(vsh);

                                // Only touch the file we actually need for the chosen mode
                                if (needArkFile) gclWriteEnableToFile(plugins, wantArk, /*arkPluginsTxt=*/true);
                                if (needProFile) gclWriteEnableToFile(vsh,     wantPro, /*arkPluginsTxt=*/false);

                                // Ensure the other backend is disabled without creating empty files
                                if (wantPro) {
                                    if (pluginsExists) gclWriteEnableToFile(plugins, false, /*arkPluginsTxt=*/true);
                                } else if (wantArk) {
                                    if (vshExists) gclWriteEnableToFile(vsh, false, /*arkPluginsTxt=*/false);
                                } else {
                                    if (pluginsExists) gclWriteEnableToFile(plugins, false, /*arkPluginsTxt=*/true);
                                    if (vshExists)     gclWriteEnableToFile(vsh,     false, /*arkPluginsTxt=*/false);
                                }
                                gclArkOn = wantArk;
                                gclProOn = wantPro;
                                rootKeepGclSelection = true;
                                buildRootRows();   // reflect new state immediately
                            }
                        } else {
                            // Existing in-plugin settings pickers
                            switch (pending) {
                                case GCL_SK_Mode:   gclCfg.mode = (uint32_t)pick; break;
                                case GCL_SK_Prefix: gclCfg.prefix = (uint32_t)pick; break;
                                case GCL_SK_Uncat:  gclCfg.uncategorized = (uint32_t)pick; break;
                                case GCL_SK_Sort:   gclCfg.catsort = (uint32_t)pick; break;
                                case GCL_SK_Blacklist: {
                                    if (deleteReq || pick > 0) {
                                        deleteBlacklistAtIndex(pick - 1);
                                        openBlacklistModal(pick);
                                        continue;
                                    }

                                    if (pick == 0) {
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
                                    // Clear run-once guard so future opens are allowed to re-enforce if needed
                                    gclSchemeApplied.erase(rootPrefix(currentDevice));
                                    s_catNamingEnforced.erase(rootPrefix(currentDevice));

                                    enforceCategorySchemeForDevice(currentDevice);
                                    const bool onMs0 = (strncasecmp(currentDevice.c_str(), "ms0:/", 5) == 0);
                                    if (isPspGo()) {
                                        // Also clear & enforce the opposite root once to keep ms0:/ and ef0:/ consistent
                                        std::string other = onMs0 ? std::string("ef0:/") : std::string("ms0:/");
                                        gclSchemeApplied.erase(rootPrefix(other));
                                        s_catNamingEnforced.erase(rootPrefix(other));
                                        enforceCategorySchemeForDevice(other);
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
                                    patchCategoryCacheFromSettings();
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
                            // Delete category: count games first and confirm
                            if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) {
                                msgBox = new MessageBox("No category selected.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                            } else if (isCategoryRowLocked(selectedIndex)) {
                                // Don't allow deleting "Category Settings" or "Uncategorized"
                                msgBox = new MessageBox("Pick a category folder.", okIconTexture, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 20, "OK", 16, 18, 8, 14);
                            } else {
                                std::string cat = entries[selectedIndex].d_name;

                                int games = countGamesInCategory(currentDevice, cat);
                                char buf[128];
                                if (games > 0) {
                                    snprintf(buf, sizeof(buf),
                                             "%d game(s) are in this folder and will be deleted.\nPress X to confirm.",
                                             games);
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
                } else {
                    continue; // keep menu modal
                }
            }


            handleInput();
        }
    }

};

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
