; Written by: Logan Greer
; masm_bracket_size.asm -- a SIZE/SIZEOF/TYPE operator after a `].' member dot:
; `lea si,[si].SIZE T' means `[si + sizeof(T)]'.  The front-end resolves the
; operator to the struct's `_size' the same way a bare `SIZE T' does.
bits 16
REC struc
r_a  dw ?
r_b  dd ?
REC ends                       ; SIZE REC = 6
	lea si, [si].SIZE REC      ; [si + 6]        8d 74 06
	lea di, [di].SIZEOF REC    ; [di + 6]        8d 7d 06
	mov ax, [bx].TYPE REC      ; [bx + 6]        8b 47 06
	ret
