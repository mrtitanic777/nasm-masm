; masm_type_member.asm -- MASM 5.x struct fields are GLOBAL offset constants,
; so `TYPE.member' is the member's offset regardless of TYPE -- even a member
; declared in a different struct (an .ERRNZ layout check often does this).
; Outside brackets the front-end drops the TYPE prefix to the bare offset.
bits 16
SA STRUC
sa_f db ?
sa_g db ?
sa_h dw ?
SA ENDS
SB STRUC
sb_p db ?
SB ENDS
	mov ax, SA.sa_g        ; own member, offset 1     b8 01 00
	mov cx, SB.sa_g        ; cross-struct -> global 1 b9 01 00
	mov dx, SA.sa_h        ; offset 2                 ba 02 00
	.errnz (SA.sa_g - SA.sa_f) - 1   ; 1-0-1 == 0 -> passes
	ret
