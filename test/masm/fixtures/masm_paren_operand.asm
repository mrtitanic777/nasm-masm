; Written by: Logan Greer
; masm_paren_operand.asm -- MASM uses `(...)' as grouping, so an operand may
; wrap a lone identifier in parentheses (`word ptr (mflags)', where mflags is a
; memory alias).  NASM rejects `(mem)', and the parens carry no meaning around a
; single name, so the front-end strips `(IDENT)' -> `IDENT'.  A real expression
; `(a+b)' keeps its parens.
bits 16
mflags EQU byte ptr [bp-14]
	mov word ptr (mflags), cx    ; -> mov word [bp-14],cx     89 4e f2
	mov ax, (mflags)             ; -> mov ax,[bp-14]          8b 46 f2
	mov bx, (VAL)                ; -> mov bx, VAL             bb 07 00
	mov dx, (2 + 5)              ; (expr) kept -> mov dx,7    ba 07 00
VAL equ 7
	ret
