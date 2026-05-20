#---------------------------------------------------------------------------------
# dk3dtest_libretro — minimal libretro core rendering a deko3d cube,
# handed to RetroArch frontend via retro_hw_render_interface_deko3d.
#
# Targets:
#   core    (default) build static archive libdk3dtest_libretro.a
#   bundle  stage core into a local RetroArch clone and produce
#           dk3dtest_libretro_libnx.nro (per-core RA bundle)
#   clean   wipe build artifacts
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment.")
endif

TOPDIR  := $(CURDIR)
TARGET  := dk3dtest_libretro
BUILD   := build
SOURCES := source
SHADERS := shaders
INCLUDES := include

#---------------------------------------------------------------------------------
# Toolchain (devkitA64)
#---------------------------------------------------------------------------------
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
CC     := $(PREFIX)gcc
CXX    := $(PREFIX)g++
AR     := $(PREFIX)ar
UAM    := $(DEVKITPRO)/tools/bin/uam

LIBNX_INC   := $(DEVKITPRO)/libnx/include
DEKO3D_INC  := $(DEVKITPRO)/portlibs/switch/include
LIBRETRO_INC := $(TOPDIR)/include

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
DEFINES := -D__SWITCH__ -DHAVE_DEKO3D
INCLUDE := -I$(LIBNX_INC) -I$(DEKO3D_INC) -I$(LIBRETRO_INC) -I$(TOPDIR)/$(INCLUDES) -I$(TOPDIR)/../include

CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDE)
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

#---------------------------------------------------------------------------------
# Sources / objects
#---------------------------------------------------------------------------------
CFILES   := $(notdir $(wildcard $(SOURCES)/*.c))
COBJS    := $(addprefix $(BUILD)/,$(CFILES:.c=.o))

# Shaders: GLSL → DKSH (precompiled by uam) → .o (embedded via .incbin)
VSH_GLSL := $(wildcard $(SHADERS)/*_vsh.glsl)
FSH_GLSL := $(wildcard $(SHADERS)/*_fsh.glsl)
VSH_DKSH := $(patsubst $(SHADERS)/%.glsl,$(BUILD)/%.dksh,$(VSH_GLSL))
FSH_DKSH := $(patsubst $(SHADERS)/%.glsl,$(BUILD)/%.dksh,$(FSH_GLSL))
SHADER_DKSH := $(VSH_DKSH) $(FSH_DKSH)
SHADER_OBJS := $(SHADER_DKSH:.dksh=_dksh.o)

OBJS := $(COBJS) $(SHADER_OBJS)

OUT_LIB := $(TARGET).a

#---------------------------------------------------------------------------------
.PHONY: all core bundle clean

all: core
core: $(OUT_LIB)

$(OUT_LIB): $(OBJS)
	@echo "==> ar $@"
	@$(AR) rcs $@ $^

$(BUILD)/%.o: $(SOURCES)/%.c | $(BUILD)
	@echo "==> cc $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

# uam: glsl → dksh (vert/frag picked by filename suffix)
$(BUILD)/%_vsh.dksh: $(SHADERS)/%_vsh.glsl | $(BUILD)
	@echo "==> uam vert $<"
	@$(UAM) -s vert -o $@ $<

$(BUILD)/%_fsh.dksh: $(SHADERS)/%_fsh.glsl | $(BUILD)
	@echo "==> uam frag $<"
	@$(UAM) -s frag -o $@ $<

# Embed DKSH binary into an .o via objcopy as a raw blob.
# Symbol naming follows libnx convention: _binary_<path>_start/_end/_size
# with non-alnum chars → '_'. We expose stable aliases in C via extern decls.
$(BUILD)/%_dksh.o: $(BUILD)/%.dksh
	@echo "==> objcopy $<"
	@cd $(BUILD) && $(PREFIX)objcopy -I binary -O elf64-littleaarch64 -B aarch64 \
		--redefine-sym _binary_$(notdir $*)_dksh_start=$(notdir $*)_dksh \
		--redefine-sym _binary_$(notdir $*)_dksh_end=$(notdir $*)_dksh_end \
		--redefine-sym _binary_$(notdir $*)_dksh_size=$(notdir $*)_dksh_size \
		$(notdir $*).dksh $(notdir $*)_dksh.o

$(BUILD):
	@mkdir -p $@

#---------------------------------------------------------------------------------
# bundle: stage .a into RetroArch, patch Makefile.libnx, build NRO
#---------------------------------------------------------------------------------
RA_CACHE := $(TOPDIR)/.cache/RetroArch
RA_REPO  ?= https://github.com/HueponiK/RetroArch.git
RA_REF   ?= deko3d-driver
NRO_OUT  := $(TARGET)_libnx.nro

bundle: $(NRO_OUT)

$(NRO_OUT): $(OUT_LIB)
	@if [ ! -d $(RA_CACHE) ]; then \
		echo "==> Cloning RetroArch ($(RA_REF))"; \
		git clone --depth 1 -b $(RA_REF) $(RA_REPO) $(RA_CACHE); \
	fi
	@echo "==> Staging $(OUT_LIB) as libretro_libnx.a"
	@cp $(OUT_LIB) $(RA_CACHE)/libretro_libnx.a
	@rm -f $(RA_CACHE)/retroarch_switch.elf $(RA_CACHE)/retroarch_switch.nro
	@git -C $(RA_CACHE) checkout -- Makefile.libnx 2>/dev/null || true
	@echo "==> Patching Makefile.libnx (app identity)"
	@sed -i 's/^APP_TITLE := .*/APP_TITLE := dk3d-RetroArch/' $(RA_CACHE)/Makefile.libnx
	@sed -i 's/^APP_AUTHOR := .*/APP_AUTHOR := dk3dtest/' $(RA_CACHE)/Makefile.libnx
	@# HAVE_STATIC_DUMMY= (empty) so Makefile.libnx appends libretro_libnx.a
	@# to OFILES without activating dynamic_dummy.c's competing retro_* stubs.
	@echo "==> Building RetroArch NRO"
	@$(MAKE) -C $(RA_CACHE) -f Makefile.libnx -j$(shell nproc) HAVE_STATIC_DUMMY=
	@cp $(RA_CACHE)/retroarch_switch.nro $@
	@cp $(RA_CACHE)/retroarch_switch.elf $(TARGET).elf
	@echo ""
	@echo "Built artifacts:"
	@echo "  NRO  : $@"
	@echo "  ELF  : $(TARGET).elf"
	@echo "  Core : $(OUT_LIB)"

clean:
	rm -rf $(BUILD) $(OUT_LIB) $(NRO_OUT) $(TARGET).elf
