# J2ME Libretro Core - Cross-compilation Makefile
# Default: Windows (MinGW)
# Supports: Windows (default), Linux (native), M17 ARM 32-bit (cross),
#           Nintendo Switch (devkitPro NRO homebrew)
#
# Build targets:
#   make                        - Build libretro core (Windows, default)
#   make platform=windows       - Same as default, explicitly Windows
#   make platform=linux         - Build libretro core (native Linux, .so)
#   make platform=m17           - Build for M17 ARM 32-bit (.so, release, stripped)
#   make app                    - Build standalone application (native)
#   make headless               - Headless application (no SDL2 required)
#   make switch                 - Nintendo Switch NRO (v34.85: maps to
#                                 platform=switch automatically; requires
#                                 devkitPro devkitA64 + libnx + switch-SDL2)
#   make switchui               - HOST verification build of the Switch
#                                 frontend (Linux/MSYS2 host gcc + host
#                                 SDL2; NOT the way to build the NRO!)
#   make clean                  - Clean all build artifacts
#
# When running inside MSYS2/MinGW shell on Windows, the native gcc is used
# automatically (no cross-compiler needed). CROSS_COMPILE is only used when
# building FROM Linux/other hosts.
#
# M17 cross-compilation:
#   make platform=m17 CROSS_COMPILE=/opt/m17-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-
#
# M17 release build: -Ofast, no debug symbols, stripped .so
#

# ==============================================
# Default settings
# ==============================================
TARGET_NAME := nojme
SRCDIR := src
INCDIR := include
OBJDIR := obj
BINDIR := bin

# Source files
JVM_SRCS := \
	$(SRCDIR)/jvm/classfile.c \
	$(SRCDIR)/jvm/debug_var.c \
	$(SRCDIR)/jvm/execute.c \
	$(SRCDIR)/jvm/heap.c \
	$(SRCDIR)/jvm/jvm.c \
	$(SRCDIR)/jvm/method_cache.c \
	$(SRCDIR)/jvm/native.c \
	$(SRCDIR)/jvm/nokia_m3d.c \
	$(SRCDIR)/jvm/opcodes.c \
	$(SRCDIR)/jvm/sax.c \
	$(SRCDIR)/jvm/stubs.c \
	$(SRCDIR)/jvm/threads.c \
	$(SRCDIR)/jvm/charset.c

MIDP_SRCS := \
	$(SRCDIR)/midp/display.c \
	$(SRCDIR)/midp/form.c \
	$(SRCDIR)/midp/graphics.c \
	$(SRCDIR)/midp/media.c \
	$(SRCDIR)/midp/mobile3d.c \
	$(SRCDIR)/midp/mascot3d.c \
	$(SRCDIR)/midp/race_probe.c \
	$(SRCDIR)/midp/rms.c \
	$(SRCDIR)/render/render.c

UTIL_SRCS := \
	$(SRCDIR)/utils/battery.c \
	$(SRCDIR)/utils/miniz.c \
	$(SRCDIR)/utils/jar_reader.c \
	$(SRCDIR)/utils/stb_image_impl.c \
	$(SRCDIR)/utils/utils.c \
	$(SRCDIR)/jvm/drm_bypass.c

MIDI_SRCS := \
	$(SRCDIR)/midi/midi.c

# ==============================================
# v34.69: AMR-NB decoder (vendored opencore-amrnb, Apache 2.0)
#
# Converted to plain C (no C++ anywhere): builds with the ordinary C
# compiler, no CXX, no libstdc++, no exceptions/RTTI machinery. The whole
# tree is plain C89/C99-style code with allocation via oscl_mem.h (malloc).
#
# The old -DNO_AMR_DECODER fallback (v34.53: used when no C++ compiler
# existed) is gone: a C compiler is mandatory for the core anyway, so the
# AMR-NB decoder is now always compiled in.
# ==============================================
AMR_WRAPPER_C := $(SRCDIR)/amr/amr_nb_dec.c
AMR_OPENCORE_C := $(wildcard $(SRCDIR)/amr/opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/*.c \
				 $(SRCDIR)/amr/opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/*.c)
AMR_C_SRCS := $(AMR_WRAPPER_C) $(AMR_OPENCORE_C)
AMR_INCS := -I$(SRCDIR)/amr -I$(SRCDIR)/amr/oscl \
	-I$(SRCDIR)/amr/opencore/codecs_v2/audio/gsm_amr/amr_nb/common/include \
	-I$(SRCDIR)/amr/opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/include \
	-I$(SRCDIR)/amr/opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src \
	-I$(SRCDIR)/amr/opencore/codecs_v2/audio/gsm_amr/common/dec/include

LIBRETRO_SRCS := \
	$(SRCDIR)/libretro/libretro.c \
	$(SRCDIR)/libretro/core_options.c \
	$(SRCDIR)/libretro/sdl_backend_stubs.c

# SDL sources (for standalone app only)
# NOTE: only the SDL2 backend is linked into the app. sdl_headless.c implements
# the SAME sdl_* API (used by the `headless` target); linking both objects
# together causes "multiple definition" link errors.
SDL_SRCS := $(SRCDIR)/sdl/sdl_graphics.c

ALL_LIBRETRO_SRCS := $(JVM_SRCS) $(MIDP_SRCS) $(UTIL_SRCS) $(MIDI_SRCS) $(LIBRETRO_SRCS)
ALL_APP_SRCS := $(JVM_SRCS) $(MIDP_SRCS) $(UTIL_SRCS) $(MIDI_SRCS) $(SDL_SRCS) $(SRCDIR)/main.c

# ==============================================
# Detect build environment
# ==============================================
# MSYSTEM is set inside MSYS2/MinGW shells (MINGW32, MINGW64, UCRT64, CLANG64, etc.)
# MINGW_PREFIX is set inside MinGW environments
# If either is set, we are ALREADY on Windows and gcc produces Windows binaries natively.
IS_MINGW_NATIVE := $(if $(MSYSTEM),1,$(if $(MINGW_PREFIX),1,0))

# ==============================================
# ARM-specific optimizations (used for all ARM targets)
# ==============================================
ARM_OPT_CFLAGS := -ftree-vectorize -fipa-cp-clone -finline-functions-called-once

# ==============================================
# v34.85: goal -> platform convenience mapping
# ==============================================
# `make switch` (without any platform= variable) selects the Switch
# platform automatically. The previously required `make platform=switch`
# still works and wins when both are given. Rationale: the v34.84 Switch
# instructions demanded `platform=switch`, the user naturally typed
# `make switch` (built nothing) and then `make switchui` (host-verification
# build - died with "make: gcc: No such file or directory" on Windows).
ifneq ($(filter switch,$(MAKECMDGOALS)),)
ifeq ($(strip $(platform)),)
platform := switch
endif
endif

# ==============================================
# Platform selection
# ==============================================

ifeq ($(platform), m17)
	# ==============================================
	# M17 ARM 32-bit cross-compilation (release, stripped)
	# ==============================================
	TARGET := $(BINDIR)/$(TARGET_NAME)_libretro.so

	CROSS_COMPILE ?= /opt/m17-toolchain/usr/bin/arm-buildroot-linux-gnueabihf-
	CC = $(CROSS_COMPILE)gcc
	AR = $(CROSS_COMPILE)ar
	STRIP = $(CROSS_COMPILE)strip

	# ARM 32-bit optimization flags - release, no debug.
	# v34.41 FIX: -Ofast replaced with -O3 -fno-fast-math -ffp-contract=off:
	# -ffast-math FMA-contracted the scalar rasterizer z/UV math, so NEON
	# and scalar diverged by ULPs (selftest: 6 raster FAILs, depth diff=495,
	# reproduced on qemu-arm gcc-14 -Ofast) and -ffinite-math-only allowed
	# the compiler to remove the sampler's NaN guards. -ffp-contract=off
	# makes FP evaluation deterministic on every GCC (m17 6.4 ... 14+).
	# ARM 32-bit optimization flags - release, no debug — release, no debug
	CFLAGS := -Wall -Wextra -std=c11 \
	-I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include -I$(SRCDIR)/jvm -I$(SRCDIR)/midp \
	-I$(SRCDIR)/utils -I$(SRCDIR)/libretro \
	-DLIBRETRO -DARM -D_GNU_SOURCE -DJ2ME_DEBUG=0 -DOPT_NO_BOUNDS_CHECK \
	-O3 -fno-fast-math -ffp-contract=off -fPIC -MMD -MP \
	-fdata-sections -ffunction-sections \
	-fno-stack-protector -fomit-frame-pointer \
	-fmerge-all-constants -fno-math-errno \
	-marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
	-flto $(ARM_OPT_CFLAGS)

	LDFLAGS := -shared -Wl,--no-undefined -Wl,--gc-sections \
	-Wl,--version-script=$(SRCDIR)/libretro/link.T \
	-flto -fuse-linker-plugin \
	-lm -pthread

	HAVE_NEON = 1
	ARCH = arm
	PLATFORM_NAME = M17 ARM 32-bit

	# v34.24: expose NEON to the preprocessor. render.c also autodetects via
	# __ARM_NEON__ (defined by -mfpu=neon-vfpv4); HAVE_NEON makes the build
	# flag queryable and lets other units conditional-compile on it.
	ifeq ($(HAVE_NEON),1)
	CFLAGS += -DHAVE_NEON=1
	endif

	# v34.80: GCC >= 14 with LTO downgrades the static __thread/_Thread_local
	# vars (src/jvm/threads.c, src/jvm/execute.c) to TLS local-exec during the
	# link-time recompile — "R_ARM_TLS_LE32 relocation not permitted in shared
	# object" (observed: bootlin armv7 gcc 14.3; -ftls-model=* does NOT help,
	# the model is recomputed from whole-program visibility). The stock m17
	# toolchain (gcc 6.4) links the same flags fine. NOJME_M17_NO_LTO=1 drops
	# LTO for toolchains that hit this (cost: a little codegen quality).
	ifeq ($(NOJME_M17_NO_LTO),1)
	CFLAGS := $(filter-out -flto,$(CFLAGS))
	LDFLAGS := $(filter-out -flto -fuse-linker-plugin,$(LDFLAGS))
	endif

else ifeq ($(platform), switch)
        # ==============================================
        # Nintendo Switch - LIBRETRO CORE (v34.86)
        # ==============================================
        # `make switch` / `make platform=switch` builds the core for the
        # Switch as bin/nojme_libretro_libnx.a - the canonical libretro
        # convention for this platform (snes9x, Genesis-Plus-GX:
        #   TARGET := $(TARGET_NAME)_libretro_libnx.a, STATIC_LINKING=1).
        # Switch RetroArch links cores STATICALLY into its .nro bundle
        # (libnx homebrew has no dlopen for arbitrary .so; the libretro
        # buildbot ships no separate .so cores for switch) - the linking
        # step is documented in README-СБОРКА.txt (Switch section).
        # Prerequisites: devkitPro pacman group "switch-dev" (devkitA64 +
        # libnx). NO switch-SDL2 needed - the core renders through the
        # libretro video/audio callbacks via the mock-SDL backend, so
        # SDL2 headers are never included in this configuration.
        # Build:      make switch   (or: make platform=switch)
        # Output:     bin/nojme_libretro_libnx.a
        # The standalone GUI emulator NRO is the `make switchui` goal.
        ifeq ($(strip $(DEVKITPRO)),)
        ifeq ($(OS),Windows_NT)
        DEVKITPRO := C:/devkitPro
        else
        DEVKITPRO := /opt/devkitpro
        endif
        DEVKITPRO_DEFAULTED = 1
        else
        DEVKITPRO := $(subst \,/,$(DEVKITPRO))
        endif
        DEVKITA64 := $(DEVKITPRO)/devkitA64
        LIBNX     := $(DEVKITPRO)/libnx
        PREFIX    := $(DEVKITA64)/bin/aarch64-none-elf-

        # Hard stop with a pointed message when the toolchain is absent.
        # (Only evaluated when platform=switch - other targets unaffected.)
        ifeq ($(DEVKITPRO_DEFAULTED),1)
        $(info NOTE: DEVKITPRO is not set - autodetected to $(DEVKITPRO). Override with: make switch DEVKITPRO=<path>)
        endif
        ifeq ($(wildcard $(DEVKITA64)/bin/aarch64-none-elf-gcc*),)
        $(error devkitA64 toolchain not found at $(DEVKITPRO)/devkitA64. Install the devkitPro "switch-dev" pacman group or point DEVKITPRO at your devkitPro folder, e.g.: make switch DEVKITPRO=C:/devkitPro)
        endif
        ifeq ($(wildcard $(LIBNX)/switch.specs),)
        $(error libnx not found at $(LIBNX) - install the devkitPro "switch-dev" pacman group)
        endif

        CC       := $(PREFIX)gcc
        AR       := $(PREFIX)ar
        STRIP    := $(PREFIX)strip

        # v34.86 core flags: mirror of the canonical switch_rules ARCH
        # (-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft) plus the
        # GCC 14.x devkitA64 paranoia set from the user's working template
        # (-O2 never -O3, NO LTO, -fno-strict-aliasing -fno-strict-overflow
        # -fno-delete-null-pointer-checks -fno-ipa-pure-const: the IPA
        # pure/const pass miscompiles pthread callback code). -fPIE because
        # Switch code is loaded at arbitrary addresses. -mtp=soft is REQUIRED
        # by libnx userland (the emulator's __thread variables in
        # threads.c / execute.c must go through the soft thread pointer).
        # -D__SWITCH__ activates the newlib portability guards (native.c:
        # no <sys/syscall.h>/syscall(2)/setpriority on devkitA64).
        CFLAGS := -Wall -Wextra -std=c11 \
        -I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include \
        -DLIBRETRO -D__SWITCH__ -DHAVE_LIBNX -D_GNU_SOURCE -DJ2ME_DEBUG=0 \
        -O2 -MMD -MP \
        -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
        -fno-strict-aliasing -fno-strict-overflow \
        -fno-delete-null-pointer-checks -fno-ipa-pure-const \
        -ffunction-sections -fdata-sections \
        -I$(LIBNX)/include

        # No LDFLAGS here: the .a is archived with $(AR) rcs (see the
        # $(TARGET) recipe below); -specs=$(LIBNX)/switch.specs is applied
        # by RetroArch's own link when the core is embedded.
        TARGET := $(BINDIR)/$(TARGET_NAME)_libretro_libnx.a
        # v34.87: dedicated object tree for the Switch core build. The shared
        # obj/libretro tree is ALSO used by the host core builds (linux/windows);
        # running `make switch` after a host `make` / `make platform=linux` in the
        # same tree would let make reuse the stale x86-64 .o files (same paths,
        # newer timestamps) and silently archive them into the aarch64 .a. A
        # separate tree makes cross-contamination impossible. (The NRO build
        # already has its own obj/switch tree since v34.86.)
        LR_OBJDIR := $(OBJDIR)/libretro-switch
        # v34.87: the cooperative scheduler (src/jvm/threads.c) needs
        # get/set/make/swapcontext; newlib has no <ucontext.h>, so the vendored
        # aarch64 implementation (src/switch/switch_ucontext.c, functionally
        # tested via scripts/switch_ucontext_test.sh under qemu-aarch64) is part
        # of the Switch CORE build too.
        ALL_LIBRETRO_SRCS += $(SRCDIR)/switch/switch_ucontext.c
        ARCH = aarch64
        PLATFORM_NAME = Nintendo Switch libretro core (devkitPro)

else ifeq ($(platform), linux)
	# ==============================================
	# Native Linux build
	# ==============================================
	TARGET := $(BINDIR)/$(TARGET_NAME)_libretro.so

	CC ?= gcc
	AR ?= ar
	STRIP ?= strip

	# Detect architecture
	UNAME_M := $(shell uname -m)
	ifeq ($(UNAME_M),x86_64)
	ARCH = x86_64
	ARCH_CFLAGS = -march=native
	else ifeq ($(UNAME_M),aarch64)
	ARCH = arm64
	ARCH_CFLAGS = -march=armv8-a
	else ifeq ($(UNAME_M),armv7l)
	ARCH = arm
	ARCH_CFLAGS = -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
	else
	ARCH = $(UNAME_M)
	ARCH_CFLAGS =
	endif

	CFLAGS := -Wall -Wextra -std=c11 \
	-I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include \
	-DLIBRETRO -D_GNU_SOURCE -DJ2ME_DEBUG=0 \
	-O2 -fPIC -MMD -MP $(ARCH_CFLAGS)

	LDFLAGS := -shared -Wl,--no-undefined \
	-Wl,--version-script=$(SRCDIR)/libretro/link.T \
	-lm -pthread

	PLATFORM_NAME = Linux $(ARCH)

	# v34.84 (midlets3 session): AddressSanitizer build -
	# make platform=linux ASAN=1
	# (run with LD_PRELOAD=$(gcc -print-file-name=libasan.so) if needed).
	ifdef ASAN
	CFLAGS += -fsanitize=address -fno-omit-frame-pointer -g
	LDFLAGS += -fsanitize=address
	endif

	# ARM-specific optimizations
	ifeq ($(ARCH),arm)
	# v34.41: -fno-fast-math -ffp-contract=off - same reason as m17 above:
	# VFPv4 VFMA contraction broke the bit-equality of NEON vs scalar.
	CFLAGS += -O3 -flto -fno-fast-math -ffp-contract=off $(ARM_OPT_CFLAGS)
	LDFLAGS += -flto -fuse-linker-plugin
	endif
	# v34.24: native armv7l with NEON (uname detected) - pass it through.
	# (marker: NOJME_NEON_NATIVE_ARM)
	ifeq ($(UNAME_M),armv7l)
	CFLAGS += -DHAVE_NEON=1
	endif

else
	# ==============================================
	# Windows (MinGW) — DEFAULT
	# ==============================================
	TARGET := $(BINDIR)/$(TARGET_NAME)_libretro.dll

ifeq ($(IS_MINGW_NATIVE),1)
	# ---- Running inside MSYS2/MinGW on Windows ----
	CC ?= gcc
	AR ?= ar
	STRIP ?= strip

	# Detect 32/64 bit from MSYSTEM or uname
	IS_64BIT := $(if $(findstring 64,$(MSYSTEM)),1,0)
	ifeq ($(IS_64BIT),1)
	ARCH = x86_64
	else
	ARCH = x86
	endif
	PLATFORM_NAME = Windows $(ARCH) (native MinGW)
else
	# ---- Cross-compiling from Linux to Windows ----
	CROSS_COMPILE ?= x86_64-w64-mingw32-
	CC = $(CROSS_COMPILE)gcc
	AR = $(CROSS_COMPILE)ar
	STRIP = $(CROSS_COMPILE)strip

	ARCH = x86_64
	PLATFORM_NAME = Windows $(ARCH) (cross-compile)
endif

	CFLAGS := -Wall -Wextra -std=c11 \
	-I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include \
	-DLIBRETRO -DWIN32 -D_GNU_SOURCE -DJ2ME_DEBUG=0 \
	-g -O2 -fPIC -MMD -MP \
	-D__USE_MINGW_ANSI_STDIO=1

	# v34.69: no -static-libstdc++ anymore — the whole core (incl. the
	# AMR-NB decoder) is plain C; only libgcc can ever be pulled in.
	LDFLAGS := -shared -static-libgcc \
	-Wl,--out-implib,$(BINDIR)/lib$(TARGET_NAME)_libretro.a \
	-lm
endif

# ==============================================
# Object files (all go to obj/<build-type>/)
# ==============================================
# v34.87: $(LR_OBJDIR) defaults to obj/libretro (host core builds); the
# platform=switch block overrides it to obj/libretro-switch (see above).
LR_OBJDIR ?= $(OBJDIR)/libretro
ALL_LIBRETRO_OBJS := $(patsubst $(SRCDIR)/%.c,$(LR_OBJDIR)/%.o,$(ALL_LIBRETRO_SRCS))
# v34.69: AMR-NB decoder is plain C — always built, no CXX probe.
ALL_LIBRETRO_OBJS += $(patsubst $(SRCDIR)/amr/%.c,$(LR_OBJDIR)/amr/%.o,$(AMR_C_SRCS))

# ==============================================
# Build targets
# ==============================================

.PHONY: all clean app dirs check info libretro

# Default: build libretro core (Windows by default)
all: libretro

# Build libretro core. v34.86: platform=switch builds the STATIC core
# archive through this same target ($(TARGET) = bin/nojme_libretro_libnx.a)
# - no special alias needed anymore. `make switch` chains here through the
# phony `switch` goal defined right after the link recipe.
libretro: check_compiler dirs $(TARGET)
	@echo ""
	@echo "============================================"
	@echo "Libretro core built successfully!"
	@echo "============================================"
	@echo "Output:    $(TARGET)"
	@echo "Platform:  $(PLATFORM_NAME)"
	@echo "Arch:      $(ARCH)"
	@echo "Compiler:  $(CC)"
	@echo "Objects:   $(LR_OBJDIR)/"
	@echo ""
	@ls -lh $(TARGET) 2>/dev/null || true
	@file $(TARGET) 2>/dev/null || true

# Check compiler availability
check_compiler:
	@echo "Checking compiler: $(CC)"
	@$(CC) --version >/dev/null 2>&1 || { \
	echo "ERROR: $(CC) not found!"; \
	echo "Please install the toolchain or set CC variable."; \
	exit 1; \
	}
	@echo "Compiler found: $(shell $(CC) --version | head -1)"

# Create directories
# (v34.87: $(LR_OBJDIR) - obj/libretro for host builds, obj/libretro-switch
# when platform=switch; every compile rule also mkdirs defensively.)
dirs:
	@echo "Creating directories..."
	@mkdir -p $(LR_OBJDIR)/jvm
	@mkdir -p $(LR_OBJDIR)/midp
	@mkdir -p $(LR_OBJDIR)/render
	@mkdir -p $(LR_OBJDIR)/utils
	@mkdir -p $(LR_OBJDIR)/midi
	@mkdir -p $(LR_OBJDIR)/libretro
	@mkdir -p $(BINDIR)
	@echo "Directories created"

# Compile rules — objects go to obj/libretro/<subdir>/
$(LR_OBJDIR)/jvm/%.o: $(SRCDIR)/jvm/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(LR_OBJDIR)/midp/%.o: $(SRCDIR)/midp/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(LR_OBJDIR)/utils/%.o: $(SRCDIR)/utils/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(LR_OBJDIR)/midi/%.o: $(SRCDIR)/midi/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(LR_OBJDIR)/render/%.o: $(SRCDIR)/render/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(LR_OBJDIR)/libretro/%.o: $(SRCDIR)/libretro/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

# v34.87: Switch core only - vendored aarch64 ucontext (see the switch
# platform block). For other platforms the rule simply never fires.
$(LR_OBJDIR)/switch/%.o: $(SRCDIR)/switch/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c $< -o $@

# v34.69: AMR-NB decoder objects (plain C, CC-compiled)
$(LR_OBJDIR)/amr/%.o: $(SRCDIR)/amr/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) $(AMR_INCS) -c $< -o $@

# Link / archive. v34.86: platform=switch ARCHIVES the core (libretro
# Switch convention: STATIC_LINKING - cores are linked into RetroArch,
# never dlopened; there is nothing to link here).
ifeq ($(platform),switch)
$(TARGET): $(ALL_LIBRETRO_OBJS)
	@echo "  AR  $(TARGET) (static libretro core archive)"
	$(AR) rcs $@ $^
else
$(TARGET): $(ALL_LIBRETRO_OBJS)
	@echo "  LD  $(TARGET)"
	$(CC) $^ -o $@ $(LDFLAGS)
ifeq ($(platform),m17)
	@echo "  STRIP $(TARGET) (release)"
	$(STRIP) --strip-unneeded $@ 2>/dev/null || true
else ifneq ($(STRIP),)
	@echo "  STRIP $(TARGET) (disabled for debug)"
	@# $(STRIP) --strip-unneeded $@ 2>/dev/null || true
endif
	@echo "Linked successfully"
endif # platform=switch

# v34.86: convenience goal - `make switch` = the libretro core for Switch.
# (The MAKECMDGOALS mapping at the top already set platform=switch.)
switch: libretro

# ==============================================
# Standalone application (native only)
# ==============================================
APP_TARGET := $(BINDIR)/j2me-emulator
APP_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/app/%.o,$(ALL_APP_SRCS))

# SDL2 detection (v34.21): pkg-config first; else probe dev headers in the
# common Linux/MSYS2 locations; else build WITHOUT SDL — sdl_graphics.c's
# __has_include chain compiles the backend down to a no-op in that case.
SDL2_PC_OK := $(shell pkg-config --exists sdl2 2>/dev/null && echo 1)
ifeq ($(SDL2_PC_OK),1)
SDL2_LIBS := $(shell pkg-config --libs sdl2)
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2)
else
SDL2_HDR := $(firstword $(wildcard /usr/include/SDL2/SDL.h /usr/local/include/SDL2/SDL.h /mingw32/include/SDL2/SDL.h /mingw64/include/SDL2/SDL.h /ucrt64/include/SDL2/SDL.h))
ifneq ($(SDL2_HDR),)
SDL2_CFLAGS := -I$(patsubst %/,%,$(dir $(SDL2_HDR)))
SDL2_LIBS := -lSDL2
else
SDL2_CFLAGS :=
SDL2_LIBS :=
endif
endif

APP_CFLAGS := -Wall -Wextra -std=c11 -O2 -I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include -D_GNU_SOURCE -MMD -MP $(SDL2_CFLAGS)
APP_LDFLAGS := -lm -pthread $(SDL2_LIBS)
# The app target is documented as a NATIVE build; on a Linux host the default
# `platform` is windows and CC points at the MinGW cross-compiler, so the app
# rules use their own compiler variable (override with: make app APP_CC=...)
APP_CC ?= gcc

# v34.69: AMR-NB decoder in the app build too (plain C — always built).
APP_OBJS += $(patsubst $(SRCDIR)/amr/%.c,$(OBJDIR)/app/amr/%.o,$(AMR_C_SRCS))

app: app-dirs $(APP_TARGET)
	@echo ""
	@echo "============================================"
	@echo "Application built successfully!"
	@echo "============================================"
	@echo "Output: $(APP_TARGET)"
	@ls -lh $(APP_TARGET)

app-dirs:
	@mkdir -p $(OBJDIR)/app/jvm $(OBJDIR)/app/midp $(OBJDIR)/app/render $(OBJDIR)/app/sdl \
	$(OBJDIR)/app/utils $(OBJDIR)/app/midi $(BINDIR)

$(APP_TARGET): $(APP_OBJS)
	$(APP_CC) $^ -o $@ $(APP_LDFLAGS)

$(OBJDIR)/app/%.o: $(SRCDIR)/%.c
	$(APP_CC) $(APP_CFLAGS) -c $< -o $@

$(OBJDIR)/app/amr/%.o: $(SRCDIR)/amr/%.c
	@mkdir -p $(dir $@)
	$(APP_CC) $(APP_CFLAGS) $(AMR_INCS) -c $< -o $@

# ==============================================
# Headless application (no SDL2 required)
# ==============================================
HEADLESS_TARGET := $(BINDIR)/j2me-headless
HEADLESS_SRCS := $(JVM_SRCS) $(MIDP_SRCS) $(UTIL_SRCS) $(MIDI_SRCS) \
	$(SRCDIR)/sdl/sdl_headless.c $(SRCDIR)/main.c
HEADLESS_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/headless/%.o,$(HEADLESS_SRCS))
HEADLESS_CC ?= gcc

HEADLESS_CFLAGS := -Wall -Wextra -std=c11 -O2 -I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include -D_GNU_SOURCE -DJ2ME_DEBUG=0 -DJ2ME_HEADLESS -MMD -MP
HEADLESS_LDFLAGS := -lm -pthread

# v34.69: AMR-NB decoder in the headless build too (plain C — always built).
HEADLESS_OBJS += $(patsubst $(SRCDIR)/amr/%.c,$(OBJDIR)/headless/amr/%.o,$(AMR_C_SRCS))

headless: headless-dirs $(HEADLESS_TARGET)
	@echo ""
	@echo "============================================"
	@echo "Headless application built successfully!"
	@echo "============================================"
	@echo "Output: $(HEADLESS_TARGET)"
	@ls -lh $(HEADLESS_TARGET)

headless-dirs:
	@mkdir -p $(OBJDIR)/headless/jvm $(OBJDIR)/headless/midp $(OBJDIR)/headless/render $(OBJDIR)/headless/sdl \
	$(OBJDIR)/headless/utils $(OBJDIR)/headless/midi $(BINDIR)

$(HEADLESS_TARGET): $(HEADLESS_OBJS)
	$(HEADLESS_CC) $^ -o $@ $(HEADLESS_LDFLAGS)

$(OBJDIR)/headless/%.o: $(SRCDIR)/%.c
	$(HEADLESS_CC) $(HEADLESS_CFLAGS) -c $< -o $@

$(OBJDIR)/headless/amr/%.o: $(SRCDIR)/amr/%.c
	@mkdir -p $(dir $@)
	$(HEADLESS_CC) $(HEADLESS_CFLAGS) $(AMR_INCS) -c $< -o $@

# ==============================================
# v34.86: switchui — Nintendo Switch NRO
# (the full standalone emulator with GUI; was the v34.85 `switch` goal)
# ==============================================
# Requires: devkitPro pacman "switch-dev" group (devkitA64 + libnx +
# general-tools: nacptool + elf2nro in tools/bin) AND the switch-SDL2
# portlib (pacman -S switch-SDL2 -> the $(DEVKITPRO)/portlibs switch tree).
# Build:   make switchui
# Output:  bin/nojme.nro (+ bin/nojme_switch.elf/.nacp)
# Install: copy bin/nojme.nro to sdmc:/switch/j2me/nojme.nro, run via HBmenu.
# (The libretro core for RetroArch is the separate `make switch` goal.)
# The whole section is parsed ONLY when the switchui goal is requested,
# so plain `make` / `make platform=linux` never touch devkitPro paths.
ifneq ($(filter switchui,$(MAKECMDGOALS)),)

ifeq ($(strip $(DEVKITPRO)),)
ifeq ($(OS),Windows_NT)
DEVKITPRO := C:/devkitPro
else
DEVKITPRO := /opt/devkitpro
endif
DEVKITPRO_DEFAULTED_UI = 1
else
DEVKITPRO := $(subst \,/,$(DEVKITPRO))
endif
DEVKITA64 := $(DEVKITPRO)/devkitA64
LIBNX     := $(DEVKITPRO)/libnx
# v34.88 FIX (user report: "implicit declaration of 'sdl_switch_game_begin'"
# at sdl_graphics.c:685 + "'sdl_handle_key_event' defined but not used" at
# :1088 on devkitA64): BOTH symptoms together mean SDL2_AVAILABLE evaluated
# to 0 - the __has_include probes in sdl_graphics.c found NEITHER <SDL2/SDL.h>
# NOR <SDL.h> on the include path. Root cause: devkitPro installs the Switch
# portlibs under $(DEVKITPRO)/portlibs/switch (per-platform tree - the same
# layout libnx's switch_rules exports as PORTLIBS), and this Makefile pointed
# at the shared $(DEVKITPRO)/portlibs root instead. The pkg-config probe
# failed too (no PKG_CONFIG_PATH -> sdl2.pc not found), so the fallback fired
# with the wrong path -> no SDL2 headers -> the whole __SWITCH__ glue section
# (which declares sdl_switch_game_begin via switch/switch_glue.h) compiled
# out while the #ifdef __SWITCH__ call site stayed -> implicit-decl error.
PORTLIBS  := $(DEVKITPRO)/portlibs/switch
UIPREFIX  := $(DEVKITA64)/bin/aarch64-none-elf-

SWITCH_APP_TITLE  ?= J2ME Emulator
SWITCH_APP_AUTHOR ?= nojme
SWITCH_APP_VER    ?= 1.0.0

ifeq ($(DEVKITPRO_DEFAULTED_UI),1)
$(info NOTE: DEVKITPRO is not set - autodetected to $(DEVKITPRO). Override with: make switchui DEVKITPRO=<path>)
endif
ifeq ($(wildcard $(DEVKITA64)/bin/aarch64-none-elf-gcc*),)
$(error devkitA64 toolchain not found at $(DEVKITPRO)/devkitA64. Install the devkitPro "switch-dev" pacman group or point DEVKITPRO at your devkitPro folder, e.g.: make switchui DEVKITPRO=C:/devkitPro)
endif
ifeq ($(wildcard $(LIBNX)/switch.specs),)
$(error libnx not found at $(LIBNX) - install the devkitPro "switch-dev" pacman group)
endif

SWITCH_CC       := $(UIPREFIX)gcc
SWITCH_AR      := $(UIPREFIX)ar
SWITCH_STRIP   := $(UIPREFIX)strip
SWITCH_NACPTOOL := $(DEVKITPRO)/tools/bin/nacptool
SWITCH_ELF2NRO  := $(DEVKITPRO)/tools/bin/elf2nro

# SDL2 discovery (v34.88): devkitPro's canonical flow is pkg-config with
# PKG_CONFIG_PATH pointing into the switch portlibs tree (what libnx's
# switch_rules exports). Candidates in order:
#   $(PORTLIBS)/bin/pkg-config, devkitA64's aarch64-none-elf-pkg-config,
#   the host pkg-config (MSYS2/Linux/macOS devkitPro environments).
# No hit -> hardcoded -I/-L against the canonical portlibs layout.
# A final header probe HARD-FAILS with an actionable message if SDL2 is
# still missing: a silent miss surfaces as the cryptic cascade
# "implicit declaration of 'sdl_switch_game_begin'" (sdl_graphics.c:685) +
# "'sdl_handle_key_event' defined but not used" (:1088) - see v34.88.
SWITCH_PKGC_PATH := $(PORTLIBS)/lib/pkgconfig
SWITCH_PKGC := $(firstword $(wildcard $(PORTLIBS)/bin/pkg-config* $(DEVKITA64)/bin/aarch64-none-elf-pkg-config*))
ifeq ($(strip $(SWITCH_PKGC)),)
SWITCH_PKGC := pkg-config
endif
SWITCH_SDL_CFLAGS := $(shell PKG_CONFIG_PATH="$(SWITCH_PKGC_PATH)" $(SWITCH_PKGC) --cflags sdl2 2>/dev/null)
SWITCH_SDL_LIBS   := $(shell PKG_CONFIG_PATH="$(SWITCH_PKGC_PATH)" $(SWITCH_PKGC) --libs sdl2 2>/dev/null)
ifeq ($(strip $(SWITCH_SDL_CFLAGS)),)
SWITCH_SDL_CFLAGS := -I$(PORTLIBS)/include -I$(PORTLIBS)/include/SDL2
SWITCH_SDL_LIBS   := -L$(PORTLIBS)/lib -lSDL2
endif
# Header probe: either the canonical portlibs layout, or whatever include
# dir pkg-config reported. SDL2 headers MUST be reachable - sdl_graphics.c
# compiles its whole __SWITCH__ section out otherwise.
SWITCH_SDL_HDR := $(firstword $(wildcard $(PORTLIBS)/include/SDL2/SDL.h $(PORTLIBS)/include/SDL.h))
ifeq ($(SWITCH_SDL_HDR),)
SWITCH_SDL_I := $(patsubst -I%,%,$(firstword $(filter -I%,$(SWITCH_SDL_CFLAGS))))
ifeq ($(strip $(SWITCH_SDL_I)),)
SWITCH_SDL_I := $(PORTLIBS)/include
endif
ifeq ($(shell test -f "$(SWITCH_SDL_I)/SDL.h" -o -f "$(SWITCH_SDL_I)/SDL2/SDL.h" 2>/dev/null && echo 1),)
$(error switch-SDL2 portlib not found under $(PORTLIBS). Install it with: sudo dkp-pacman -S switch-SDL2 (then retry: make switchui). If it is installed elsewhere, override PORTLIBS: make switchui PORTLIBS=<path-to>/portlibs/switch)
endif
endif

# Same flag family as the core build (GCC 14 paranoia, -O2, no LTO,
# -mtp=soft -fPIE) plus the real SDL2 headers: the NRO uses the actual
# switch-SDL2 for window/input/audio.
SWITCH_CFLAGS := -Wall -Wextra -std=c11 \
-I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include \
-D__SWITCH__ -D_GNU_SOURCE -DJ2ME_DEBUG=0 \
-O2 -MMD -MP \
-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
-fno-strict-aliasing -fno-strict-overflow \
-fno-delete-null-pointer-checks -fno-ipa-pure-const \
-ffunction-sections -fdata-sections \
-fno-asynchronous-unwind-tables -fno-ident \
-I$(LIBNX)/include \
$(SWITCH_SDL_CFLAGS)

# Libraries are linked AFTER the objects (static-archive order).
# (v34.84 BUG: -lSDL2 sat in LDFLAGS, -lnx/-lpthread absent.)
SWITCH_LIBS := $(SWITCH_SDL_LIBS) -lnx -lm -lpthread
SWITCH_LDFLAGS := -specs=$(LIBNX)/switch.specs \
-Wl,--as-needed -Wl,--gc-sections -Wl,--build-id=none

SWITCH_SRCS := $(ALL_APP_SRCS) \
	$(SRCDIR)/switch/switch_ui.c \
	$(SRCDIR)/switch/switch_font.c \
	$(SRCDIR)/switch/switch_scaling.c \
	$(SRCDIR)/switch/switch_ucontext.c \
	$(SRCDIR)/switch/switch_trace.c \
	$(SRCDIR)/switch/pathguard.c

SWITCH_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/switch/%.o,$(SWITCH_SRCS))
SWITCH_OBJS += $(patsubst $(SRCDIR)/amr/%.c,$(OBJDIR)/switch/amr/%.o,$(AMR_C_SRCS))

SWITCH_ELF := $(BINDIR)/nojme_switch.elf
# v36.18: stripped-копия эльфа — именно она скармливается elf2nro (полный
# эльф сохраняет символы для addr2line-разбора крахов).
SWITCH_ELF_STRIPPED := $(BINDIR)/nojme_switch_stripped.elf
SWITCH_NACP := $(BINDIR)/nojme_switch.nacp
SWITCH_NRO := $(BINDIR)/nojme.nro

switchui: switchui-dirs $(SWITCH_NRO)
	@echo ""
	@echo "============================================"
	@echo "Switch NRO (standalone GUI emulator) built!"
	@echo "============================================"
	@echo "Output:  $(SWITCH_NRO)"
	@echo "Install: copy to sdmc:/switch/j2me/ and run via HBmenu"
	@echo "(for the RetroArch libretro core run: make switch)"
	@ls -lh $(SWITCH_NRO) 2>/dev/null || echo "(size: $(shell wc -c < $(SWITCH_NRO) 2>/dev/null) bytes)"

switchui-dirs:
	@mkdir -p $(OBJDIR)/switch/jvm $(OBJDIR)/switch/midp $(OBJDIR)/switch/render \
	$(OBJDIR)/switch/utils $(OBJDIR)/switch/midi $(OBJDIR)/switch/sdl \
	$(OBJDIR)/switch/switch $(BINDIR)

$(SWITCH_ELF): $(SWITCH_OBJS)
	@echo "  LD  $(SWITCH_ELF)"
	$(SWITCH_CC) $^ -o $@ $(SWITCH_LDFLAGS) $(SWITCH_LIBS)

$(SWITCH_NACP):
	@echo "  NACP $(SWITCH_NACP)"
	$(SWITCH_NACPTOOL) --create "$(SWITCH_APP_TITLE)" "$(SWITCH_APP_AUTHOR)" "$(SWITCH_APP_VER)" $@

$(SWITCH_NRO): $(SWITCH_ELF) $(SWITCH_NACP) switch/icon.jpg
	@echo "  NRO  $(SWITCH_NRO)"
	@# v36.18: УМЕНЬШЕНИЕ NRO — elf2nro получает СТРИПНУТУЮ копию эльфа
	@# (--strip-unneeded: без symtab/strtab/debug-секций). Полный эльф
	@# остаётся рядом для addr2line-разбора крахов. GNU strip -o пишет
	@# в новый файл, исходный эльф не трогает.
	$(SWITCH_STRIP) --strip-unneeded -o $(SWITCH_ELF_STRIPPED) $(SWITCH_ELF)
	$(SWITCH_ELF2NRO) $(SWITCH_ELF_STRIPPED) $@ --icon=switch/icon.jpg --nacp=$(SWITCH_NACP)
	@ls -lh $@ $(SWITCH_ELF) $(SWITCH_ELF_STRIPPED) 2>/dev/null || true

$(OBJDIR)/switch/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(SWITCH_CC) $(SWITCH_CFLAGS) -c $< -o $@

$(OBJDIR)/switch/amr/%.o: $(SRCDIR)/amr/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(SWITCH_CC) $(SWITCH_CFLAGS) $(AMR_INCS) -c $< -o $@

endif # switchui goal guard

.PHONY: switchui switchui-dirs

# ==============================================
# switchui-verify - HOST syntax check of the Switch frontend (v34.86)
# ==============================================
# Same sources, same -D__SWITCH__ - but compiled with the HOST compiler
# and HOST SDL2 (Linux / MSYS2 with a native gcc). This is a sandbox
# verification target ONLY: it does NOT produce the Switch NRO.
# (Was the v34.85 `switchui` goal - renamed because switchui now means
# the real NRO build.)
SWITCHUI_TARGET := $(BINDIR)/switchui-verify
SWITCHUI_SRCS := $(ALL_APP_SRCS) \
	$(SRCDIR)/switch/switch_ui.c \
	$(SRCDIR)/switch/switch_font.c \
	$(SRCDIR)/switch/switch_scaling.c \
	$(SRCDIR)/switch/switch_trace.c \
	$(SRCDIR)/switch/pathguard.c
SWITCHUI_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/switchui/%.o,$(SWITCHUI_SRCS))
SWITCHUI_OBJS += $(patsubst $(SRCDIR)/amr/%.c,$(OBJDIR)/switchui/amr/%.o,$(AMR_C_SRCS))
SWITCHUI_CC ?= gcc
# SDL2 discovery: pkg-config, else override via env:
#   SWITCHUI_SDL_CFLAGS / SWITCHUI_SDL_LIBS
SWITCHUI_SDL_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
SWITCHUI_SDL_LIBS ?= $(shell pkg-config --libs sdl2 2>/dev/null || echo -lSDL2)

SWITCHUI_CFLAGS := -Wall -Wextra -std=c11 -O2 \
	-I$(INCDIR) -I$(SRCDIR) -I$(SRCDIR)/include \
	-D__SWITCH__ -D_GNU_SOURCE -DJ2ME_DEBUG=0 \
	$(SWITCHUI_SDL_CFLAGS) -MMD -MP

SWITCHUI_LDFLAGS := -lm -pthread $(SWITCHUI_SDL_LIBS)

# Host-compiler probe (parse-time, silent; one --version call - `make
# switch` never evaluates any switchui recipe, so it costs nothing there).
SWITCHUI_CC_OK := $(shell $(SWITCHUI_CC) --version >/dev/null 2>&1 && echo 1)

ifeq ($(SWITCHUI_CC_OK),1)
switchui-verify: switchui-verify-dirs $(SWITCHUI_TARGET)
	@echo ""
	@echo "Switch-frontend verification binary built: $(SWITCHUI_TARGET)"
	@echo "(host syntax check - for the real Switch NRO run: make switchui)"

switchui-verify-dirs:
	@mkdir -p $(OBJDIR)/switchui/jvm $(OBJDIR)/switchui/midp $(OBJDIR)/switchui/render \
	$(OBJDIR)/switchui/utils $(OBJDIR)/switchui/midi $(OBJDIR)/switchui/sdl \
	$(OBJDIR)/switchui/switch $(BINDIR)

$(SWITCHUI_TARGET): $(SWITCHUI_OBJS)
	$(SWITCHUI_CC) $^ -o $@ $(SWITCHUI_LDFLAGS)

$(OBJDIR)/switchui/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(SWITCHUI_CC) $(SWITCHUI_CFLAGS) -c $< -o $@

$(OBJDIR)/switchui/amr/%.o: $(SRCDIR)/amr/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	$(SWITCHUI_CC) $(SWITCHUI_CFLAGS) $(AMR_INCS) -c $< -o $@

else
# No host compiler in this environment (e.g. plain cmd.exe + devkitPro
# make) - explain instead of failing on a missing `gcc`.
switchui-verify:
	@echo ""
	@echo "==================================================================="
	@echo " NOTE: 'switchui-verify' is a HOST syntax check: it needs a host C"
	@echo " compiler ($(SWITCHUI_CC)) plus host SDL2 dev headers (Linux or"
	@echo " MSYS2), which this environment does not have. This is NOT how"
	@echo " the emulator is built anyway."
	@echo ""
	@echo " For the Nintendo Switch builds run one of:"
	@echo "     make switch      (libretro core, bin/nojme_libretro_libnx.a)"
	@echo "     make switchui     (standalone GUI emulator, bin/nojme.nro)"
	@echo " (needs devkitPro: devkitA64 + libnx (+ switch-SDL2 for switchui);"
	@echo "  full instructions in README-СБОРКА.txt)"
	@echo "==================================================================="
	@exit 1
endif

.PHONY: switchui-verify switchui-verify-dirs

# ==============================================
# Utility targets
# ==============================================

# Clean all object and output files
clean:
	@echo "Cleaning $(OBJDIR)/ $(BINDIR)/"
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "Cleaned"

# Check source files
check:
	@echo "=== Checking source files ==="
	@for src in $(ALL_LIBRETRO_SRCS); do \
	if [ -f "$$src" ]; then \
	echo "  OK  $$src"; \
	else \
	echo "  MISSING  $$src"; \
	exit 1; \
	fi \
	done
	@echo "=== All source files present ==="
	@echo "Total: $(words $(ALL_LIBRETRO_SRCS)) files"

# Show build info
info:
	@echo "=== Build Configuration ==="
	@echo "Platform:       $(PLATFORM_NAME)"
	@echo "Target:         $(TARGET)"
	@echo "Compiler:       $(CC)"
	@echo "Arch:           $(ARCH)"
	@echo "Obj directory:  $(OBJDIR)/"
	@echo "MinGW native:   $(IS_MINGW_NATIVE)"
	@echo "MSYSTEM:        $(MSYSTEM)"
	@echo ""
	@echo "=== Compiler Flags ==="
	@echo "CFLAGS:  $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo ""
	@echo "=== Source files ==="
	@echo "JVM:      $(words $(JVM_SRCS)) files"
	@echo "MIDP:     $(words $(MIDP_SRCS)) files"
	@echo "Utils:    $(words $(UTIL_SRCS)) files"
	@echo "MIDI:     $(words $(MIDI_SRCS)) files"
	@echo "Libretro: $(words $(LIBRETRO_SRCS)) files"
	@echo "AMR (C):  $(words $(AMR_C_SRCS)) files"
	@echo "Total:    $(words $(ALL_LIBRETRO_SRCS)) + $(words $(AMR_C_SRCS)) C files"

# Header dependency tracking (auto-generated .d files from -MMD).
# v34.73 CRITICAL FIX (two-tier self-heal of stale dependency files).
# Symptom fixed: unpacking the archive over an old build tree kept v34.71-era
# C++ artifacts in obj/, and the old blanket `find`-include fed
# obj/libretro/amr/amr_nb_dec.d (dependency edge "....o: src/amr/amr_nb_dec.cpp")
# to make, aborting with "No rule to make target 'src/amr/amr_nb_dec.cpp'".
#
# Tier 1 (name filter): include .d ONLY for objects of the CURRENT build
# lists; orphaned .d/.o (e.g. obj/libretro/amr/sp_dec.d) are deleted.
# Tier 2 (content check, scripts/stale_dep_check.sh): the C++ wrapper .d has
# the SAME NAME as the C wrapper's (obj/.../amr_nb_dec.d) and passes tier 1 -
# so every existing .d is scanned and dropped when it references a .cpp
# source (the tree has none) or when its FIRST (primary) prerequisite no
# longer exists on disk (with -MMD that is the compiled source; -MP only
# adds phony rules for headers, never for the source). Continuation-safe:
# GCC wraps long first lines with backslash-newline right after "target:\"
# (vendored AMR paths) - the script joins them before parsing.
# Result: overlay unpacks self-heal, no manual `make clean` required.
# v34.86: SWITCH_OBJS lives inside the switchui goal guard below and
# SWITCHUI_OBJS inside the host-verify block - both expand to empty for
# every other invocation (libretro/app/headless/switch-core), which keeps
# the tier-1 self-heal correct without touching foreign artifacts.
BUILD_OBJS := $(ALL_LIBRETRO_OBJS) $(APP_OBJS) $(HEADLESS_OBJS) $(SWITCH_OBJS) $(SWITCHUI_OBJS)
BUILD_DEPS := $(BUILD_OBJS:.o=.d)
FOUND_DEPS := $(wildcard $(OBJDIR)/libretro/*.d $(OBJDIR)/libretro/*/*.d \
                        $(OBJDIR)/app/*.d $(OBJDIR)/app/*/*.d \
                        $(OBJDIR)/headless/*.d $(OBJDIR)/headless/*/*.d)
STALE_DEPS := $(filter-out $(BUILD_DEPS),$(FOUND_DEPS))
ifneq ($(STALE_DEPS),)
$(shell rm -f $(STALE_DEPS) $(STALE_DEPS:.d=.o) 2>/dev/null)
$(info NOTE: removed $(words $(STALE_DEPS)) orphaned .d/.o file(s) left by an older build (tier 1 self-heal).)
endif
STALE_CONTENT := $(shell sh scripts/stale_dep_check.sh $(OBJDIR) 2>/dev/null | sort -u)
ifneq ($(STALE_CONTENT),)
$(shell rm -f $(STALE_CONTENT) $(STALE_CONTENT:.d=.o) 2>/dev/null)
$(info NOTE: removed $(words $(STALE_CONTENT)) .d/.o file(s) referencing a removed source (tier 2 self-heal, e.g. pre-v34.69 C++ AMR wrapper).)
endif
-include $(BUILD_DEPS)
