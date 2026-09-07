// kfe_app.cpp
// ----------------------------------------------------------------
// KernelFileExplorer — XMB-updated-time sorter
// Reorder flow: X = Pick/Drop, while picked use ↑/↓ to swap.
// START commits mtimes for the *current visual list*
//   - Bottom item gets "start" time, then +10s per step up,
//   - EBOOT entries: update **parent folder mtime**
//   - ISO entries:   update ISO file timestamps (mtime/ctime/atime)
//   - Initial list order = descending by chosen timestamp (folder mtime / ISO ctime).
// HOME menu via exit callback.
//
// Debug UI:
//   - Thumbstick up toggles timestamp overlay (shows the timestamp used to sort).
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
#include <psploadexec.h>
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
    int pspSysconCtrlLED(int led, int state);
    int pspLedSuppressStart(void);
    int pspLedSuppressStop(void);
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

// ---------------------------------------------------------------
// Adrenaline / ePSP USB bridge
// ---------------------------------------------------------------
// On a Vita the PSP's own USB stack does nothing: there is no emulated USB
// controller behind sceUsbStart/sceUsbActivate. Every Adrenaline-family CFW
// instead exposes the host's mass storage through a PSP-callable export, but
// which module and library hold it differs per CFW:
//
//   Adrenaline 7 (TheFloW)  module "SystemControl"    lib "SystemCtrlForUser"
//   Adrenaline 8 (isage)    module "Pentazemin"       lib "AdrenalineCtrl"
//   ARK-4 on Vita           module "ARKCompatLayer"   lib "AdrenalineCtrl"
//   ARK-5 on Vita           no bridge exists -- USB stays unavailable
//
// Nothing is imported statically. A stub for a library that is missing would
// stop the module loading altogether, and none of these exist on a real PSP,
// so the export tables are walked at runtime through kubridge instead. NIDs are
// derived from the function name, so they are the same in every fork.
#define ADR_NID_START_USB     0x80C0ED7B   // sctrlStartUsb
#define ADR_NID_STOP_USB      0x5FC12767   // sctrlStopUsb
#define ADR_NID_GET_USB_STATE 0x05D8E209   // sctrlGetUsbState

// M33 SDK module layout: kuKernelFindModuleByName fills in this shape.
typedef struct KfeSceModule2 {
    struct KfeSceModule2* next;         // 0x00
    unsigned short attribute;           // 0x04
    unsigned char  version[2];          // 0x06
    char           modname[27];         // 0x08
    char           terminal;            // 0x23
    char           mod_state;           // 0x24
    char           unk1;                // 0x25
    char           unk2[2];             // 0x26
    unsigned int   unk3;                // 0x28
    int            modid;               // 0x2C
    unsigned int   unk4;                // 0x30
    int            mem_id;              // 0x34
    unsigned int   mpid_text;           // 0x38
    unsigned int   mpid_data;           // 0x3C
    void*          ent_top;             // 0x40
    unsigned int   ent_size;            // 0x44
    void*          stub_top;            // 0x48
    unsigned int   stub_size;           // 0x4C
    unsigned int   entry_addr_;         // 0x50
    unsigned int   unk5[4];             // 0x54
    unsigned int   entry_addr;          // 0x64
    unsigned int   gp_value;            // 0x68
    unsigned int   text_addr;           // 0x6C
    unsigned int   text_size;           // 0x70
    unsigned int   data_size;           // 0x74
    unsigned int   bss_size;            // 0x78
    unsigned int   nsegment;            // 0x7C
    unsigned int   segmentaddr[4];      // 0x80
    unsigned int   segmentsize[4];      // 0x90
} KfeSceModule2;

typedef struct KfeLibEntry {
    const char*    libname;
    unsigned char  version[2];
    unsigned short attribute;
    unsigned char  len;                 // entry size in 32-bit words
    unsigned char  vstubcount;
    unsigned short stubcount;
    void*          entrytable;          // NIDs, then matching addresses
} KfeLibEntry;

// kuKernelMemcpy is a bare memcpy with k1 dropped -- it does no bounds checking
// whatsoever, so handing it a bad pointer faults in kernel mode and takes the
// whole system (or the Vita's PSP emulator) down with it. Every address is
// therefore range-checked against main RAM before it is dereferenced.
static bool kfeAddrSane(const void* p, unsigned int len) {
    if (!p || len == 0 || len > 0x10000) return false;
    const unsigned int a = ((unsigned int)p) & 0x1FFFFFFF;   // drop cache / kernel-segment bits
    if (a < 0x08000000) return false;                        // below main RAM
    if (a > 0x0C000000 - len) return false;                  // past 64MB, or the read would run off the end
    return true;
}

// Resolve one export by NID. Everything the walk touches lives in kernel memory,
// so each read is pulled across with kuKernelMemcpy. Returns 0 when the module,
// the library or the NID is absent -- the normal case on a real PSP -- and bails
// out rather than dereferencing anything that fails the sanity check.
static unsigned int kfeResolveExport(const char* modName, const char* libName, unsigned int nid) {
    if (!modName || !libName) return 0;

    const unsigned int wantLen = (unsigned int)strlen(libName);
    if (wantLen == 0 || wantLen > 32) return 0;

    KfeSceModule2 mod;
    memset(&mod, 0, sizeof(mod));
    if (kuKernelFindModuleByName((char*)modName, (SceModule*)&mod) < 0) return 0;
    if (mod.ent_size == 0 || mod.ent_size > 8192) return 0;
    if (!kfeAddrSane(mod.ent_top, mod.ent_size)) return 0;

    std::vector<unsigned char> ents(mod.ent_size);
    kuKernelMemcpy(ents.data(), mod.ent_top, mod.ent_size);

    for (unsigned int off = 0; off + sizeof(KfeLibEntry) <= mod.ent_size; ) {
        const KfeLibEntry* e = (const KfeLibEntry*)(ents.data() + off);
        if (e->len == 0) break;                       // malformed; stop rather than spin
        const unsigned int stride = (unsigned int)e->len * 4;

        // Read only strlen+1 bytes of the library name. A fixed-size read here can
        // walk off the end of the last string in a section and fault.
        char nameBuf[36];
        if (kfeAddrSane(e->libname, wantLen + 1)) {
            memset(nameBuf, 0, sizeof(nameBuf));
            kuKernelMemcpy(nameBuf, (void*)e->libname, wantLen + 1);
            if (nameBuf[wantLen] == '\0' && memcmp(nameBuf, libName, wantLen) == 0) {
                const unsigned int total = (unsigned int)e->stubcount + (unsigned int)e->vstubcount;
                const unsigned int bytes = total * 2 * (unsigned int)sizeof(unsigned int);
                if (total > 0 && total < 2048 && kfeAddrSane(e->entrytable, bytes)) {
                    std::vector<unsigned int> tab(total * 2);
                    kuKernelMemcpy(tab.data(), e->entrytable, bytes);
                    for (unsigned int j = 0; j < total; ++j) {
                        if (tab[j] == nid) return tab[total + j];
                    }
                }
            }
        }
        off += stride;
    }
    return 0;
}

struct KfeAdrUsb {
    bool         probed   = false;
    unsigned int startFn  = 0;
    unsigned int stopFn   = 0;
    unsigned int stateFn  = 0;
};
static KfeAdrUsb gAdrUsb;

// Probe once. Order matters only in that the first hit wins; a machine only ever
// has one of these modules loaded.
static void AdrUsbProbe() {
    if (gAdrUsb.probed) return;
    gAdrUsb.probed = true;

    static const struct { const char* mod; const char* lib; } kSources[] = {
        { "Pentazemin",     "AdrenalineCtrl"    },   // Adrenaline 8 (isage)
        { "ARKCompatLayer", "AdrenalineCtrl"    },   // ARK-4 on Vita
        { "SystemControl",  "SystemCtrlForUser" },   // Adrenaline 7 (TheFloW)
    };

    for (unsigned i = 0; i < sizeof(kSources)/sizeof(kSources[0]); ++i) {
        const unsigned int start = kfeResolveExport(kSources[i].mod, kSources[i].lib, ADR_NID_START_USB);
        if (!start) continue;
        const unsigned int stop = kfeResolveExport(kSources[i].mod, kSources[i].lib, ADR_NID_STOP_USB);
        if (!stop) continue;                          // half a bridge is no bridge
        gAdrUsb.startFn = start;
        gAdrUsb.stopFn  = stop;
        gAdrUsb.stateFn = kfeResolveExport(kSources[i].mod, kSources[i].lib, ADR_NID_GET_USB_STATE);
        return;
    }
}

static bool AdrUsbAvailable() { AdrUsbProbe(); return gAdrUsb.startFn != 0 && gAdrUsb.stopFn != 0; }

// These are kernel exports, so they go through kuKernelCall -- the same way
// ARK's own menu invokes them.
static int AdrKernelCall(unsigned int fn) {
    if (!fn) return -1;
    struct KernelCallArg args;
    memset(&args, 0, sizeof(args));
    const int rc = kuKernelCall((void*)fn, &args);
    if (rc < 0) return rc;
    return (int)args.ret1;
}

static int  AdrUsbStart() { return AdrKernelCall(gAdrUsb.startFn); }
static int  AdrUsbStop()  { return AdrKernelCall(gAdrUsb.stopFn);  }
// sctrlGetUsbState returns 1 when a host is connected, 2 when it is not.
static bool AdrUsbConnected() {
    if (!gAdrUsb.stateFn) return false;
    return AdrKernelCall(gAdrUsb.stateFn) == 1;
}

#define ADR_NID_IS_EF_ENABLE 0x74919684   // sctrlIsEfEnable

// Vita System Storage. Adrenaline 8 / Epinephrine is the only CFW that actually
// backs ef0: on a Vita -- under ARK-4 and ARK-5 the node still answers while
// nothing stands behind it, which is why probing the device is not enough. Ask
// the CFW instead: sctrlIsEfEnable() reports 0 when System Storage is disabled
// *or* configured to the same location as the Memory Stick, and 1 only when it
// is a genuinely separate, usable device.
static bool AdrEfEnabled() {
    static bool probed  = false;
    static bool enabled = false;
    if (probed) return enabled;          // settled once; changing it needs a relaunch anyway
    probed = true;

    // Only Pentazemin is consulted. ARK does not export sctrlIsEfEnable, so probing
    // ARKCompatLayer would buy nothing and would mean walking its export tables on
    // every boot under ARK.
    const unsigned int fn = kfeResolveExport("Pentazemin", "AdrenalineCtrl", ADR_NID_IS_EF_ENABLE);
    if (fn) enabled = (AdrKernelCall(fn) != 0);
    return enabled;                       // no CFW backing -> not usable
}

// MessageBox wraps by character count and counts the inline-icon token as literal
// text. The old "warning.png" token was 12 phantom characters on the warning line,
// which forced the panel absurdly wide to keep that line unwrapped. A one-character
// sentinel costs the wrapper almost nothing and still renders as the icon.
#define KFE_WARN_TOKEN "\x01"

// ---------------------------------------------------------------
// Pad polling
// ---------------------------------------------------------------
// sceCtrlReadBufferPositive blocks until the next 60Hz sampling cycle and hands
// back the OLDEST buffered sample. A frame that runs long therefore falls behind
// the ring buffer and starts acting on stale input, and only ever sees one sample
// per frame no matter how many arrived -- so a quick tap that lands between two
// frames is never seen at all. That is what makes list navigation feel sluggish
// and drop rapid presses.
//
// Peek gives the current state without blocking or consuming, and the latch
// reports every press that occurred since it was last read, so nothing is missed
// even when a frame overruns. Exactly one consumer may read the latch per frame --
// reading it clears it.
struct KfePad {
    unsigned buttons;   // currently held
    unsigned pressed;   // went down since the previous poll (never misses a tap)
};

// Edge detection is done here rather than with sceCtrlReadLatch. The latch is the
// textbook answer, but it silently reported nothing on ARK-5 -- handleInput ran
// every frame and saw no presses for seconds -- and its behaviour cannot be
// verified from outside the hardware. Peek plus a state diff is the mechanism the
// app already used, minus the blocking read that caused the original sluggishness.
static unsigned gKfePrevButtons = 0;

static KfePad kfePollPad() {
    SceCtrlData pad; memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(&pad, 1);
    KfePad r;
    r.buttons = pad.Buttons;
    r.pressed = pad.Buttons & ~gKfePrevButtons;
    gKfePrevButtons = pad.Buttons;
    return r;
}

// Used while waiting for a full release. Keeps the edge baseline in step so the
// buttons held during the wait cannot fire the moment input resumes.
static unsigned kfeDrainPad() {
    SceCtrlData pad; memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(&pad, 1);
    gKfePrevButtons = pad.Buttons;
    return pad.Buttons;
}

// USB state
static bool gUsbActive = false;
static MessageBox* gUsbBox = nullptr;


static bool gUsbShownConnected = false;
// Set when USB Mode is dismissed. The work that follows a disconnect (reloading
// the home animation, healing the plugin config) is slow and blocking, and it used
// to run right after the modal was destroyed -- so the screen looked interactive
// while nothing responded for seconds. It is deferred to the main loop instead,
// where it can be done behind a status modal.
static bool gUsbPostWorkPending = false;
// USB helpers
// On the Adrenaline bridge a single call does the whole job on the host side, so
// the PSP driver stack is left alone and Activate/Deactivate become no-ops.
static int UsbStartStacked() {
    if (AdrUsbAvailable()) return AdrUsbStart();
    EnsureUsbKernelModules();
    (void)sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
    (void)sceUsbStart(PSP_USBSTOR_DRIVERNAME, 0, 0);
    return 0;}
static void UsbStopStacked() {
    if (AdrUsbAvailable()) { AdrUsbStop(); return; }
    sceUsbDeactivate(0x1c8);
    sceUsbStop(PSP_USBSTOR_DRIVERNAME, 0, 0);
    sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);
}
static void UsbActivate()   { if (AdrUsbAvailable()) return; sceUsbActivate(0x1c8); }
static void UsbDeactivate() { if (AdrUsbAvailable()) return; sceUsbDeactivate(0x1c8); }


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
static Texture* ps1IconTextureGray = nullptr;
static Texture* homebrewIconTextureGray = nullptr;
static Texture* isoIconTextureGray = nullptr;
static Texture* updateIconTextureGray = nullptr;
static Texture* turbografxIconTexture = nullptr;
static Texture* turbografxIconTextureGray = nullptr;
static Texture* warningIconTexture = nullptr;
static Texture* updownIconTexture = nullptr;
static bool gEnablePopAnimations = false; // Toggle Populating animation
static std::vector<std::string> gPopAnimDirs;
static std::vector<size_t> gPopAnimOrder;
static size_t gPopAnimOrderIndex = 0;
static std::string gPopAnimLoadedDir;
static std::vector<MBAnimFrame> gPopAnimFrames;
static unsigned long long gPopAnimMinDelayUs = 0;
static unsigned long long gPopAnimTotalCycleUs = 0; // Total duration of one full animation cycle
static const int POP_ANIM_TARGET_H = 60;
static const char* POP_ANIM_PREF = ""; // Set to a folder name to force a specific animation
