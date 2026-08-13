; masm_caseinsens.asm -- MASM is case-insensitive; NASM is not.
; Under --masm a symbol resolves regardless of case, across every symbol kind:
; data labels (contents), code labels, jump targets, `=' numerics, EQU
; constants, and macro names.  Real Win3.1 kernel relies on this (context.asm
; declares `curTDB', uses `CurTDB').  Bytes match the consistently-cased form.
; (Stock NASM stays case-sensitive -- this behaviour is --masm only.)
bits 16
counter dw 7
Stride  =  4
Limit   EQU 100
AddAX MACRO n
	add ax, n
	ENDM
	mov ax, COUNTER     ; data label, mixed case -> [counter] = a1 00 00
	inc Counter         ; -> inc word [counter] = ff 06 00 00
	mov bx, stride      ; `=' const, lower -> bb 04 00
	mov cx, LIMIT       ; EQU const, upper -> b9 64 00
	addax 1             ; macro, lower -> add ax,1 = 05 01 00
	ADDAX 2             ; macro, upper -> add ax,2 = 05 02 00
	call MyProc         ; forward code label
	jmp  theEnd
myproc:                 ; defined lower, called mixed
	ret
THEEND:                 ; defined upper, jumped lower
	nop
