// kfe_app.cpp
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
#include <cmath>
#include <set>

#include "Texture.h"
#include "MessageBox.h"
#include "iso_titles_extras.h"
#include "kfe_app.h"
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
static Texture* ps1IconTexture = nullptr;
static Texture* homebrewIconTexture = nullptr;
static Texture* isoIconTexture = nullptr;
static Texture* updateIconTexture = nullptr;
static bool gEnablePopAnimations = false; // Toggle Populating animation
static std::vector<std::string> gPopAnimDirs;
static std::vector<size_t> gPopAnimOrder;
static size_t gPopAnimOrderIndex = 0;
static std::string gPopAnimLoadedDir;
static std::vector<MBAnimFrame> gPopAnimFrames;
static unsigned long long gPopAnimMinDelayUs = 0;
static const int POP_ANIM_TARGET_H = 60;
static const char* POP_ANIM_PREF = ""; // Set to a folder name to force a specific animation
