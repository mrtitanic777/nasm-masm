; masm_ccall_far.asm -- a cCall whose TARGET is a `<... ptr NAME>' pointer.
; The shim's cCall binds its target from the first argument and does `call
; <target>', so the `<>' wrapper and the `ptr' keyword must be stripped first.
; The front-end rewrites `cCall <far ptr NAME>,<args>' -> `cCall far NAME,
; <args>' (the far/near keyword stays, so the call keeps its distance).  Shown
; here with `near' so it assembles under -f bin; `<far ptr NAME>' -> `call far
; NAME' identically in the obj backend.
bits 16
%imacro cCall 1-*
	call %1                ; %1 = the rewritten target (`near NAME')
%endmacro
	jmp start
tgt:
	ret
start:
	cCall <near ptr tgt>   ; -> cCall near tgt -> call near tgt
	ret
