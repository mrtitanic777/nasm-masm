; Written by: Logan Greer
; masm_operand_parens.asm -- some MASM sources parenthesise a whole operand as a
; grouping habit (`mov cx, ( nCount )', `mov ax, word ptr ( lpSource + 2 )').
; When the operand is a memory reference (a cmacros param -> [bp+N]) NASM rejects
; the parens; MASM treats them as pure grouping.  Strip parens that span an
; ENTIRE operand value; a sub-expression's parens (precedence) are kept.
bits 16
%idefine lpSource [bp+6]
%idefine nCount [bp+10]
	mov cx, ( nCount )              ; -> mov cx,[bp+10]      8b 4e 0a
	les di, ( lpSource )           ; -> les di,[bp+6]       c4 7e 06
	mov ax, word ptr ( lpSource + 2 ) ; -> mov ax,[bp+8]    8b 46 08
	mov bx, 5 * (2 + 3)            ; sub-expr: parens kept  bb 19 00 (=25)
	mov dx, (7)                    ; -> mov dx,7            ba 07 00
	ret
