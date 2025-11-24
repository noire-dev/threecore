
# SourceTech Makefile, GNU Make required
COMPILE_PLATFORM=$(shell uname | sed -e 's/_.*//' | tr '[:upper:]' '[:lower:]' | sed -e 's/\//_/g')
COMPILE_ARCH=$(shell uname -m | sed -e 's/i.86/x86/' | sed -e 's/^arm.*/arm/')

ifeq ($(shell uname -m),arm64)
  COMPILE_ARCH=aarch64
endif

ifeq ($(COMPILE_PLATFORM),mingw32)
  ifeq ($(COMPILE_ARCH),i386)
    COMPILE_ARCH=x86
  endif
endif

BUILD_CLIENT        = 1
BUILD_SERVER        = 1

# Build
MOUNT_DIR           = code

# General
USE_LOCAL_HEADERS   = 1

# Audio
USE_OGG_VORBIS      = 1
USE_CODEC_MP3       = 1
USE_INTERNAL_MP3    = 1

# Render
USE_VULKAN          = 1
USE_OPENGL          = 1
USE_OPENGL2         = 0
USE_OPENGL_API      = 1
USE_VULKAN_API      = 1

# valid options: opengl, vulkan
RENDERER_DEFAULT = opengl

CNAME            = sandbox
DNAME            = sandbox.ded

ifeq ($(V),1)
  echo_cmd=@:
  Q=
else
  echo_cmd=@echo
  Q=@
endif

ifeq ($(COMPILE_PLATFORM),cygwin)
  PLATFORM=mingw32
endif

ifndef PLATFORM
  PLATFORM=$(COMPILE_PLATFORM)
endif
export PLATFORM

ifeq ($(PLATFORM),mingw32)
  MINGW=1
  BUILD_SERVER=0
endif
ifeq ($(PLATFORM),mingw64)
  MINGW=1
  BUILD_SERVER=0
endif

ifeq ($(COMPILE_ARCH),i86pc)
  COMPILE_ARCH=x86
endif

ifeq ($(COMPILE_ARCH),amd64)
  COMPILE_ARCH=x86_64
endif
ifeq ($(COMPILE_ARCH),x64)
  COMPILE_ARCH=x86_64
endif

ifndef ARCH
  ARCH=$(COMPILE_ARCH)
endif
export ARCH

ifneq ($(PLATFORM),$(COMPILE_PLATFORM))
  CROSS_COMPILING=1
else
  CROSS_COMPILING=0

  ifneq ($(ARCH),$(COMPILE_ARCH))
    CROSS_COMPILING=1
  endif
endif
export CROSS_COMPILING

ifndef DESTDIR
  DESTDIR=bin
endif

ifndef BUILD_DIR
  BUILD_DIR=build
endif

ifeq ($(RENDERER_DEFAULT),opengl)
  USE_OPENGL=1
  USE_OPENGL2=0
  USE_VULKAN=0
  USE_OPENGL_API=1
  USE_VULKAN_API=0
endif
ifeq ($(RENDERER_DEFAULT),opengl2)
  USE_OPENGL=0
  USE_OPENGL2=1
  USE_VULKAN=0
  USE_OPENGL_API=1
  USE_VULKAN_API=0
endif
ifeq ($(RENDERER_DEFAULT),vulkan)
  USE_OPENGL=0
  USE_OPENGL2=0
  USE_VULKAN=1
  USE_OPENGL_API=0
endif

ifneq ($(USE_VULKAN),0)
  USE_VULKAN_API=1
endif

#############################################################################

BD=$(BUILD_DIR)/debug-$(PLATFORM)-$(ARCH)
BR=$(BUILD_DIR)/release-$(PLATFORM)-$(ARCH)
ADIR=$(MOUNT_DIR)/asm
CDIR=$(MOUNT_DIR)/client
SDIR=$(MOUNT_DIR)/server
RCDIR=$(MOUNT_DIR)/renderercommon
R1DIR=$(MOUNT_DIR)/renderer
R2DIR=$(MOUNT_DIR)/renderer2
RVDIR=$(MOUNT_DIR)/renderervk
SDLDIR=$(MOUNT_DIR)/sdl
SDLHDIR=$(MOUNT_DIR)/libsdl/include/SDL2

CMDIR=$(MOUNT_DIR)/qcommon
UDIR=$(MOUNT_DIR)/unix
W32DIR=$(MOUNT_DIR)/win32
BLIBDIR=$(MOUNT_DIR)/botlib
JPDIR=$(MOUNT_DIR)/libjpeg
OGGDIR=$(MOUNT_DIR)/libogg
VORBISDIR=$(MOUNT_DIR)/libvorbis
MADDIR=$(MOUNT_DIR)/libmad

bin_path=$(shell which $(1) 2> /dev/null)

STRIP ?= strip
PKG_CONFIG ?= pkg-config
INSTALL=install
MKDIR=mkdir -p

ifneq ($(call bin_path, $(PKG_CONFIG)),)
    SDL_INCLUDE ?= $(shell $(PKG_CONFIG) --silence-errors --cflags-only-I sdl2)
    SDL_LIBS ?= $(shell $(PKG_CONFIG) --silence-errors --libs sdl2)
endif

# supply some reasonable defaults for SDL/X11
ifeq ($(X11_INCLUDE),)
  X11_INCLUDE = -I/usr/X11R6/include
endif
ifeq ($(X11_LIBS),)
  X11_LIBS = -lX11
endif
ifeq ($(SDL_LIBS),)
  SDL_LIBS = -lSDL2
endif

# supply some reasonable defaults for ogg/vorbis
ifeq ($(OGG_FLAGS),)
  OGG_FLAGS = -I$(OGGDIR)/include
endif
ifeq ($(VORBIS_FLAGS),)
  VORBIS_FLAGS = -I$(VORBISDIR)/include -I$(VORBISDIR)/lib
endif

BASE_CFLAGS =

ifeq ($(USE_CODEC_MP3),1)
  BASE_CFLAGS += -DFPM_DEFAULT
endif

ifeq ($(USE_LOCAL_HEADERS),1)
  BASE_CFLAGS += -DUSE_LOCAL_HEADERS=1
endif

ifeq ($(USE_VULKAN_API),1)
  BASE_CFLAGS += -DUSE_VULKAN_API
endif

ifeq ($(USE_OPENGL_API),1)
  BASE_CFLAGS += -DUSE_OPENGL_API
endif

ifeq ($(USE_CODEC_MP3),1)
  BASE_CFLAGS += -DUSE_CODEC_MP3

  ifeq ($(USE_INTERNAL_MP3),1)
    MAD_CFLAGS = -DUSE_INTERNAL_MP3 -I$(MADDIR)/include
    ifeq ($(ARCH),x86)
      MAD_CFLAGS += -DFPM_INTEL
    else
    ifeq ($(ARCH),x86_64)
      MAD_CFLAGS += -DFPM_64BIT
    else
    ifeq ($(ARCH),arm)
      MAD_CFLAGS += -DFPM_ARM
    else
      MAD_CFLAGS += -DFPM_DEFAULT
    endif
    endif
    endif
  endif

  BASE_CFLAGS += $(MAD_CFLAGS)
endif

ARCHEXT=

CLIENT_EXTRA_FILES=

#############################################################################
# SETUP AND BUILD -- MINGW32
#############################################################################

ifdef MINGW

  ifeq ($(CROSS_COMPILING),1)
    # If CC is already set to something generic, we probably want to use
    # something more specific
    ifneq ($(findstring $(strip $(CC)),cc gcc),)
      CC=
    endif

    # We need to figure out the correct gcc and windres
    ifeq ($(ARCH),x86_64)
      MINGW_PREFIXES=x86_64-w64-mingw32 amd64-mingw32msvc
      STRIP=x86_64-w64-mingw32-strip
    endif
    ifeq ($(ARCH),x86)
      MINGW_PREFIXES=i686-w64-mingw32 i586-mingw32msvc i686-pc-mingw32
    endif

    ifndef CC
      CC=$(firstword $(strip $(foreach MINGW_PREFIX, $(MINGW_PREFIXES), \
         $(call bin_path, $(MINGW_PREFIX)-gcc))))
    endif

#   STRIP=$(MINGW_PREFIX)-strip -g

    ifndef WINDRES
      WINDRES=$(firstword $(strip $(foreach MINGW_PREFIX, $(MINGW_PREFIXES), \
         $(call bin_path, $(MINGW_PREFIX)-windres))))
    endif
  else
    # Some MinGW installations define CC to cc, but don't actually provide cc,
    # so check that CC points to a real binary and use gcc if it doesn't
    ifeq ($(call bin_path, $(CC)),)
      CC=gcc
    endif

  endif

  # using generic windres if specific one is not present
  ifeq ($(WINDRES),)
    WINDRES=windres
  endif

  ifeq ($(CC),)
    $(error Cannot find a suitable cross compiler for $(PLATFORM))
  endif

  BASE_CFLAGS += -Wall -Wimplicit -Wstrict-prototypes -DMINGW=1 -DWIN32_LEAN_AND_MEAN
  BASE_CFLAGS += -Wno-unused-result -fvisibility=hidden
  BASE_CFLAGS += -ffunction-sections -flto
  BASE_CFLAGS += -D__inline=inline

  ifeq ($(ARCH),x86_64)
    ARCHEXT = .x64
    BASE_CFLAGS += -m64
    OPTIMIZE = -O2 -ffast-math
  endif
  ifeq ($(ARCH),x86)
    BASE_CFLAGS += -m32
    OPTIMIZE = -O2 -march=i586 -mtune=i686 -ffast-math
  endif

  SHLIBEXT = dll
  SHLIBCFLAGS = -fPIC -fvisibility=hidden
  SHLIBLDFLAGS = -shared $(LDFLAGS)

  BINEXT = .exe

  LDFLAGS += -mwindows -Wl,--dynamicbase -Wl,--nxcompat
  LDFLAGS += -Wl,--gc-sections -fvisibility=hidden
  LDFLAGS += -lwsock32 -lgdi32 -lwinmm -lole32 -lws2_32 -lpsapi -lcomctl32
  LDFLAGS += -flto

  CLIENT_LDFLAGS=$(LDFLAGS)

  BASE_CFLAGS += -DUSE_LOCAL_HEADERS=1 -I$(SDLHDIR)
  ifeq ($(ARCH),x86)
    CLIENT_LDFLAGS += -L$(MOUNT_DIR)/libsdl/windows/lib32
    CLIENT_LDFLAGS += -lSDL2
    CLIENT_EXTRA_FILES += $(MOUNT_DIR)/libsdl/windows/lib32/SDL2.dll
  else
    CLIENT_LDFLAGS += -L$(MOUNT_DIR)/libsdl/windows/lib64
    CLIENT_LDFLAGS += -lSDL2
    CLIENT_EXTRA_FILES += $(MOUNT_DIR)/libsdl/windows/lib64/SDL2.dll
  endif

  ifeq ($(USE_OGG_VORBIS),1)
    BASE_CFLAGS += -DUSE_OGG_VORBIS $(OGG_FLAGS) $(VORBIS_FLAGS)
    CLIENT_LDFLAGS += $(OGG_LIBS) $(VORBIS_LIBS)
  endif

  DEBUG_CFLAGS = $(BASE_CFLAGS) -DDEBUG -D_DEBUG -g -O0
  RELEASE_CFLAGS = $(BASE_CFLAGS) -DNDEBUG $(OPTIMIZE)

else # !MINGW

ifeq ($(COMPILE_PLATFORM),darwin)

#############################################################################
# SETUP AND BUILD -- MACOS
#############################################################################

  BASE_CFLAGS += -Wall -Wimplicit -Wstrict-prototypes -pipe

  BASE_CFLAGS += -Wno-unused-result

  BASE_CFLAGS += -DMACOS_X

  OPTIMIZE = -O2 -fvisibility=hidden

  SHLIBEXT = dylib
  SHLIBCFLAGS = -fPIC -fvisibility=hidden
  SHLIBLDFLAGS = -dynamiclib $(LDFLAGS)

  ARCHEXT = .$(ARCH)

  LDFLAGS +=

  ifeq ($(ARCH),x86_64)
    BASE_CFLAGS += -arch x86_64
    LDFLAGS += -arch x86_64
  endif
  ifeq ($(ARCH),aarch64)
    BASE_CFLAGS += -arch arm64
    LDFLAGS += -arch arm64
  endif

  ifeq ($(USE_LOCAL_HEADERS),1)
    MACLIBSDIR=$(MOUNT_DIR)/libsdl/macos
    BASE_CFLAGS += -I$(SDLHDIR)
    CLIENT_LDFLAGS += $(MACLIBSDIR)/SDL2.dylib
    CLIENT_EXTRA_FILES += $(MACLIBSDIR)/SDL2.dylib
  else
  ifneq ($(SDL_INCLUDE),)
    BASE_CFLAGS += $(SDL_INCLUDE)
    CLIENT_LDFLAGS = $(SDL_LIBS)
  else
    BASE_CFLAGS += -I/Library/Frameworks/SDL2.framework/Headers
    CLIENT_LDFLAGS += -F/Library/Frameworks -framework SDL2
  endif
  endif

  ifeq ($(USE_OGG_VORBIS),1)
    BASE_CFLAGS += -DUSE_OGG_VORBIS $(OGG_FLAGS) $(VORBIS_FLAGS)
    CLIENT_LDFLAGS += $(OGG_LIBS) $(VORBIS_LIBS)
  endif

  DEBUG_CFLAGS = $(BASE_CFLAGS) -DDEBUG -D_DEBUG -g -O0
  RELEASE_CFLAGS = $(BASE_CFLAGS) -DNDEBUG $(OPTIMIZE)

else

#############################################################################
# SETUP AND BUILD -- *NIX PLATFORMS
#############################################################################

  BASE_CFLAGS += -Wall -Wimplicit -Wstrict-prototypes -pipe

  BASE_CFLAGS += -Wno-unused-result

  BASE_CFLAGS += -I/usr/include -I/usr/local/include

  OPTIMIZE = -O2 -fvisibility=hidden

  ifeq ($(ARCH),x86_64)
    ARCHEXT = .x64
  else
  ifeq ($(ARCH),x86)
    OPTIMIZE += -march=i586 -mtune=i686
  endif
  endif

  ifeq ($(ARCH),arm)
    OPTIMIZE += -march=armv7-a
    ARCHEXT = .arm
  endif

  ifeq ($(ARCH),aarch64)
    ARCHEXT = .aarch64
  endif

  SHLIBEXT = so
  SHLIBCFLAGS = -fPIC -fvisibility=hidden
  SHLIBLDFLAGS = -shared $(LDFLAGS)

  LDFLAGS += -lm
  LDFLAGS += -Wl,--gc-sections -fvisibility=hidden

  BASE_CFLAGS += $(SDL_INCLUDE)
  CLIENT_LDFLAGS = $(SDL_LIBS)

  ifeq ($(USE_OGG_VORBIS),1)
    BASE_CFLAGS += -DUSE_OGG_VORBIS $(OGG_FLAGS) $(VORBIS_FLAGS)
    CLIENT_LDFLAGS += $(OGG_LIBS) $(VORBIS_LIBS)
  endif

  ifeq ($(PLATFORM),linux)
    LDFLAGS += -ldl -Wl,--hash-style=both
    ifeq ($(ARCH),x86)
      # linux32 make ...
      BASE_CFLAGS += -m32
      LDFLAGS += -m32
    endif
  endif

  DEBUG_CFLAGS = $(BASE_CFLAGS) -DDEBUG -D_DEBUG -g -O0
  RELEASE_CFLAGS = $(BASE_CFLAGS) -DNDEBUG $(OPTIMIZE)

  DEBUG_LDFLAGS = -rdynamic

endif # *NIX platforms

endif # !MINGW

TARGET_CLIENT = $(CNAME)$(ARCHEXT)$(BINEXT)

TARGET_SERVER = $(DNAME)$(ARCHEXT)$(BINEXT)

TARGETS =

ifneq ($(BUILD_SERVER),0)
  TARGETS += $(B)/$(TARGET_SERVER)
endif

ifneq ($(BUILD_CLIENT),0)
  TARGETS += $(B)/$(TARGET_CLIENT)
endif

RENDCFLAGS=$(NOTSHLIBCFLAGS)

define DO_CC
$(echo_cmd) "CC $<"
$(Q)$(CC) $(CFLAGS) -o $@ -c $<
endef

define DO_REND_CC
$(echo_cmd) "REND_CC $<"
$(Q)$(CC) $(CFLAGS) $(RENDCFLAGS) -o $@ -c $<
endef

define DO_BOT_CC
$(echo_cmd) "BOT_CC $<"
$(Q)$(CC) $(CFLAGS) $(BOTCFLAGS) -DBOTLIB -o $@ -c $<
endef

define DO_AS
$(echo_cmd) "AS $<"
$(Q)$(CC) $(CFLAGS) -DELF -x assembler-with-cpp -o $@ -c $<
endef

define DO_DED_CC
$(echo_cmd) "DED_CC $<"
$(Q)$(CC) $(CFLAGS) -DDEDICATED -o $@ -c $<
endef

define DO_WINDRES
$(echo_cmd) "WINDRES $<"
$(Q)$(WINDRES) -i $< -o $@
endef

#############################################################################
# MAIN TARGETS
#############################################################################

default: release
all: debug release

debug:
	@$(MAKE) targets B=$(BD) CFLAGS="$(CFLAGS) $(DEBUG_CFLAGS)" LDFLAGS="$(LDFLAGS) $(DEBUG_LDFLAGS)" V=$(V)

release:
	@$(MAKE) targets B=$(BR) CFLAGS="$(CFLAGS) $(RELEASE_CFLAGS)" V=$(V)

define ADD_COPY_TARGET
TARGETS += $2
$2: $1
	$(echo_cmd) "CP $$<"
	@cp $1 $2
endef

# These functions allow us to generate rules for copying a list of files
# into the base directory of the build; this is useful for bundling libs,
# README files or whatever else
define GENERATE_COPY_TARGETS
$(foreach FILE,$1, \
  $(eval $(call ADD_COPY_TARGET, \
    $(FILE), \
    $(addprefix $(B)/,$(notdir $(FILE))))))
endef

ifneq ($(BUILD_CLIENT),0)
  $(call GENERATE_COPY_TARGETS,$(CLIENT_EXTRA_FILES))
endif

# Create the build directories and tools, print out
# an informational message, then start building
targets: makedirs
	@echo ""
	@echo "Building SourceTech in $(B):"
	@echo ""
	@echo "  PLATFORM: $(PLATFORM)"
	@echo "  ARCH: $(ARCH)"
	@echo "  COMPILE_PLATFORM: $(COMPILE_PLATFORM)"
	@echo "  COMPILE_ARCH: $(COMPILE_ARCH)"
ifdef MINGW
	@echo "  WINDRES: $(WINDRES)"
endif
	@echo "  CC: $(CC)"
	@echo ""
	@echo "  CFLAGS:"
	@for i in $(CFLAGS); \
	do \
		echo "    $$i"; \
	done
	@echo ""
	@echo "  Output:"
	@for i in $(TARGETS); \
	do \
		echo "    $$i"; \
	done
	@echo ""
ifneq ($(TARGETS),)
	@$(MAKE) $(TARGETS) V=$(V)
endif

makedirs:
	@if [ ! -d $(BUILD_DIR) ];then $(MKDIR) $(BUILD_DIR);fi
	@if [ ! -d $(B) ];then $(MKDIR) $(B);fi
	@if [ ! -d $(B)/client ];then $(MKDIR) $(B)/client;fi
	@if [ ! -d $(B)/client/jpeg ];then $(MKDIR) $(B)/client/jpeg;fi
	@if [ ! -d $(B)/client/ogg ];then $(MKDIR) $(B)/client/ogg;fi
	@if [ ! -d $(B)/client/vorbis ];then $(MKDIR) $(B)/client/vorbis;fi
	@if [ ! -d $(B)/client/libmad ];then $(MKDIR) $(B)/client/libmad;fi
	@if [ ! -d $(B)/rend1 ];then $(MKDIR) $(B)/rend1;fi
	@if [ ! -d $(B)/rendv ];then $(MKDIR) $(B)/rendv;fi
ifneq ($(BUILD_SERVER),0)
	@if [ ! -d $(B)/ded ];then $(MKDIR) $(B)/ded;fi
endif

#############################################################################
# CLIENT/SERVER
#############################################################################

Q3REND1OBJ = \
  $(B)/rend1/tr_arb.o \
  $(B)/rend1/tr_backend.o \
  $(B)/rend1/tr_bsp.o \
  $(B)/rend1/tr_cmds.o \
  $(B)/rend1/tr_curve.o \
  $(B)/rend1/tr_image.o \
  $(B)/rend1/tr_image_png.o \
  $(B)/rend1/tr_image_jpg.o \
  $(B)/rend1/tr_image_bmp.o \
  $(B)/rend1/tr_image_tga.o \
  $(B)/rend1/tr_image_pcx.o \
  $(B)/rend1/tr_init.o \
  $(B)/rend1/tr_light.o \
  $(B)/rend1/tr_main.o \
  $(B)/rend1/tr_marks.o \
  $(B)/rend1/tr_mesh.o \
  $(B)/rend1/tr_model.o \
  $(B)/rend1/tr_noise.o \
  $(B)/rend1/tr_scene.o \
  $(B)/rend1/tr_shade.o \
  $(B)/rend1/tr_shade_calc.o \
  $(B)/rend1/tr_shader.o \
  $(B)/rend1/tr_sky.o \
  $(B)/rend1/tr_surface.o \
  $(B)/rend1/tr_vbo.o \
  $(B)/rend1/tr_world.o

Q3RENDVOBJ = \
  $(B)/rendv/tr_backend.o \
  $(B)/rendv/tr_bsp.o \
  $(B)/rendv/tr_cmds.o \
  $(B)/rendv/tr_curve.o \
  $(B)/rendv/tr_image.o \
  $(B)/rendv/tr_image_png.o \
  $(B)/rendv/tr_image_jpg.o \
  $(B)/rendv/tr_image_bmp.o \
  $(B)/rendv/tr_image_tga.o \
  $(B)/rendv/tr_image_pcx.o \
  $(B)/rendv/tr_init.o \
  $(B)/rendv/tr_light.o \
  $(B)/rendv/tr_main.o \
  $(B)/rendv/tr_marks.o \
  $(B)/rendv/tr_mesh.o \
  $(B)/rendv/tr_model.o \
  $(B)/rendv/tr_noise.o \
  $(B)/rendv/tr_scene.o \
  $(B)/rendv/tr_shade.o \
  $(B)/rendv/tr_shade_calc.o \
  $(B)/rendv/tr_shader.o \
  $(B)/rendv/tr_shadows.o \
  $(B)/rendv/tr_sky.o \
  $(B)/rendv/tr_surface.o \
  $(B)/rendv/tr_world.o \
  $(B)/rendv/vk.o \
  $(B)/rendv/vk_flares.o \
  $(B)/rendv/vk_vbo.o \

JPGOBJ = \
  $(B)/client/jpeg/jaricom.o \
  $(B)/client/jpeg/jcapimin.o \
  $(B)/client/jpeg/jcapistd.o \
  $(B)/client/jpeg/jcarith.o \
  $(B)/client/jpeg/jccoefct.o  \
  $(B)/client/jpeg/jccolor.o \
  $(B)/client/jpeg/jcdctmgr.o \
  $(B)/client/jpeg/jchuff.o   \
  $(B)/client/jpeg/jcinit.o \
  $(B)/client/jpeg/jcmainct.o \
  $(B)/client/jpeg/jcmarker.o \
  $(B)/client/jpeg/jcmaster.o \
  $(B)/client/jpeg/jcomapi.o \
  $(B)/client/jpeg/jcparam.o \
  $(B)/client/jpeg/jcprepct.o \
  $(B)/client/jpeg/jcsample.o \
  $(B)/client/jpeg/jctrans.o \
  $(B)/client/jpeg/jdapimin.o \
  $(B)/client/jpeg/jdapistd.o \
  $(B)/client/jpeg/jdarith.o \
  $(B)/client/jpeg/jdatadst.o \
  $(B)/client/jpeg/jdatasrc.o \
  $(B)/client/jpeg/jdcoefct.o \
  $(B)/client/jpeg/jdcolor.o \
  $(B)/client/jpeg/jddctmgr.o \
  $(B)/client/jpeg/jdhuff.o \
  $(B)/client/jpeg/jdinput.o \
  $(B)/client/jpeg/jdmainct.o \
  $(B)/client/jpeg/jdmarker.o \
  $(B)/client/jpeg/jdmaster.o \
  $(B)/client/jpeg/jdmerge.o \
  $(B)/client/jpeg/jdpostct.o \
  $(B)/client/jpeg/jdsample.o \
  $(B)/client/jpeg/jdtrans.o \
  $(B)/client/jpeg/jerror.o \
  $(B)/client/jpeg/jfdctflt.o \
  $(B)/client/jpeg/jfdctfst.o \
  $(B)/client/jpeg/jfdctint.o \
  $(B)/client/jpeg/jidctflt.o \
  $(B)/client/jpeg/jidctfst.o \
  $(B)/client/jpeg/jidctint.o \
  $(B)/client/jpeg/jmemmgr.o \
  $(B)/client/jpeg/jmemnobs.o \
  $(B)/client/jpeg/jquant1.o \
  $(B)/client/jpeg/jquant2.o \
  $(B)/client/jpeg/jutils.o

ifeq ($(USE_OGG_VORBIS),1)
OGGOBJ = \
  $(B)/client/ogg/bitwise.o \
  $(B)/client/ogg/framing.o

VORBISOBJ = \
  $(B)/client/vorbis/analysis.o \
  $(B)/client/vorbis/bitrate.o \
  $(B)/client/vorbis/block.o \
  $(B)/client/vorbis/codebook.o \
  $(B)/client/vorbis/envelope.o \
  $(B)/client/vorbis/floor0.o \
  $(B)/client/vorbis/floor1.o \
  $(B)/client/vorbis/info.o \
  $(B)/client/vorbis/lookup.o \
  $(B)/client/vorbis/lpc.o \
  $(B)/client/vorbis/lsp.o \
  $(B)/client/vorbis/mapping0.o \
  $(B)/client/vorbis/mdct.o \
  $(B)/client/vorbis/psy.o \
  $(B)/client/vorbis/registry.o \
  $(B)/client/vorbis/res0.o \
  $(B)/client/vorbis/smallft.o \
  $(B)/client/vorbis/sharedbook.o \
  $(B)/client/vorbis/synthesis.o \
  $(B)/client/vorbis/vorbisfile.o \
  $(B)/client/vorbis/window.o
endif

ifeq ($(USE_CODEC_MP3),1)
MADOBJ = \
  $(B)/client/libmad/bit.o \
  $(B)/client/libmad/decoder.o \
  $(B)/client/libmad/fixed.o \
  $(B)/client/libmad/frame.o \
  $(B)/client/libmad/huffman.o \
  $(B)/client/libmad/layer3.o \
  $(B)/client/libmad/layer12.o \
  $(B)/client/libmad/stream.o \
  $(B)/client/libmad/synth.o \
  $(B)/client/libmad/timer.o \
  $(B)/client/libmad/version.o
endif

Q3OBJ = \
  $(B)/client/cl_cgame.o \
  $(B)/client/cl_cin.o \
  $(B)/client/cl_console.o \
  $(B)/client/cl_input.o \
  $(B)/client/cl_keys.o \
  $(B)/client/cl_main.o \
  $(B)/client/cl_net_chan.o \
  $(B)/client/cl_parse.o \
  $(B)/client/cl_scrn.o \
  $(B)/client/cl_ui.o \
  $(B)/client/cl_avi.o \
  $(B)/client/cl_jpeg.o \
  \
  $(B)/client/cm_load.o \
  $(B)/client/cm_patch.o \
  $(B)/client/cm_polylib.o \
  $(B)/client/cm_test.o \
  $(B)/client/cm_trace.o \
  \
  $(B)/client/cmd.o \
  $(B)/client/common.o \
  $(B)/client/cvar.o \
  $(B)/client/files.o \
  $(B)/client/history.o \
  $(B)/client/keys.o \
  $(B)/client/md4.o \
  $(B)/client/md5.o \
  $(B)/client/msg.o \
  $(B)/client/net_chan.o \
  $(B)/client/net_ip.o \
  $(B)/client/huffman.o \
  $(B)/client/huffman_static.o \
  \
  $(B)/client/snd_adpcm.o \
  $(B)/client/snd_dma.o \
  $(B)/client/snd_mem.o \
  $(B)/client/snd_mix.o \
  $(B)/client/snd_wavelet.o \
  \
  $(B)/client/snd_main.o \
  $(B)/client/snd_codec.o \
  $(B)/client/snd_codec_wav.o \
  \
  $(B)/client/sv_bot.o \
  $(B)/client/sv_ccmds.o \
  $(B)/client/sv_client.o \
  $(B)/client/sv_filter.o \
  $(B)/client/sv_game.o \
  $(B)/client/sv_init.o \
  $(B)/client/sv_main.o \
  $(B)/client/sv_net_chan.o \
  $(B)/client/sv_snapshot.o \
  $(B)/client/sv_world.o \
  \
  $(B)/client/q_math.o \
  $(B)/client/q_shared.o \
  \
  $(B)/client/unzip.o \
  $(B)/client/puff.o \
  $(B)/client/vm.o \
  $(B)/client/vm_interpreted.o \
  \
  $(B)/client/be_aas_bspq3.o \
  $(B)/client/be_aas_cluster.o \
  $(B)/client/be_aas_debug.o \
  $(B)/client/be_aas_entity.o \
  $(B)/client/be_aas_file.o \
  $(B)/client/be_aas_main.o \
  $(B)/client/be_aas_move.o \
  $(B)/client/be_aas_optimize.o \
  $(B)/client/be_aas_reach.o \
  $(B)/client/be_aas_route.o \
  $(B)/client/be_aas_routealt.o \
  $(B)/client/be_aas_sample.o \
  $(B)/client/be_ai_goal.o \
  $(B)/client/be_ai_move.o \
  $(B)/client/be_ea.o \
  $(B)/client/be_interface.o

ifeq ($(USE_OGG_VORBIS),1)
  Q3OBJ += $(OGGOBJ) $(VORBISOBJ) \
    $(B)/client/snd_codec_ogg.o
endif

ifeq ($(USE_CODEC_MP3),1)
  Q3OBJ += $(MADOBJ) \
    $(B)/client/snd_codec_mp3.o
endif

ifeq ($(USE_VULKAN),1)
  Q3OBJ += $(Q3RENDVOBJ) $(JPGOBJ)
else
  Q3OBJ += $(Q3REND1OBJ) $(JPGOBJ)
endif

ifeq ($(ARCH),x86)
ifndef MINGW
  Q3OBJ += \
    $(B)/client/snd_mix_mmx.o \
    $(B)/client/snd_mix_sse.o
endif
endif

ifeq ($(ARCH),x86_64)
  Q3OBJ += \
    $(B)/client/snd_mix_x86_64.o
endif

ifeq ($(ARCH),x86)
  Q3OBJ += $(B)/client/vm_x86.o
endif
ifeq ($(ARCH),x86_64)
  Q3OBJ += $(B)/client/vm_x86.o
endif
ifeq ($(ARCH),arm)
  Q3OBJ += $(B)/client/vm_armv7l.o
endif
ifeq ($(ARCH),aarch64)
  Q3OBJ += $(B)/client/vm_aarch64.o
endif
ifeq ($(ARCH),arm64)
  Q3OBJ += $(B)/client/vm_aarch64.o
endif

ifdef MINGW

  Q3OBJ += \
    $(B)/client/win_main.o \
    $(B)/client/win_shared.o \
    $(B)/client/win_resource.o

  Q3OBJ += \
      $(B)/client/sdl_glimp.o \
      $(B)/client/sdl_gamma.o \
      $(B)/client/sdl_input.o \
      $(B)/client/sdl_snd.o

else # !MINGW

  Q3OBJ += \
    $(B)/client/unix_main.o \
    $(B)/client/unix_shared.o \
    $(B)/client/linux_signals.o

  Q3OBJ += \
      $(B)/client/sdl_glimp.o \
      $(B)/client/sdl_gamma.o \
      $(B)/client/sdl_input.o \
      $(B)/client/sdl_snd.o

endif # !MINGW

# client binary

$(B)/$(TARGET_CLIENT): $(Q3OBJ)
	$(echo_cmd) "LD $@"
	$(Q)$(CC) -o $@ $(Q3OBJ) $(CLIENT_LDFLAGS) \
		$(LDFLAGS)

#############################################################################
# DEDICATED SERVER
#############################################################################

Q3DOBJ = \
  $(B)/ded/sv_bot.o \
  $(B)/ded/sv_client.o \
  $(B)/ded/sv_ccmds.o \
  $(B)/ded/sv_filter.o \
  $(B)/ded/sv_game.o \
  $(B)/ded/sv_init.o \
  $(B)/ded/sv_main.o \
  $(B)/ded/sv_net_chan.o \
  $(B)/ded/sv_snapshot.o \
  $(B)/ded/sv_world.o \
  \
  $(B)/ded/cm_load.o \
  $(B)/ded/cm_patch.o \
  $(B)/ded/cm_polylib.o \
  $(B)/ded/cm_test.o \
  $(B)/ded/cm_trace.o \
  $(B)/ded/cmd.o \
  $(B)/ded/common.o \
  $(B)/ded/cvar.o \
  $(B)/ded/files.o \
  $(B)/ded/history.o \
  $(B)/ded/keys.o \
  $(B)/ded/md4.o \
  $(B)/ded/md5.o \
  $(B)/ded/msg.o \
  $(B)/ded/net_chan.o \
  $(B)/ded/net_ip.o \
  $(B)/ded/huffman.o \
  $(B)/ded/huffman_static.o \
  \
  $(B)/ded/q_math.o \
  $(B)/ded/q_shared.o \
  \
  $(B)/ded/unzip.o \
  $(B)/ded/vm.o \
  $(B)/ded/vm_interpreted.o \
  \
  $(B)/ded/be_aas_bspq3.o \
  $(B)/ded/be_aas_cluster.o \
  $(B)/ded/be_aas_debug.o \
  $(B)/ded/be_aas_entity.o \
  $(B)/ded/be_aas_file.o \
  $(B)/ded/be_aas_main.o \
  $(B)/ded/be_aas_move.o \
  $(B)/ded/be_aas_optimize.o \
  $(B)/ded/be_aas_reach.o \
  $(B)/ded/be_aas_route.o \
  $(B)/ded/be_aas_routealt.o \
  $(B)/ded/be_aas_sample.o \
  $(B)/ded/be_ai_goal.o \
  $(B)/ded/be_ai_move.o \
  $(B)/ded/be_ea.o \
  $(B)/ded/be_interface.o

ifdef MINGW
  Q3DOBJ += \
  $(B)/ded/win_main.o \
  $(B)/client/win_resource.o \
  $(B)/ded/win_shared.o \
  $(B)/ded/win_syscon.o
else
  Q3DOBJ += \
  $(B)/ded/linux_signals.o \
  $(B)/ded/unix_main.o \
  $(B)/ded/unix_shared.o
endif

ifeq ($(ARCH),x86)
  Q3DOBJ += $(B)/ded/vm_x86.o
endif
ifeq ($(ARCH),x86_64)
  Q3DOBJ += $(B)/ded/vm_x86.o
endif
ifeq ($(ARCH),arm)
  Q3DOBJ += $(B)/ded/vm_armv7l.o
endif
ifeq ($(ARCH),aarch64)
  Q3DOBJ += $(B)/ded/vm_aarch64.o
endif
ifeq ($(ARCH),arm64)
  Q3DOBJ += $(B)/ded/vm_aarch64.o
endif

$(B)/$(TARGET_SERVER): $(Q3DOBJ)
	$(echo_cmd) "LD $@"
	$(Q)$(CC) -o $@ $(Q3DOBJ) $(LDFLAGS)

#############################################################################
## CLIENT/SERVER RULES
#############################################################################

$(B)/client/%.o: $(ADIR)/%.s
	$(DO_AS)

$(B)/client/%.o: $(CDIR)/%.c
	$(DO_CC)

$(B)/client/%.o: $(SDIR)/%.c
	$(DO_CC)

$(B)/client/%.o: $(CMDIR)/%.c
	$(DO_CC)

$(B)/client/%.o: $(BLIBDIR)/%.c
	$(DO_BOT_CC)

$(B)/client/jpeg/%.o: $(JPDIR)/%.c
	$(DO_CC)

$(B)/client/ogg/%.o: $(OGGDIR)/src/%.c
	$(DO_CC)

$(B)/client/vorbis/%.o: $(VORBISDIR)/lib/%.c
	$(DO_CC)

$(B)/client/libmad/%.o: $(MADDIR)/%.c
	$(DO_CC)

$(B)/client/%.o: $(SDLDIR)/%.c
	$(DO_CC)

$(B)/rend1/%.o: $(R1DIR)/%.c
	$(DO_REND_CC)

$(B)/rend1/%.o: $(RCDIR)/%.c
	$(DO_REND_CC)

$(B)/rend1/%.o: $(CMDIR)/%.c
	$(DO_REND_CC)

$(B)/rendv/%.o: $(RVDIR)/%.c
	$(DO_REND_CC)

$(B)/rendv/%.o: $(RCDIR)/%.c
	$(DO_REND_CC)

$(B)/rendv/%.o: $(CMDIR)/%.c
	$(DO_REND_CC)

$(B)/client/%.o: $(UDIR)/%.c
	$(DO_CC)

$(B)/client/%.o: $(W32DIR)/%.c
	$(DO_CC)

$(B)/client/%.o: $(W32DIR)/%.rc
	$(DO_WINDRES)

$(B)/ded/%.o: $(ADIR)/%.s
	$(DO_AS)

$(B)/ded/%.o: $(SDIR)/%.c
	$(DO_DED_CC)

$(B)/ded/%.o: $(CMDIR)/%.c
	$(DO_DED_CC)

$(B)/ded/%.o: $(BLIBDIR)/%.c
	$(DO_BOT_CC)

$(B)/ded/%.o: $(UDIR)/%.c
	$(DO_DED_CC)

$(B)/ded/%.o: $(W32DIR)/%.c
	$(DO_DED_CC)

$(B)/ded/%.o: $(W32DIR)/%.rc
	$(DO_WINDRES)

install: release
	@for i in $(TARGETS); do \
		if [ -f $(BR)$$i ]; then \
			$(INSTALL) -D -m 0755 "$(BR)/$$i" "$(DESTDIR)$$i"; \
			$(STRIP) "$(DESTDIR)$$i"; \
		fi \
	done
