	/* The store's font, Archivo regular, medium and bold (SIL Open Font License 1.1, fonts/OFL.txt), built into the
	   .xex for ui.cpp. */
	.section .rodata
	.globl font_regular, font_regular_end, font_medium, font_medium_end, font_bold, font_bold_end
	.balign 4
font_regular:
	.incbin "fonts/Archivo-Regular.ttf"
font_regular_end:
	.balign 4
font_medium:
	.incbin "fonts/Archivo-Medium.ttf"
font_medium_end:
	.balign 4
font_bold:
	.incbin "fonts/Archivo-Bold.ttf"
font_bold_end:
