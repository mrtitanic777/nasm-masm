; Written by: Logan Greer
; masm_macro_variadic.asm -- a MASM caller may pass a `<...>' list argument,
; which NASM splits on its inner commas into several arguments.  Front-end-
; translated MASM macros are declared VARIADIC (0-*) so the extra pieces are
; accepted (named params still bind to %1..%N; overflow ignored) rather than
; rejected as "not taking N parameters" -- e.g. a no-op WOWTrace-style macro.
bits 16
TRACE macro msg, args
	nop                    ; body uses msg/args; extra <> pieces are ignored
	endm
	TRACE <one>            ; single arg
	TRACE "hi", <<ax,1>,<bx,2>>   ; a <> list arg NASM over-splits
	ret
