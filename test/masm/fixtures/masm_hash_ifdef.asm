; masm_hash_ifdef.asm -- some MASM sources (and the Win3.1 KERNEL reconstruction)
; write the conditional-assembly IF-family with a C-preprocessor `#' prefix
; (`#ifdef WOW' ... `#endif').  NASM reads a leading `#' as a line-number
; directive, so the front-end strips it and treats the rest as the MASM
; conditional.  Only the IF-family is unwrapped; the block for an undefined
; symbol is skipped, exactly as `ifdef' would.
bits 16
#ifndef WOW
	mov ax, 1              ; taken (WOW undefined)   b8 01 00
#endif
#ifdef WOW
	mov bx, 2              ; skipped
#endif
#if 1
	mov cx, 3              ; taken                   b9 03 00
#else
	mov dx, 4              ; skipped
#endif
	ret
