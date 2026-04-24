/* Embedded TrueType fonts for stb_truetype (see compositor/font.c). */

.section .rodata
.balign 4

.global font_noto_sans_start
.global font_noto_sans_end
font_noto_sans_start:
    .incbin "NotoSans-Regular.ttf"
font_noto_sans_end:

.balign 4
.global font_noto_symbols2_start
.global font_noto_symbols2_end
font_noto_symbols2_start:
    .incbin "NotoSansSymbols2-Regular.ttf"
font_noto_symbols2_end:
