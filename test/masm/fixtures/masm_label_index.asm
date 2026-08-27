; masm_label_index.asm -- MASM array/pointer indexing on a data label:
; `tbl[2]' is `[tbl + 2]'.  Only a registered data label is rewritten (a bare
; `tbl' with no index still means its contents, `[tbl]', via the data-label
; rule).  The index's own `]' closes the group.
bits 16
tbl dw 10, 20, 30
	mov ax, tbl              ; bare -> [tbl] (contents) = a1 <tbl> ... 8b 06
	mov bx, tbl[2]           ; -> [tbl+2] = 8b 1e 02 00
	mov cx, word ptr tbl[4]  ; -> [tbl+4] = 8b 0e 04 00
	ret
