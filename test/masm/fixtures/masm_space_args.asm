; Written by: Logan Greer
; masm_space_args.asm -- MASM accepts a SPACE (or tab) as a macro-argument
; separator, so a cmacros data declaration may be written `globalW name 0'
; instead of `globalW name, 0'.  NASM splits macro args only on commas, so the
; shim macro would see `name 0' as one argument and build a broken label; the
; front-end inserts the comma for the global/static data macros.
bits 16
%imacro globalW 1-2
	%1: dw %2       ; (stand-in for the shim's data emission)
%endmacro
	globalW cache 0aa55h   ; -> globalW cache, 0aa55h -> cache: dw 0aa55h  55 aa
	globalW other, 0bb66h  ; comma form still works                       66 bb
	ret
