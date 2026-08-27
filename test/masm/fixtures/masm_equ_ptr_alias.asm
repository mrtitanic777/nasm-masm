; masm_equ_ptr_alias.asm -- a MASM text equate that aliases a bare size-cast
; keyword (`wptr EQU word ptr'), used to abbreviate casts at the use site.
; The RHS is not numeric, so it binds as a text alias, not a numeric equate.
bits 16
wptr equ word ptr
bptr equ byte ptr
dptr equ dword ptr
buf dd 0
	mov wptr [buf], 3       ; c7 06 xx xx 03 00
	mov bptr [buf], 1       ; c6 06 xx xx 01
	mov dptr [buf], 7       ; 66 c7 06 xx xx 07 00 00 00
	ret
