; masm_caseinsens.asm -- MASM is case-insensitive; NASM is not.
; Under --masm a symbol resolves regardless of case: a label defined one way
; is reachable spelled another (`counter'/`COUNTER'/`Counter'), for data
; labels (contents), code labels, and jump targets alike.  Real Win3.1 kernel
; relies on this (context.asm declares `curTDB', uses `CurTDB').  Bytes are
; identical to the consistently-cased spelling.
bits 16
counter dw 7
	mov ax, COUNTER     ; data label, mixed case -> [counter] = a1 00 00
	inc Counter         ; -> inc word [counter] = ff 06 00 00
	call MyProc         ; forward code label
	jmp  theEnd
myproc:                 ; defined lower, called mixed
	ret
THEEND:                 ; defined upper, jumped lower
	nop
