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

# Pull in the standard PSP SDK build rules
include $(PSPSDK)/lib/build.mak

# Regenerate your exports stub
fs_driver.S: ../fs_driver/exports.exp
	psp-build-exports -s ../fs_driver/exports.exp

# Ensure the stub exists before compiling the core + stub objects
src/kfe_app.o: fs_driver.S
fs_driver.o: fs_driver.S
