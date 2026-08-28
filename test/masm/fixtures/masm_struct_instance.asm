; Written by: Logan Greer
; masm_struct_instance.asm -- a BARE struct instance `STRUCTTYPE <inits>' whose
; name comes from a preceding `labelB <..>' / `name label type' (kdata pairs
; `labelB <PUBLIC,bootExecBlock>' with `EXECBLOCK <0,0,0,0>').  The front-end
; emits just the field data at the current location; `.member' access on the
; preceding (registered) label resolves through the struct field offsets.
bits 16
EXECBLOCK struc
envseg    dw ?
lpcmdline dd ?
EXECBLOCK ends
bootblk label byte
	EXECBLOCK <0aa55h, 12345678h>   ; -> dw 0aa55h ; dd 12345678h  55aa 78563412
	mov ax, bootblk.envseg          ; envseg@0 -> mov ax,[bootblk]  a1 00 00
	ret
