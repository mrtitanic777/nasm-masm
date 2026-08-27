; masm_farptr_half.asm -- cmacros far-pointer half accessors on a struct member.
; A DWORD (far pointer) member has `.off'/`.lo' (low word, +0) and
; `.sel'/`.seg'/`.hi' (high word, +2) sub-accessors.  On a `[reg].member.half'
; or `[reg].TYPE.member.half' access the front-end resolves the half to a word
; offset -- WITHOUT mistaking a real member named `lo'/`hi' (as in a union) for
; a half: the suffix is only a half when the chain's first component is a
; member, never a struct TYPE.
bits 16
NODE struc
n_link  dd  ?               ; far pointer at offset 0
n_data  dw  ?               ; at offset 4
NODE ends
PAIR struc                  ; a struct whose members really are named lo/hi
lo      dw  ?
hi      dw  ?
PAIR ends
	mov ax, [bx].n_link.lo   ; [bx + 0]      8b 07
	mov dx, [bx].n_link.hi   ; [bx + 0 + 2]  8b 57 02
	mov cx, [bx].n_link.sel  ; [bx + 0 + 2]  8b 4f 02
	mov si, [bx].n_data      ; plain member  8b 77 04
	mov di, [bx].PAIR.hi     ; real member `hi' (type-qualified)  8b 7f 02
	ret
