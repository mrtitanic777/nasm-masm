; masm_anon_cond.asm -- MASM @F/@B skip a @@: that a conditional excludes.  The
; anonymous-label counter must NOT advance for a @@: in a dead IF branch, or a
; live @F would point at a label that is never emitted.
bits 16
	jz @F                   ; -> the emitted @@: below, not the skipped one
	nop
ifdef NOT_DEFINED           ; skipped
@@:                         ; must not consume the counter
	int3
endif
@@:                         ; the real target
	ret
