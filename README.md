# simpleos-kernel

Part of the [SimpleOS](https://github.com/MySimpleOs/simpleos) project.

## Role

The SimpleOS kernel. Targets `x86_64-elf`, enters long mode via the Limine
boot protocol, runs mapped into the higher half at `0xffffffff80000000`.

## Layout

```
kernel/
├── Makefile
├── linker.ld
├── src/
│   ├── kmain.c             entry point — paints framebuffer, halts
│   └── limine_requests.c   Limine protocol requests + start/end markers
└── README.md
```

Build artifacts live under `$(ROOT)/build/kernel/` in the parent repo, not
inside this submodule.

## Build

From the SimpleOS root:

```bash
make kernel          # produces build/kernel/simpleos.elf
make iso             # adds Limine to a bootable ISO
make run             # boots the ISO in QEMU
```

Standalone (outside the meta repo):

```bash
make CROSS=/path/to/x86_64-elf- LIMINE_H=/path/to/limine.h
```

Requires the SimpleOS cross-toolchain (`make toolchain` at the parent) and
a copy of `limine.h` (comes from `boot/limine/` once `make limine` has run).

## Compile flags

Kernel code is built freestanding with no red zone, no PIC/PIE, no stack
protector, and general-purpose registers only (no FPU/MMX/SSE state to save
on context switch yet). See `Makefile` for the exact list.
