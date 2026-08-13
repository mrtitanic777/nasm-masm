; masm_public.asm -- PUBLIC / ASSUME directives (front-end).
; `PUBLIC name[,name...]' -> NASM `global' (a `:type' qualifier is dropped);
; `ASSUME seg:val,...' carries no encoding in flat/obj output -> dropped.
; Both are byte-neutral here, so the emitted code is just the two instructions.
; (EXTRN name:type -> `extern name' is exercised under -f obj, not -f bin.)
bits 16
	assume cs:_TEXT, ds:_DATA, es:nothing
	public go, helper
go:
	mov ax, 1           ; -> b8 01 00
	ret                 ; -> c3
helper:
	ret                 ; -> c3
