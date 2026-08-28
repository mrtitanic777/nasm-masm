; Written by: Logan Greer
; masm_if_undef.asm -- MASM evaluates an undefined symbol in a conditional-
; assembly IF expression as 0 (a build flag never -D'd or EQU'd).  NASM would
; error "symbol not defined"; under --masm the IF is taken as false and skipped.
bits 16
if SDEBUG                    ; SDEBUG undefined -> 0 -> block skipped
	mov ax, never_defined   ; would error if the block were assembled
	int3
endif
if RETAIL_FLAG + 0          ; undefined in an expression -> 0
	hlt
endif
	nop                     ; 90
	ret                     ; c3
