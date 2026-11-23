ifeq ($(strip $(DEVKITPRO)),)
$(error "Define DEVKITPRO en tus variables de entorno.")
endif

include $(DEVKITPRO)/wups/share/wups_rules

TARGET   := WiiU_Transfer_Updater
BUILD    := build
SOURCES  := source

ARCH     := -mcpu=750 -meabi -mhard-float
CXXFLAGS := -g -O2 -Wall $(ARCH) -std=gnu++17 -ffunction-sections -fdata-sections
LDFLAGS  := -g $(ARCH) $(WUPSSPECS)
LIBS     := -lwups -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lbrotlidec -lbrotlicommon -lz -lwut -lm

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(CURDIR)/source
export DEPSDIR  := $(CURDIR)/$(BUILD)
export INCLUDE  := -I$(WUPS_ROOT)/include -I$(WUT_ROOT)/include -I$(DEVKITPRO)/portlibs/wiiu/include -I$(DEVKITPRO)/portlibs/ppc/include
export LIBPATHS := -L$(WUPS_ROOT)/lib -L$(WUT_ROOT)/lib -L$(DEVKITPRO)/portlibs/wiiu/lib -L$(DEVKITPRO)/portlibs/ppc/lib
export OFILES   := main.o
export LD       := $(CXX)

.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	rm -fr $(BUILD) $(TARGET).elf $(TARGET).wps
else
all: $(OUTPUT).wps
endif
