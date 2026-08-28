; Written by: Logan Greer
; masm_even.asm -- MASM `EVEN' aligns the location counter to a word boundary.
; NASM has no `even' directive, so the front-end maps it to `align 2'.
bits 16
	db 1                   ; odd offset
	even                   ; -> align 2: pad to even    (00)
	dw 0aa55h              ; now word-aligned            55 aa
	db 2
	db 3                   ; even offset already
	even                   ; -> align 2: no padding
	dw 1234h               ; 34 12
	ret
