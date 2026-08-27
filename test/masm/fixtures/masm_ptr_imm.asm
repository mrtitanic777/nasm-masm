; masm_ptr_imm.asm -- MASM `WORD PTR <constant>' is a sized IMMEDIATE, not a
; memory reference: `mov [mem], WORD PTR k' stores the word value k.  NASM's
; PTR always meant memory; under --masm a number / non-data-label identifier
; after PTR stays an immediate (a following `[' or a real data label is still
; memory).  A parenthesised expression after PTR keeps NASM's memory meaning.
bits 16
K equ 7
	mov [bx+4], WORD PTR K      ; word [bx+4],7    c7 47 04 07 00
	mov ax, WORD PTR K          ; immediate 7      b8 07 00
	mov dl, BYTE PTR 5          ; immediate 5      b2 05
	ret
