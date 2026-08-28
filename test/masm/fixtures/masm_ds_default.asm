; Written by: Logan Greer
; masm_ds_default.asm -- MASM (like every mainstream x86 assembler but NASM)
; drops a segment-override prefix that names the operand's DEFAULT segment.
; DS is the default for any base but BP/SP/EBP/ESP, so an explicit `ds:' there
; is redundant and emits no 3Eh byte; a stack-register base defaults to SS, so
; `ds:' there IS meaningful and stays.  A non-default override (es:) always stays.
bits 16
	mov ax, ds:[si]         ; ds redundant -> 8b 04
	mov ax, ds:[bx]         ; ds redundant -> 8b 07
	mov ax, ds:[bp]         ; base bp => SS default, ds: kept -> 3e 8b 46 00
	mov ax, es:[si]         ; es never default -> 26 8b 04
	mov bx, ds:[1234h]      ; direct addr, ds default -> 8b 1e 34 12
	ret
