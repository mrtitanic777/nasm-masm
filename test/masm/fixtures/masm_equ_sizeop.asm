; Written by: Logan Greer
; masm_equ_sizeop.asm -- SIZE/SIZEOF operator inside an EQU expression.
; `DSC_LEN EQU (SIZE DscPtr)' -- the EQU right-hand side gets the same operator
; rewriting an instruction operand does, so `SIZE t' becomes `t_size'.  Real
; Win3.1 protect.inc uses `DSC_LEN equ (size DscPtr)'.
bits 16
struc DscPtr
  .lo:   resw 1
  .hi:   resw 1
  .base: resd 1
endstruc
DSC_LEN	equ (size DscPtr)       ; -> (DscPtr_size) = 8
	mov ax, DSC_LEN         ; -> b8 08 00
	mov bx, DSC_LEN + 1     ; -> bb 09 00
	ret
