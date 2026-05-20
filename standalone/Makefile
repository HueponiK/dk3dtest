#---------------------------------------------------------------------------------
# dk3dtest_standalone — barebones deko3d spinning cube (offscreen → blit → present)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET    := dk3dtest_standalone
BUILD     := build
SOURCES   := source
INCLUDES  := include
ROMFS     := romfs

APP_TITLE   := dk3dtest cube
APP_AUTHOR  := dk3dtest
APP_VERSION := 0.1.0

#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# -ldeko3dd for the validation/debug build (per Primer.md: catches RaiseError + routes through cbDebug).
LIBS    := -ldeko3dd -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_BIN :=
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	-I$(CURDIR)/$(BUILD) \
	-I$(DEVKITPRO)/libnx/include \
	-I$(CURDIR)/../include

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# Shader compilation rules: shaders/*.glsl → romfs/shaders/*.dksh via uam.
SHADER_DIR     := $(CURDIR)/shaders
SHADER_OUT_DIR := $(CURDIR)/romfs/shaders
VSH_SRC := $(wildcard $(SHADER_DIR)/*_vsh.glsl)
FSH_SRC := $(wildcard $(SHADER_DIR)/*_fsh.glsl)
SHADERS := $(VSH_SRC:$(SHADER_DIR)/%_vsh.glsl=$(SHADER_OUT_DIR)/%_vsh.dksh) \
           $(FSH_SRC:$(SHADER_DIR)/%_fsh.glsl=$(SHADER_OUT_DIR)/%_fsh.dksh)

# switch_rules' %.nro target is bare `elf2nro $< $@ $(NROFLAGS)` — it does
# NOT automatically pass --icon or --nacp. Without these, elf2nro produces
# an NRO with no asset section, which Ryujinx's NroAssetSection parser
# trips over (NullReferenceException at load time). Pass them explicitly.
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp
ifneq ($(strip $(ROMFS)),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) shaders clean all

all: shaders $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@[ -d $@ ] || mkdir -p $@

$(SHADER_OUT_DIR):
	@mkdir -p $@

shaders: $(SHADER_OUT_DIR) $(SHADERS)

$(SHADER_OUT_DIR)/%_vsh.dksh: $(SHADER_DIR)/%_vsh.glsl | $(SHADER_OUT_DIR)
	@echo "[uam] $<"
	@uam -s vert -o $@ $<

$(SHADER_OUT_DIR)/%_fsh.dksh: $(SHADER_DIR)/%_fsh.glsl | $(SHADER_OUT_DIR)
	@echo "[uam] $<"
	@uam -s frag -o $@ $<

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(SHADER_OUT_DIR)

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
