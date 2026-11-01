/* Embedded assets: the two Noto Sans faces (SIL Open Font License, licenses/NotoSans) and the
 * project icon (licenses/Icon/CREDITS.txt). Paths come from CMake. */
    .section .rodata
    .balign 16
    .global tt_font_black_data
tt_font_black_data:
    .incbin TT_FONT_BLACK
    .global tt_font_black_end
tt_font_black_end:
    .byte 0

    .balign 16
    .global tt_font_regular_data
tt_font_regular_data:
    .incbin TT_FONT_REGULAR
    .global tt_font_regular_end
tt_font_regular_end:
    .byte 0

    .balign 16
    .global tt_icon_data
tt_icon_data:
    .incbin TT_ICON
    .global tt_icon_end
tt_icon_end:
    .byte 0

    .section .note.GNU-stack,"",@progbits
