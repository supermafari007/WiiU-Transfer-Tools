ifeq ($(strip $(DEVKITPRO)),)
$(error "Define DEVKITPRO en tus variables de entorno.")
endif

include $(DEVKITPRO)/wut/share/wut_rules

TARGET        := WiiU_Transfer
BUILD         := build
SOURCES       := source
INCLUDES      := ../protocol

APP_NAME      := Wii U Transfer Tools
APP_SHORTNAME := Wii U Transfer Tools
APP_AUTHOR    := El Diuca
APP_ICON      := $(CURDIR)/assets/icon.png
APP_TV_SPLASH := $(CURDIR)/assets/tv.png
APP_DRC_SPLASH:= $(CURDIR)/assets/drc.png
WUHB_EXTRA    := --content=$(CURDIR)/content

ARCH     := -mcpu=750 -meabi -mhard-float
CXXFLAGS := -g -O2 -Wall $(ARCH) $(DEFINES) -std=gnu++17 -ffunction-sections -fdata-sections
LDFLAGS  := -g $(ARCH) $(RPXSPECS)
LIBS     := -lwut -lm

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(CURDIR)/source
export DEPSDIR  := $(CURDIR)/$(BUILD)
export INCLUDE  := -I$(CURDIR)/../protocol -I$(WUT_ROOT)/include
export LIBPATHS := -L$(WUT_ROOT)/lib
export OFILES   := main.o
export LD       := $(CXX)

.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	rm -fr $(BUILD) $(TARGET).rpx $(TARGET).wuhb $(TARGET).elf
else
all: $(OUTPUT).wuhb
endif
