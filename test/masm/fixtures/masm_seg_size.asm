; masm_seg_size.asm -- MASM writes a segment override BEFORE the size cast
; (`es:byte ptr [bx]'), but NASM wants the size first (`byte es:[bx]').  The
; front-end reorders `<seg>:<size> ptr' to `<size> ptr <seg>:'.
bits 16
	or  es:byte ptr [bx], 3      ; -> or byte es:[bx],3    26 80 0f 03
	cmp es:byte ptr [di], al     ; -> cmp byte es:[di],al  26 38 05
	mov ax, ds:word ptr [si]     ; -> mov ax, word ds:[si] 3e 8b 04
	ret
