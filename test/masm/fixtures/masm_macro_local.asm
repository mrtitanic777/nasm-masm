; Written by: Logan Greer
; masm_macro_local.asm -- MASM `LOCAL' inside a MACRO body declares macro-local
; LABELS (a fresh unique symbol per expansion), NOT MASM-6 PROC stack locals.
; Each expansion must get its own `skip', so two invocations don't collide.
bits 16
STIRET macro
	LOCAL	Dont
	pushf
	pop	ax
	test	ah, 2
	jnz	short Dont
	sti
Dont:
	iret
	endm
	STIRET
	STIRET
