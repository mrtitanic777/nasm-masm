; Written by: Logan Greer
; hll_struct.asm -- B3: STRUCT definitions, member access, SIZEOF.
; A STRUCT becomes a NASM struc, so STRUCT.member offsets and STRUCT_size
; fall out for free. Register-based member access `[reg].STRUCT.member' ->
; `[reg + STRUCT.member]', and `SIZEOF STRUCT' -> the struct size. Validated
; byte-identical to ML 6.11:
;   mov eax, SIZEOF SBOX       -> b8 0c000000   (4+2+2+4 = 12 bytes)
;   movzx ecx, word [esi].lo   -> 0f b7 4e 04   (lo at offset 4, past tag[4])
;   mov edx, [esi].PAIR       -> 8b 56 08      (member at offset 8)
;   mov esi, [ebx].PAIR.y     -> 8b 73 04      (y at offset 4 of a DWORD pair)
SBOX	STRUCT
tag	BYTE	4 DUP(?)
lo	WORD	?
hi	WORD	?
PAIR	DWORD	?
SBOX	ENDS

PT	STRUCT
x	DWORD	?
y	DWORD	?
PT	ENDS

	.386
	.model flat, stdcall
	.code
f	proc
	mov eax, SIZEOF SBOX
	movzx ecx, word ptr [esi].SBOX.lo
	mov edx, [esi].SBOX.PAIR
	mov esi, [ebx].PT.y
	ret
f	endp
	end
