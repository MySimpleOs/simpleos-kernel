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
OBJS        := $(patsubst $(SRC)/%.c,$(BUILD_DIR)/%.o,$(CSRCS))
DEPS        := $(OBJS:.o=.d)

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

.PHONY: all clean

all: $(KERNEL_ELF)

$(BUILD_DIR)/include/limine.h: $(LIMINE_H)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR)/%.o: $(SRC)/%.c | $(BUILD_DIR)/include/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJS) $(CURDIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
