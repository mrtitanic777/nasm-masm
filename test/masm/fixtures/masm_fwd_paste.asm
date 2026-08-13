; masm_fwd_paste.asm -- forward-label alias through an &-paste equate.
; The Win3.1 kernel fault-trap tables (gpfix.inc) build a symbol by pasting a
; macro parameter (`tbl&count = handler') and bind it to a handler label that
; is defined LATER in the file.  Such a `<paste> = <ident>' must become a
; textual alias (%define), resolved at the `dw' use site, not an immediate
; %assign (which would fail "not defined before use" on the forward label).
bits 16
cnt = 0
mk	macro	handler, count
	tbl&count = handler
endm
outer	macro	handler
	mk	handler, %cnt
	cnt = cnt + 1
endm
	outer	target                      ; tbl0 := target (defined below)
	dw	tbl0                        ; -> dw target = 0002 -> 02 00
target:
	ret                                 ; -> c3
	end
