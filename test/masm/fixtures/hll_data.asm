; hll_data.asm -- B3/B4: MASM data-label semantics + size inference.
; A bare data label means its CONTENTS ([label], sized by its type); OFFSET is
; the address. Validated byte-identical to ML (a1/a0/66a1/a3/ff05/ba forms).
	.386
	.model flat, stdcall
	.data
bval	db 5
wval	dw 1234h
val	dd 12345678h
	.code
	mov eax, val            ; contents -> [val]
	mov al, bval            ; byte contents
	mov ax, wval            ; word contents
	mov edx, offset val     ; the ADDRESS of val
	inc val                 ; sized: inc dword [val]
	mov val, eax            ; store [val]
	end
