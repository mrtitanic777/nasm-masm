; Written by: Logan Greer
; hll_comment.asm -- MASM COMMENT block directive + listing no-ops.
; COMMENT delim ... delim is a block comment (delim = first non-space char after
; COMMENT); it may span lines or close on the same line. Listing/cross-reference
; directives (.XCREF/.CREF/.LALL/.LIST/.NOLIST/...) are accepted and dropped.
; Only the three MOVs survive:  mov ax,5 / mov bx,6 / mov cx,7 = b80500 bb0600 b90700
	bits 16
	comment $ this is
	a block comment
	spanning lines $
	mov ax, 5
comment *one-liner*
	mov bx, 6
	.xcref
	.list
	mov cx, 7
