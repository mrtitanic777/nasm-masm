; masm_this_align.asm -- two related location-counter features (ldboot's
; paragraph alignment needs both):
;  * MASM `THIS <type>' = the current address -> `$' (`here equ this byte').
;  * an assembly-time `if (<expr with $>) / body / endif' is a location
;    conditional NASM's %if cannot evaluate; wrap each body line in
;    `times ((cond) != 0) body' (an assembly-time conditional emission).
bits 16
here equ this byte      ; here = $ (offset 0)
	db 'hello'          ; 5 bytes -> offset 5
	rept 16             ; pad until ($-here) is a multiple of 16
if ($ - here) and 0Fh
	db 0
endif
	endm
	dw 0aa55h           ; now at offset 0x10                55 aa
	ret
