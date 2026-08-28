; Written by: Logan Greer
; masm_type_member.asm -- MASM 5.x struct fields are GLOBAL offset constants,
; so `TYPE.member' is the member's offset regardless of TYPE -- even a member
; declared in a different struct (an .ERRNZ layout check often does this), both
; bare and after a `[reg].'.  A UNION/RECORD keeps its qualified form.
bits 16
SA STRUC
sa_f db ?
sa_g db ?
sa_h dw ?
SA ENDS
SB STRUC
sb_p db ?
SB ENDS
	mov ax, SA.sa_g          ; own member, offset 1     b8 01 00
	mov cx, SB.sa_g          ; cross-struct -> global 1 b9 01 00
	mov si, [bx].SB.sa_h     ; [bx + 2] (global sa_h)   8b 77 02
	.errnz (SA.sa_g - SA.sa_f) - 1   ; passes
	ret
