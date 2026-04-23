/* The userspace demo program (libc/examples/hello) is compiled as a flat
 * binary linked at 0x400000 and included here byte-for-byte. kmain copies
 * the range [userdemo_start, userdemo_end) into a user page at 0x400000
 * before spawning the user thread. */

.section .rodata

.global userdemo_start
.global userdemo_end

userdemo_start:
    .incbin "userdemo.bin"
userdemo_end:
