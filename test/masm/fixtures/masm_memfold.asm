; Written by: Logan Greer
; masm_memfold.asm -- fold a trailing [idx] / +disp into a memory operand.
; MASM lets a memory operand carry a following index or displacement:
; `[bp-4]+2' and `[bp-4][2]' both mean `[bp-4+2]'.  This matters because a
; cmacros param/local expands to a bound `[bp-off]', so source `Limit+2' /
; `Limit[2]' become `[bp-off]+2' / `[bp-off][2]' -- folded by the parser.
; (The `+disp' form is the parser's job; literal adjacent `][' the front-end
; already folds, but both are exercised here.)
bits 16
	mov ax, [bp-4]+2     ; -> [bp-2] = 8b 46 fe
	mov bx, [bp-6]-2     ; -> [bp-8] = 8b 5e f8
	mov cx, [bx][si][4]  ; -> [bx+si+4] = 8b 48 04
	mov dx, [bp+8]       ; plain, unaffected = 8b 56 08
	ret
