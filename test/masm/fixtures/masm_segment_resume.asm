; masm_segment_resume.asm -- MASM SEGMENT/ENDS nest: `NAME ENDS' resumes the
; segment that was open before the matching `NAME SEGMENT'.  A data table
; emitted into another segment mid-stream (as the fault-trap macros do) must not
; disturb the enclosing segment, so `after' lands right after `before' and the
; self-relative distance is exactly the 1 byte between them.
bits 16
CODE segment
before:	db 1
GPFIX segment
	db 9, 9, 9, 9		; goes to GPFIX, not CODE
GPFIX ends
after:	db 2			; resumes CODE -- right after `before'
	dw after - before	; == 1 if the segment resumed correctly
CODE ends
