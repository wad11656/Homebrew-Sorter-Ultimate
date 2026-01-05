#include <pspctrl.h>
#include <pspkernel.h>
#include <pspusb.h>
#include <pspusbstor.h>

#include <string.h>

PSP_MODULE_INFO("Usb", 0x1007, 1, 0);

int usbStarted = 0;

int PressPad(SceCtrlData pad, int buttons, int oldButtons, int old)
{
	if(old) return pad.Buttons & buttons && !(oldButtons & buttons);
	else return pad.Buttons & buttons;
}

int LoadStartModule(char *module)
{
	SceUID mod = sceKernelLoadModule(module, 0, NULL);
	if (mod < 0 && mod != SCE_KERNEL_ERROR_EXCLUSIVE_LOAD) return mod;
	if (mod >= 0)
	{
		mod = sceKernelStartModule(mod, strlen(module)+1, module, NULL, NULL);
		if (mod < 0) return mod;
	}
	return 0;
}

void UsbEnable()
{
	sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
	sceUsbStart(PSP_USBSTOR_DRIVERNAME, 0, 0);
	sceUsbstorBootSetCapacity(0x800000);
	sceUsbActivate(0x1c8);
	usbStarted = 1;
}

void UsbDisable()
{
	if(usbStarted)
	{
		sceUsbDeactivate(0x1c8);
		sceUsbStop(PSP_USBSTOR_DRIVERNAME, 0, 0);
		sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);
		usbStarted = 0;
	}
}

int main_thread(SceSize args, void* argp)
{
	SceCtrlData pad;
	u32 oldButtons = 0;
	LoadStartModule("flash0:/kd/semawm.prx");
	LoadStartModule("flash0:/kd/usbstor.prx");
	LoadStartModule("flash0:/kd/usbstormgr.prx");
	LoadStartModule("flash0:/kd/usbstorms.prx");
	LoadStartModule("flash0:/kd/usbstorboot.prx");

	while(1)
	{
		sceCtrlPeekBufferPositive(&pad, 1);
		if(PressPad(pad, PSP_CTRL_LTRIGGER, oldButtons, 1) && PressPad(pad, PSP_CTRL_RTRIGGER, oldButtons, 1) && PressPad(pad, PSP_CTRL_CIRCLE, oldButtons, 1))
		{
			if(usbStarted == 0) UsbEnable();
			else if(usbStarted == 1) UsbDisable();
		}
		oldButtons = pad.Buttons;
		sceKernelDelayThread(100000);
	}
	return 0;
}

int module_start(SceSize args, void* argp)
{
	SceUID main_thid = sceKernelCreateThread("UsbThread", main_thread, 0x18, 0x10000, 0, NULL);
	if(main_thid >= 0) sceKernelStartThread(main_thid, args, argp);
	return 0;
}

int module_stop(SceSize args, void *argp)
{
	return 0;
}

