; masm_ifdef_equ.asm -- MASM IFDEF is true for ANY defined symbol, including a
; numeric EQU/= constant.  NASM's %ifdef sees only preprocessor macros, so the
; front-end must decide IFDEF/IFNDEF on an EQU/= constant directly.
bits 16
KDEBUG equ 1
FLAG   =   0
ifdef KDEBUG            ; defined (EQU) -> taken
	db 0AAh
endif
ifndef KDEBUG           ; defined -> NOT taken
	db 0BBh
endif
ifdef FLAG             ; defined via = -> taken (value irrelevant to IFDEF)
	db 0CCh
endif
ifdef NOTDEFINED        ; genuinely undefined -> NOT taken
	db 0DDh
endif
	ret
