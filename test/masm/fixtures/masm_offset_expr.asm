; masm_offset_expr.asm -- MASM OFFSET is a no-op prefix inside an expression (a
; bare label is already its offset).  It must work not only as a whole operand
; (`dw OFFSET lbl') but mid-expression (`dw 4 + OFFSET lbl'), as the cmacros
; <seg>OFFSET operator produces in kdata's `(N-1)*size S + dataOffset foo'.
bits 16
org 0
lbl:	dw 0			; lbl at offset 0
	dw OFFSET lbl		; 0000
	dw 4 + OFFSET lbl	; 0004
	dw (3)*2 + OFFSET lbl	; 0006
	dw OFFSET lbl + 2	; 0002
