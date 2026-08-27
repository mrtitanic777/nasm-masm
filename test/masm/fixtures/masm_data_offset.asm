; masm_data_offset.asm -- OFFSET in a DATA initializer (`dw OFFSET label') is a
; no-op: a bare label is already its address.  The regular operand parser
; handles `mov ax,OFFSET foo', but the data (dw/dd) parser did not, so a
; globalW/dw with an OFFSET/<seg>OFFSET init (e.g. FaultHandler) failed to
; define the label -- cascading to every bare use of it.
bits 16
tgt:
	nop
tbl	dw OFFSET tgt          ; -> dw tgt        (addr of tgt)
	dd OFFSET tgt          ; -> dd tgt
	mov ax, OFFSET tgt     ; operand-parser OFFSET still works
	ret
