; masm_movzx_size.asm -- MOVZX/MOVSX need an explicit source size that NASM
; cannot infer from a MASM memory operand.  MASM takes it from the source's
; declared TYPE: a struct field's DB/DW, a typed data label, or a cmacros
; scalar parm/local (parmB/W = byte/word).  The front-end looks that up and
; injects `byte'/`word' before the source; register and pre-sized sources are
; left untouched.
bits 16
PGA struc
pga_pglock db ?
pga_sel    dw ?
PGA ends
blk dw 0
%imacro parmW 1
    %ixdefine %1 [bp + 6]        ; stand-in for the shim's parm binding
%endmacro
	parmW Selector
	movzx ax,  [bx].pga_pglock   ; DB field  -> movzx ax, byte [bx]
	movzx ebx, [bx].pga_sel      ; DW field  -> movzx ebx, word [bx+2]
	movzx edx, blk               ; DW label  -> movzx edx, word [blk]
	movzx ecx, Selector          ; parmW     -> movzx ecx, word [bp+6]
	movsx si,  [bx].pga_pglock   ; DB field  -> movsx si, byte [bx]
	movzx eax, bl                ; register source: untouched
	ret
