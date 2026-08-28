; Written by: Logan Greer
; masm_adjbracket.asm -- MASM adjacent-bracket addressing [a][b] = [a+b].
; MASM lets bracket groups concatenate as addition: `[bp][2]' is `[bp+2]',
; `[bx][si]' is `[bx+si]'.  Pervasive in the real Win3.1 kernel stack-frame
; code (`mov ax,[bp][2]').  Bytes match the [a+b] spelling exactly.
bits 16
	mov ax, [bp][2]     ; -> mov ax,[bp+2]  = 8b 46 02
	xchg [bp][4], ax    ; -> xchg [bp+4],ax = 87 46 04
	mov cx, [bx][si]    ; -> mov cx,[bx+si] = 8b 08
	mov dx, [bx][si][4] ; -> mov dx,[bx+si+4] = 8b 50 04
	ret
