# Makefile base para módulo ARM IOSU de Wii U Transfer (requiere devkitARM)
ifeq ($(strip $(DEVKITARM)),)
$(error "Define DEVKITARM en tus variables de entorno.")
endif

include $(DEVKITARM)/base_rules

TARGET  := wiiu_transfer_iosu
CFLAGS  := -mcpu=arm926ej-s -mthumb -O2 -Wall -I../protocol
OBJS    := source/netfsa_iosu.o

all: $(TARGET).elf

$(TARGET).elf: $(OBJS)
	$(CC) -nostdlib -Wl,-Ttext=0x05070000 $^ -o $@

clean:
	rm -f $(OBJS) $(TARGET).elf
