release: all
	psp-packer Usb.prx

TARGET = Usb
OBJS = main.o

CFLAGS = -O2 -G0 -Wall -fno-pic
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)) -c

LDFLAGS += -mno-crt0 -nostartfiles
LIBS = -lpspusb -lpspusbstor

BUILD_PRX = 1
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak