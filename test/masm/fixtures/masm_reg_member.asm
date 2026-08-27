; masm_reg_member.asm -- MASM uses `.' for struct-member access on a BASE
; REGISTER too: `[di.CacheExe]' means `[di + CacheExe]' (the field's offset),
; and a bare numeric `.' displacement `[bp.6]' means `[bp + 6]'.  NASM has no
; `reg.field' form, so the front-end rewrites `base.field' -> `base + field'
; when inside `[]' and base is a general-purpose register.  Only inside
; brackets -- a bare `di.CacheExe' outside a memref is left alone.
bits 16
REC struc
rec_lo	dw	?
rec_hi	dw	?
REC ends
	mov bx, [di.rec_lo]        ; [di+0]            8b 1d
	mov ax, [di.rec_hi]        ; [di+2]            8b 45 02
	mov dx, [bp.6]             ; numeric disp [bp+6]   8b 56 06
	mov [si.rec_hi], cx        ; [si+2]            89 4c 02
	ret
