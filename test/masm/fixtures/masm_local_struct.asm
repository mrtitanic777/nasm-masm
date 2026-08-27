; masm_local_struct.asm -- a cmacros `localV name,size' stack BUFFER local is a
; struct instance; `name.member' must address the field on the stack frame.
; The front-end records localV names (masm_lbufs) so var.field rewrites them
; (`DscBuf.dsc_limit' -> `[DscBuf + dsc_limit]' -> `[[bp-8]+2]' -> `[bp-6]'),
; while a far-pointer param keeps its .sel/.off half accessor.  Both the bare
; (`DscBuf.f') and bracketed (`[DscBuf].f') source forms resolve identically.
bits 16
DSC struc
dsc_access db ?
dsc_flags  db ?
dsc_limit  dw ?
DSC ends
%imacro localV 2
    %ixdefine %1 [bp - 8]      ; stand-in for the shim's frame binding
%endmacro
	localV DscBuf, 8
	mov DscBuf.dsc_access, al   ; [bp-8]    88 46 f8
	mov ax, DscBuf.dsc_limit    ; [bp-8+2]  8b 46 fa
	mov [DscBuf].dsc_flags, bl  ; [bp-8+1]  88 5e f9
	ret
