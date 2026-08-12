; rawbits.asm -- a raw `bits 32' (rather than .386/.MODEL) must still set the
; struc-safe __?MASM_BITS?__ shadow, so a following `segment' takes USE32 under
; the object backends (not the stale 16-bit default). Regression for the gap
; found assembling NASM's own MASM test (masmdisp.asm) which drives bitness with
; a bare `bits' directive. Under -f obj this must stay 32-bit (no 66 prefixes):
;   mov eax, ebx -> 8b c3      add ecx, edx -> 03 ca
	bits 32
_TEXT	segment
	mov eax, ebx
	add ecx, edx
	ret
_TEXT	ends
	end
