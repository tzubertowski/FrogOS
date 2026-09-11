# Directory where the Makefile lives
MAKEFILE_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# Output directories
OUTDIR      ?= $(MAKEFILE_DIR)/out
APP_OUTDIR  ?= $(OUTDIR)/app
HB_OUTDIR   ?= $(OUTDIR)/harfbuzz
SB_OUTDIR   ?= $(OUTDIR)/sheenbidi

# Target definitions
TARGET      := $(OUTDIR)/frogui_libretro.so

# Toolchain and Paths Configuration
MIPS          ?= $(HOME)/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin/mips-mti-linux-gnu-
SYSROOT       ?= $(HOME)/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot
HARFBUZZ_DIR  := $(MAKEFILE_DIR)/external/harfbuzz
SHEENBIDI_DIR := $(MAKEFILE_DIR)/external/SheenBidi

CC := $(MIPS)gcc

# Sub-module object files
HB_SRC := $(HARFBUZZ_DIR)/src/harfbuzz.cc
HB_OBJ := $(HB_OUTDIR)/harfbuzz.lo

SB_SRC := $(SHEENBIDI_DIR)/Source/SheenBidi.c
SB_OBJ := $(SB_OUTDIR)/SheenBidi.lo

# License files target
OUT_LICENSES := $(OUTDIR)/frogui_libretro.harfbuzz.LICENSE \
                $(OUTDIR)/frogui_libretro.sheenbidi.LICENSE

# Explicit Object List in APP_OUTDIR
OBJS := $(APP_OUTDIR)/frogui_libretro.lo \
        $(APP_OUTDIR)/render.lo \
        $(APP_OUTDIR)/font.lo \
        $(APP_OUTDIR)/recent_games.lo \
        $(APP_OUTDIR)/theme.lo \
        $(APP_OUTDIR)/favorites.lo \
        $(APP_OUTDIR)/banner.lo \
        $(APP_OUTDIR)/backlight.lo \
        $(APP_OUTDIR)/input.lo \
        $(APP_OUTDIR)/core_override.lo \
        $(APP_OUTDIR)/ext_filter.lo \
        $(APP_OUTDIR)/i18n.lo \
        $(SB_OBJ) \
        $(HB_OBJ)

# Compiler & Linker Flags
INCLUDES := -I. \
            -I./common \
            -I$(SYSROOT)/usr/include \
            -I$(SYSROOT)/usr/include/freetype2 \
            -I$(HARFBUZZ_DIR)/src \
            -I$(SHEENBIDI_DIR) \
            -I$(SHEENBIDI_DIR)/Headers \
            -I$(SHEENBIDI_DIR)/Source

CFLAGS   := -mips32r2 -march=mips32r2 -mtune=24kc -mfp32 -mhard-float -mlong-calls -EL \
            --sysroot=$(SYSROOT) -fPIC -G0 -Wall $(INCLUDES) -Ofast \
            -ffunction-sections -fdata-sections \
            -DPLATFORM_SF3000 -DNDEBUG -D__LIBRETRO__

# Size-optimized HarfBuzz flags (Thread-safety preserved)
HB_CFLAGS := $(CFLAGS) \
            -Os \
            -fno-exceptions \
            -fno-rtti \
            -DHB_MINI \
            -DHB_LEAN \
            -DHB_NO_CDECL \
            -DHB_NO_SETLOCALE \
            -DHB_NO_OT_FONT \
            -DHB_NO_FALLBACK_SHAPE \
            -DHAVE_FREETYPE

LDFLAGS  := -mips32r2 -mhard-float -mfp32 -EL -fPIC -shared -nostdlib -Wl,--no-undefined \
            -Wl,--gc-sections \
            --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/lib

LDLIBS   := -lfreetype -lm -lc -ldl -lpthread -lstdc++ -lgcc

# Submodule initialization rule
init_submodules:
	@if [ ! -f "$(HB_SRC)" ] || [ ! -f "$(SB_SRC)" ]; then \
		echo "Initializing git submodules..."; \
		git submodule update --init --recursive; \
	fi

# Default Rule
all: init_submodules $(TARGET) $(OUT_LICENSES)

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(APP_OUTDIR):
	mkdir -p $(APP_OUTDIR)

$(HB_OUTDIR):
	mkdir -p $(HB_OUTDIR)

$(SB_OUTDIR):
	mkdir -p $(SB_OUTDIR)

# License Copy Rules
$(OUTDIR)/frogui_libretro.harfbuzz.LICENSE: | $(OUTDIR) init_submodules
	@echo "Copying HarfBuzz License..."
	@if [ -f "$(HARFBUZZ_DIR)/COPYING" ]; then \
		cp "$(HARFBUZZ_DIR)/COPYING" $@; \
	else \
		echo "HarfBuzz license file not found!" && exit 1; \
	fi

$(OUTDIR)/frogui_libretro.sheenbidi.LICENSE: | $(OUTDIR) init_submodules
	@echo "Copying SheenBidi License..."
	@if [ -f "$(SHEENBIDI_DIR)/LICENSE" ]; then \
		cp "$(SHEENBIDI_DIR)/LICENSE" $@; \
	else \
		echo "SheenBidi license file not found!" && exit 1; \
	fi

# Linker Step
$(TARGET): $(OUTDIR) $(APP_OUTDIR) $(HB_OUTDIR) $(SB_OUTDIR) $(OBJS)
	@echo "Linking $@..."
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@
	@ls -la $@
	@echo "Build successful!"

# Application Compile Rules
$(APP_OUTDIR)/%.lo: %.c | $(APP_OUTDIR) init_submodules
	@echo "CC $<"
	$(CC) $(CFLAGS) -c -o $@ $<

$(APP_OUTDIR)/%.lo: common/%.c | $(APP_OUTDIR) init_submodules
	@echo "CC $<"
	$(CC) $(CFLAGS) -c -o $@ $<

# SheenBidi Unity Compile Rule
$(SB_OBJ): $(SB_SRC) | $(SB_OUTDIR) init_submodules
	@echo "CC Unity SheenBidi $<"
	$(CC) $(CFLAGS) -DSB_CONFIG_UNITY -w -c -o $@ $<

# HarfBuzz Bundle Compile Rule
$(HB_OBJ): $(HB_SRC) | $(HB_OUTDIR) init_submodules
	@echo "CXX (HarfBuzz) $<"
	$(CC) $(HB_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUTDIR)

.PHONY: all clean init_submodules
