; Written by: Logan Greer
; masm_ccall_far.asm -- cCall target/argument-syntax variants the front-end
; normalises for the shim's cCall:
;  1. a `<... ptr NAME>' pointer TARGET: strip `<>' and `ptr', keep far/near,
;     so `call <target>' keeps its call distance.  (Shown with `near' so it
;     assembles under -f bin; `<far ptr NAME>' -> `call far NAME' in obj.)
;  2. a target and its `<...>' argument list separated by a SPACE, not a comma
;     (`cCall NAME <args>'): insert the comma so NASM does not mis-split the
;     target into the first argument.
bits 16
%imacro cCall 1-*
	call %1                ; %1 = the rewritten target (args elided here)
%endmacro
	jmp start
tgt:
	ret
start:
	cCall <near ptr tgt>   ; <near ptr> target      -> call near tgt   e8 ..
	cCall tgt <ax,bx>      ; space before <> args   -> call tgt        e8 ..
	ret
