; hll_macro.asm -- B7: MACRO/ENDM, REPT, TEXTEQU, = .
; A MACRO with named params, a REPT block, a text equate and a numeric equate.
; Validated byte-identical to ML.
	.386
	.model flat, stdcall
COUNT	=	3
areg	TEXTEQU	<eax>
	.code
addm	MACRO a, b
	mov areg, a
	add areg, b
	ENDM
f	proc
	addm 3, 4
	REPT COUNT
	  inc eax
	ENDM
	ret
f	endp
	end
