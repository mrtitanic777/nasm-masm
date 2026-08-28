; Written by: Logan Greer
; hll_wordops.asm -- MASM word operators in IF/= expressions.
; EQ NE LT GT LE GE -> == != < > <= >= ; MOD SHL SHR -> % << >> ;
; AND OR XOR NOT -> & | ^ ~ (whole-word, in expression context only).
; Validated byte-identical to ML 6.11 (X=7): X gt 5 (T), X eq 7 (T),
; (X and 3) ne 0 (7&3=3, T) -> all three MOVs = b8 01 / bb 02 / b9 03.
	.386
	.model flat, stdcall
X	=	7
	.code
f	proc
	if X gt 5
	  mov eax, 1
	endif
	if X eq 7
	  mov ebx, 2
	endif
	if (X and 3) ne 0
	  mov ecx, 3
	endif
	ret
f	endp
	end
