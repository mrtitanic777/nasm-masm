; masm_lar_lsl.asm -- LAR/LSL read a selector from r/m16; the operand size
; follows the destination register, not the selector variable's storage width.
; MASM writes a spurious `dword ptr' size cast on the source; NASM rejects a
; dword memory source, so the front-end normalises the source size to `word'.
bits 16
sel  dw 0
h    dd 0
	lar ax, sel               ; -> lar ax,[sel]        0f 02 06 ..
	lar eax, dword ptr h      ; -> lar eax,word [h]    66 0f 02 06 ..
	lsl ecx, dword ptr h      ; -> lsl ecx,word [h]    66 0f 03 0e ..
	lsl edx, word ptr sel     ; -> lsl edx,word [sel]  66 0f 03 16 ..
	ret
