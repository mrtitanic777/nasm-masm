; Written by: Logan Greer
; masm_anonlabel.asm -- MASM anonymous labels @@: / @F / @B.
; `@@:' defines an anonymous label; `@F' jumps to the NEXT one forward, `@B'
; to the nearest one backward.  Translated to counter-generated labels, which
; is single-pass-safe: @F is the label the next @@: will mint, @B the last one.
; Used pervasively in the real Win3.1 kernel (56 modules).  Bytes are identical
; to the same code written with ordinary labels.
bits 16
	cmp ax, 1
	jz  @F          ; -> forward to the first @@ below
	inc ax
@@:
	cmp bx, 2
	jz  @B          ; -> back to the @@ just above
	dec bx
@@:
	ret
