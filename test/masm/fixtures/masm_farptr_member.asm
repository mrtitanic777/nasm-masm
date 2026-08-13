; masm_farptr_member.asm -- var.field member access on a far-pointer variable.
; A DWORD/far-pointer variable reinterpreted through a struct's fields:
; `myPtr.sel' -> `[myPtr + sel]' (sel = SEGOFF.sel offset 2), the high word.
; Distinct from a struct INSTANCE member (`p POINT<>' defines the label `p.x'):
; that stays a sized data label.  A type-qualified `SEGOFF.sel' (base is the
; struct TYPE) is left alone as the plain offset.
bits 16
SEGOFF STRUC
soff DW ?
ssel DW ?
SEGOFF ENDS
myPtr dd 0
	mov ax, myPtr.ssel  ; high word -> mov ax,[myPtr+2] = a1 02 00
	mov bx, myPtr.soff  ; low  word -> mov bx,[myPtr]   = 8b 1e 00 00
	mov cx, SEGOFF.ssel ; type-qualified offset stays 2 -> mov cx,2 = b9 02 00
	ret
