#
# Makefile - RT-950 Pro custom firmware build
#
# Target: Artery AT32F403A (ARM Cortex-M4F @ 240 MHz)
# Toolchain: arm-none-eabi-gcc
#

# -- Toolchain ------------------------------------------------------------
PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
AS      = $(PREFIX)gcc
LD      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
OBJDUMP = $(PREFIX)objdump
SIZE    = $(PREFIX)size

# -- Project paths --------------------------------------------------------
BUILD_DIR = build
SRC_DIR   = src
INC_DIR   = include

# -- Source files ---------------------------------------------------------
SRCS = \
	$(SRC_DIR)/startup.c \
	$(SRC_DIR)/system.c \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/drivers/gpio.c \
	$(SRC_DIR)/drivers/lcd.c \
	$(SRC_DIR)/drivers/uart.c \
	$(SRC_DIR)/drivers/spi.c \
	$(SRC_DIR)/drivers/dma.c \
	$(SRC_DIR)/drivers/bk4829.c \
	$(SRC_DIR)/drivers/adc.c \
	$(SRC_DIR)/drivers/dac_audio.c \
	$(SRC_DIR)/drivers/timer.c \
	$(SRC_DIR)/drivers/si4732.c \
	$(SRC_DIR)/app/display.c \
	$(SRC_DIR)/app/font.c \
	$(SRC_DIR)/app/splash.c \
	$(SRC_DIR)/app/power.c \
	$(SRC_DIR)/app/gps.c \
	$(SRC_DIR)/app/radio.c \
	$(SRC_DIR)/app/vfo.c \
	$(SRC_DIR)/app/keypad.c \
	$(SRC_DIR)/app/encoder.c \
	$(SRC_DIR)/app/menu.c \
	$(SRC_DIR)/app/freq_entry.c \
	$(SRC_DIR)/app/aprs.c \
	$(SRC_DIR)/app/dtmf.c \
	$(SRC_DIR)/app/audio.c \
	$(SRC_DIR)/app/text_input.c \
	$(SRC_DIR)/app/dtmf_contacts.c \
	$(SRC_DIR)/app/zone_browser.c \
	$(SRC_DIR)/app/bluetooth.c \
	$(SRC_DIR)/app/noaa.c \
	$(SRC_DIR)/app/crossband.c \
	$(SRC_DIR)/app/am_radio.c \
	$(SRC_DIR)/drivers/flash_layout.c \
	$(SRC_DIR)/drivers/flash_wearleveling.c \
	$(SRC_DIR)/drivers/flash_xor.c \
	$(SRC_DIR)/drivers/flash_crc.c \
	$(SRC_DIR)/drivers/calibration.c \
	$(SRC_DIR)/app/vox.c \
	$(SRC_DIR)/app/fm_radio.c \
	$(SRC_DIR)/app/channel.c \
	$(SRC_DIR)/app/scanner.c \
	$(SRC_DIR)/app/spectrum.c \
	$(SRC_DIR)/app/cps.c \
	$(SRC_DIR)/app/settings.c \
	$(SRC_DIR)/kernel/scheduler.c \
	$(SRC_DIR)/kernel/event.c \
	$(SRC_DIR)/tests/hw_test.c \
	$(SRC_DIR)/debug_uart.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# -- Target output --------------------------------------------------------
TARGET  = rt950-custom
ELF     = $(BUILD_DIR)/$(TARGET).elf
BIN     = $(BUILD_DIR)/$(TARGET).bin
HEX     = $(BUILD_DIR)/$(TARGET).hex
MAP     = $(BUILD_DIR)/$(TARGET).map
BTF     = $(BUILD_DIR)/$(TARGET).BTF

# -- Linker script --------------------------------------------------------
LDSCRIPT = linker.ld

# -- Compiler flags -------------------------------------------------------
CFLAGS  = -mcpu=cortex-m4
CFLAGS += -mthumb
CFLAGS += -mfloat-abi=hard
CFLAGS += -mfpu=fpv4-sp-d16
CFLAGS += -O2
CFLAGS += -Wall -Wextra -Wshadow -Werror
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fno-common
CFLAGS += -std=c11
CFLAGS += -I$(INC_DIR)

# -- Linker flags ---------------------------------------------------------
LDFLAGS  = -mcpu=cortex-m4
LDFLAGS += -mthumb
LDFLAGS += -mfloat-abi=hard
LDFLAGS += -mfpu=fpv4-sp-d16
LDFLAGS += -T$(LDSCRIPT)
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(MAP)
LDFLAGS += -nostdlib
LDFLAGS += --specs=nosys.specs

# -- Default encryption key (all zeros -- replace with actual key) ---------
BTF_KEY = 00000000000000000000000000000000

# ==========================================================================
#  Build targets
# ==========================================================================

.PHONY: all clean flash btf size disasm test

all: $(BIN) $(HEX) size

# Hardware test build: make test TEST=N (1-11)
ifdef TEST
CFLAGS += -DHW_TEST=$(TEST)
endif

# Debug UART output: make DEBUG=1
ifdef DEBUG
CFLAGS += -DDEBUG_UART
endif
test:
ifndef TEST
	@echo "Usage: make test TEST=N"
	@echo "  1=blinky  2=uart  3=lcd  4=bk4829  5=si4732"
	@echo "  6=flash   7=adc   8=dac  9=keypad  10=gps  11=diag"
else
	@$(MAKE) clean
	@$(MAKE) all TEST=$(TEST)
	@echo ""
	@echo "  Test $(TEST) built -> $(BIN)"
endif

# Link
$(ELF): $(OBJS)
	@echo "  LD    $@"
	@$(LD) $(LDFLAGS) -o $@ $^

# ELF -> raw binary
$(BIN): $(ELF)
	@echo "  BIN   $@"
	@$(OBJCOPY) -O binary $< $@

# ELF -> Intel HEX
$(HEX): $(ELF)
	@echo "  HEX   $@"
	@$(OBJCOPY) -O ihex $< $@

# Compile C sources
$(BUILD_DIR)/app/display.o: $(SRC_DIR)/app/font_menu_ext.inc

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Per-file overrides: disable tail-call optimization for modules with
# flash read chains that are sensitive to GCC sibling-call transforms.
$(BUILD_DIR)/drivers/spi.o: CFLAGS += -fno-optimize-sibling-calls
$(BUILD_DIR)/drivers/flash_wearleveling.o: CFLAGS += -fno-optimize-sibling-calls
$(BUILD_DIR)/app/vfo.o: CFLAGS += -fno-optimize-sibling-calls

# Print size summary
size: $(ELF)
	@echo ""
	@$(SIZE) $<

# Generate BTF firmware image
btf: $(BIN)
	@echo "  BTF   $(BTF)"
	@python3 tools/encrypt_btf.py $(BIN) $(BTF) --key $(BTF_KEY)

# Disassembly listing (for verification)
disasm: $(ELF)
	@$(OBJDUMP) -d -S $< > $(BUILD_DIR)/$(TARGET).lst
	@echo "  Disassembly -> $(BUILD_DIR)/$(TARGET).lst"

# Flash via OpenOCD / pyOCD / DFU (customize for your programmer)
flash: $(BIN)
	@echo "  TODO: Configure flash command for your programmer"
	@echo "  Example: openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \\"
	@echo "           -c \"program $(BIN) 0x08003000 verify reset exit\""

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  Cleaned build directory"
