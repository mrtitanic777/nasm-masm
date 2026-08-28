; Written by: Logan Greer
; masm_bracket_alias.asm -- MASM param/local bound to a memory operand [bp-o]
; may ALSO be bracketed at the use site (`mov bx,[wLen]'), expanding to the
; redundant `[[bp-o]]'.  The parser collapses the inner bracket under --masm,
; and folds a trailing `+disp' (`[nBytes+2]' -> `[[bp-o]+2]') into the operand.
bits 16
%idefine wLen   [bp - 2]
%idefine nBytes [bp - 6]
	mov bx, [wLen]          ; -> mov bx,[bp-2]        8b 5e fe
	mov bx, wLen            ; bare form still works   8b 5e fe
	add [nBytes], cx        ; -> add [bp-6],cx        01 4e fa
	mov ax, [nBytes+2]      ; -> mov ax,[bp-6+2]=[bp-4] 8b 46 fc
	ret
