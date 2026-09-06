TARGET       = KernelFileExplorer
OBJS         = main.o

# Compiler flags (inline include)
CFLAGS       = -O2 -G0 -Wall -I../libs/include
CXXFLAGS     = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS      = $(CFLAGS)

# Linker settings
LIBDIR       = ../libs/lib
LDFLAGS      =
LIBS         = -lpspsystemctrl_user

# PSP eboot metadata
EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = Kernel File Explorer
PSP_EBOOT_ICON  = ../ICON0.PNG
SFOFLAGS       += -d EFAWARE=1

# Include SDK build rules
PSPSDK          = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
