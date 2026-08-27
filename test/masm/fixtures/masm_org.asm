; masm_org.asm -- MASM `ORG N' sets the SEGMENT-relative location counter to N
; (laying data/code at fixed offsets).  NASM's own `org' sets the absolute load
; base and is -f bin only, so it fails for the object backends.  The front-end
; emits the equivalent padding instead -- `times (N-($-$$)) db 0' -- which works
; everywhere.  `ORG 0' at a segment start is a no-op.
bits 16
	org 0                  ; no-op
	dw 0aa55h              ; at offset 0        55 aa
	org 10h                ; pad to offset 0x10 (14x 00)
	dw 0bb66h              ; at offset 0x10     66 bb
	ret
