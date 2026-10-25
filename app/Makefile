TARGET   = APP
OBJS = main.o src/kfe_app.o fs_driver.o src/Texture.o src/MessageBox.o \
       third_party/lz4/lz4.o \
       src/iso_titles_extras.o \
       third_party/minilzo/minilzo.o

# Locate the PSP SDK
PSPSDK := $(shell psp-config --pspsdk-path)

# Your extra include paths (stb in ../libs/include, our headers in ./include)
INCDIR = ../libs/include include third_party/minilzo third_party/lz4

# Compiler flags
CFLAGS   = -O2 -G0 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -std=gnu++11
ASFLAGS  = $(CFLAGS)

# Build as PRX so we can stub out kernel functions
BUILD_PRX = 1

# Your extra library paths
LIBDIR = ../libs/lib

# PSP libs + intraFont + C++ runtime
LIBS = -lintrafont \
       -lpsputility \
       -lpspgu -lpspgum -lpsprtc -lpspctrl -lpsppower \
       -lpspdisplay -lpspkubridge -lpspsystemctrl_user -lpspusb -lpspusbstor \
       -lpspdebug -lstdc++ -lm -lz

# PSP EBOOT metadata
EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = Homebrew Sorter Ultimate
PSP_EBOOT_ICON  = ../ICON0.png
PSP_EBOOT_PIC1  = ../PIC1.png
PSP_EBOOT_SND0  = ../SND0.AT3
SFOFLAGS       += -d EFAWARE=1

# The same EBOOT without the XMB music (SND0.AT3), built alongside the standard one every time
NOSOUND_EBOOT   = ../build-nosound/EBOOT.PBP
EXTRA_TARGETS  += $(NOSOUND_EBOOT)

# Pull in the standard PSP SDK build rules
include $(PSPSDK)/lib/build.mak

# Standard EBOOT's pieces, SND0 left out
$(NOSOUND_EBOOT): $(TARGET).prx $(PSP_EBOOT_SFO)
	mkdir -p $(dir $@)
	$(PACK_PBP) $@ $(PSP_EBOOT_SFO) $(PSP_EBOOT_ICON) $(PSP_EBOOT_ICON1) $(PSP_EBOOT_UNKPNG) \
		$(PSP_EBOOT_PIC1) NULL $(TARGET).prx $(PSP_EBOOT_PSAR)

# Regenerate your exports stub
fs_driver.S: ../fs_driver/exports.exp
	psp-build-exports -s ../fs_driver/exports.exp

# Ensure the stub exists before compiling the core + stub objects
# kfe_app.cpp is the class headers in src/: rebuild it when any of them (or our include/) changes
src/kfe_app.o: fs_driver.S $(wildcard src/*.h include/*.h)
fs_driver.o: fs_driver.S
