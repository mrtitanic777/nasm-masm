; Written by: Logan Greer
; hll_expr.asm -- B9: expression operators (TYPE, SIZEOF, LOW/HIGH, radix).
; The C front-end rewrites the MASM prefix operators into NASM expressions;
; primitive *_size come from the package, STRUCT_size from NASM's `struc'.
; Validated byte-identical to ML 6.11:
;   LOW 1234h        -> b0 34            HIGH 1234h        -> b4 12
;   TYPE POINT (=8)  -> bb 08000000      TYPE DWORD (=4)   -> b9 04000000
;   1010b (=0Ah)     -> ba 0a000000      17q (octal 15)    -> be 0f000000
;   LOWWORD 12345678h  -> bf 78560000    HIGHWORD 12345678h -> bd 34120000
POINT	STRUCT
x	DWORD	?
y	DWORD	?
POINT	ENDS
	.386
	.model flat, stdcall
VAL	equ	1234h
	.code
f	proc
	mov al, LOW VAL
	mov ah, HIGH VAL
	mov ebx, TYPE POINT
	mov ecx, TYPE DWORD
	mov edx, 1010b
	mov esi, 17q
	mov edi, LOWWORD 12345678h
	mov ebp, HIGHWORD 12345678h
	ret
f	endp
	end
