; masm_label_stringop.asm -- a label prefixing a bare MASM string instruction
; (`foint: lods byte ptr es:[si]').  The string-op rewrite keys on the first
; word (here the label), so the front-end splits the label off, sizes the
; string op from its operand, and re-emits the label ahead of it.
bits 16
foint:	lods byte ptr es:[si]        ; foint: + lodsb            ac
	nop                          ;                            90
loop2:	rep movs word ptr es:[di], word ptr ds:[si]  ; loop2: + rep movsw  f3 a5
	ret
