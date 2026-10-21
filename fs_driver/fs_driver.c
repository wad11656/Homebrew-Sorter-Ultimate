#include <pspsdk.h>
#include <pspiofilemgr_kernel.h>
#include <pspsyscon.h>
#include <pspthreadman.h>

#include "systemctrl.h"

PSP_MODULE_INFO("fs_driver", PSP_MODULE_KERNEL, 1, 2);
PSP_NO_CREATE_MAIN_THREAD();

int pspIoOpenDir(const char *dirname) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoDopen(dirname);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoReadDir(SceUID dir, SceIoDirent *dirent) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoDread(dir, dirent);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoCloseDir(SceUID dir) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoDclose(dir);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoMakeDir(const char *dir, SceMode mode) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoMkdir(dir, mode);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoRemoveDir(const char *path) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoRmdir(path);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoOpenFile(const char *file, int flags, SceMode mode) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoOpen(file, flags, mode);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoReadFile(SceUID file, void *data, SceSize size) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoRead(file, data, size);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoWriteFile(SceUID file, const void *data, SceSize size) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoWrite(file, data, size);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoCloseFile(SceUID file) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoClose(file);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoLseek(SceUID file, SceOff offset, int whence) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoLseek(file, offset, whence);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoLseek32(SceUID file, SceOff offset, int whence) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoLseek32(file, offset, whence);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoGetstat(const char *file, SceIoStat *stat) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoGetstat(file, stat);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoChstat(const char *file, SceIoStat *stat, int bits) {
    u32 k1   = pspSdkSetK1(0);
    int lvl  = sctrlKernelSetUserLevel(8);
    int ret  = sceIoChstat(file, stat, bits);
    pspSdkSetK1(k1);
    sctrlKernelSetUserLevel(lvl);
    return ret;
}

int pspIoRename(const char *oldname, const char *newname) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoRename(oldname, newname);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoRemoveFile(const char *file) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoRemove(file);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspIoDevctl(const char *dev, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	
	int ret = sceIoDevctl(dev, cmd, indata, inlen, outdata, outlen);
	
	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int pspSysconCtrlLED(int led, int state) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);

	int ret = sceSysconCtrlLED(led, state);

	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

static volatile int g_ledStop = 0;
static SceUID g_ledThread = -1;

static int ledThread(SceSize, void*) {
	while (!g_ledStop) {
		sceSysconCtrlLED(0, 0); // MS LED off
		sceKernelDelayThreadCB(1000);
	}
	sceSysconCtrlLED(0, 1); // restore on exit
	return 0;
}

int pspLedSuppressStart(void) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);

	if (g_ledThread >= 0) {
		pspSdkSetK1(k1);
		sctrlKernelSetUserLevel(level);
		return 0;
	}
	g_ledStop = 0;
	g_ledThread = sceKernelCreateThread("kfe_led", ledThread, 0x30, 0x1000, 0, NULL);
	if (g_ledThread >= 0) {
		sceKernelStartThread(g_ledThread, 0, NULL);
	} else {
		g_ledThread = -1;
	}

	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return 0;
}

int pspLedSuppressStop(void) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);

	if (g_ledThread < 0) {
		pspSdkSetK1(k1);
		sctrlKernelSetUserLevel(level);
		return 0;
	}
	g_ledStop = 1;
	sceKernelWaitThreadEnd(g_ledThread, NULL);
	sceKernelDeleteThread(g_ledThread);
	g_ledThread = -1;

	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return 0;
}

// Turns the HOME button off (0) or back on (1). Masks HOME in the controller
// driver itself, and also switches off the HOME menu. The CFW resolves the NIDs,
// which are randomized in the 6.xx driver libraries.
// Returns bit 0 = HOME mask applied, bit 1 = HOME menu call found.
#define NID_CTRL_SET_BUTTON_MASKS 0x7CA723DC
#define NID_IMPOSE_SET_HOME_POPUP 0x5595A71A
#define KFE_CTRL_HOME             0x00010000
int pspSetHomePopup(int enable) {
	u32 k1 = pspSdkSetK1(0);
	int level = sctrlKernelSetUserLevel(8);
	int ret = 0;

	u32 mask = sctrlHENFindFunction("sceController_Service", "sceCtrl_driver", NID_CTRL_SET_BUTTON_MASKS);
	if (mask) {
		((void (*)(unsigned int, unsigned int))mask)(KFE_CTRL_HOME, enable ? 0 : 1);
		ret |= 1;
	}

	u32 popup = sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", NID_IMPOSE_SET_HOME_POPUP);
	if (!popup) popup = sctrlHENFindFunction("sceImpose_Driver", "sceImpose", NID_IMPOSE_SET_HOME_POPUP);
	if (popup) {
		((int (*)(int))popup)(enable);
		ret |= 2;
	}

	pspSdkSetK1(k1);
	sctrlKernelSetUserLevel(level);
	return ret;
}

int module_start(SceSize args, void *argp) {
	return 0;
}

int module_stop(void) {
	return 0;
}
