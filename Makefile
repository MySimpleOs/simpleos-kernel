# SimpleOS kernel — cross-compiled for x86_64-elf, Limine boot protocol.

ROOT        ?= $(abspath ..)
BUILD       ?= $(ROOT)/build
BUILD_DIR   := $(BUILD)/kernel
SRC         := $(CURDIR)/src
LIMINE_H    ?= $(ROOT)/boot/limine/limine.h

CROSS       ?= $(ROOT)/toolchain/out/bin/x86_64-elf-
CC          := $(CROSS)gcc
LD          := $(CROSS)ld

CSRCS       := $(shell find $(SRC) -name '*.c')
# userdemo.S needs a generated .bin next to it, so exclude from the generic
# rule and give it its own build step below.
SSRCS       := $(shell find $(SRC) -name '*.S' ! -name 'userdemo.S')
OBJS        := $(patsubst $(SRC)/%.c,$(BUILD_DIR)/%.o,$(CSRCS)) \
               $(patsubst $(SRC)/%.S,$(BUILD_DIR)/%.o,$(SSRCS)) \
               $(BUILD_DIR)/userdemo.o
DEPS        := $(OBJS:.o=.d)

USERDEMO_SRC_BIN := $(ROOT)/build/libc/init.bin
USERDEMO_BIN     := $(BUILD_DIR)/userdemo.bin

CFLAGS      := -std=gnu11 \
               -ffreestanding -fno-stack-protector -fno-stack-check \
               -fno-PIC -fno-PIE \
               -fno-asynchronous-unwind-tables \
               -fno-omit-frame-pointer \
               -mcmodel=kernel -mno-red-zone \
               -mgeneral-regs-only \
               -m64 \
               -Wall -Wextra -O2 -g \
               -MMD -MP \
               -I$(SRC) \
               -I$(BUILD_DIR)/include

LDFLAGS     := -nostdlib -static --no-dynamic-linker \
               -z max-page-size=0x1000 \
               -T $(CURDIR)/linker.ld

KERNEL_ELF  := $(BUILD_DIR)/simpleos.elf

FONT_ASSETS := $(CURDIR)/assets/Roboto-Regular.ttf \
               $(CURDIR)/assets/NotoSansSymbols2-Regular.ttf

CURSOR_INC   := $(SRC)/assets/cursor_default.inc
CURSOR_PNG   := $(ROOT)/default@2x.png
CURSOR_PY    := $(ROOT)/scripts/gen_cursor_rgba.py

.PHONY: all clean

all: $(KERNEL_ELF)

$(BUILD_DIR)/include/limine.h: $(LIMINE_H)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR)/%.o: $(SRC)/%.c | $(BUILD_DIR)/include/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# SIMD fast-paths opt back into XMM/YMM. -mgeneral-regs-only forbids the
# intrinsic headers from emitting vector ops, so strip it for this TU and
# turn SSE2+AVX2 on explicitly. CR0/CR4/XCR0 are set per-CPU at boot
# (arch/x86_64/simd.c) so these instructions are legal at run time.
$(BUILD_DIR)/compositor/blit_simd.o: $(SRC)/compositor/blit_simd.c | $(BUILD_DIR)/include/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2 -c $< -o $@

# stb_truetype uses float + __builtin_sqrt; -mgeneral-regs-only forbids XMM returns.
$(BUILD_DIR)/compositor/font.o: $(SRC)/compositor/font.c | $(BUILD_DIR)/include/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2 -c $< -o $@

$(BUILD_DIR)/assets/fonts.o: $(SRC)/assets/fonts.S $(FONT_ASSETS) | $(BUILD_DIR)/include/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wa,-I$(CURDIR)/assets -c $< -o $@

$(CURSOR_INC): $(CURSOR_PNG) $(CURSOR_PY)
	@python3 $(CURSOR_PY) $(CURSOR_PNG)

$(BUILD_DIR)/compositor/cursor.o: $(SRC)/compositor/cursor.c $(CURSOR_INC) | $(BUILD_DIR)/include/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(USERDEMO_BIN): $(USERDEMO_SRC_BIN)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR)/userdemo.o: $(SRC)/userdemo.S $(USERDEMO_BIN)
	@mkdir -p $(dir $@)
	$(CC) -Wa,-I$(BUILD_DIR) $(CFLAGS) -c $(SRC)/userdemo.S -o $@

$(KERNEL_ELF): $(OBJS) $(CURDIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
