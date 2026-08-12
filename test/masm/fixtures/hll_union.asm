; hll_union.asm -- B3 tail: UNION types (members overlap at offset 0).
; A UNION lowers to per-member offset defines (all 0) plus NAME_size = the
; largest member; member access and SIZEOF then work like a struct.
; Validated byte-identical to ML 6.11:
;   [esi].U.lo  (DWORD @0)   -> 8b 06        mov eax,[esi]
;   [esi].U.mid (WORD  @0)   -> 0f b7 1e     movzx ebx,word [esi]
;   [esi].U.sm  (BYTE  @0)   -> 8a 0e        mov cl,[esi]
;   SIZEOF U    (max = 4)    -> ba 04000000  mov edx,4
U	UNION
lo	DWORD	?
mid	WORD	?
sm	BYTE	?
U	ENDS
	.386
	.model flat, stdcall
	.code
f	proc
	mov eax, [esi].U.lo
	movzx ebx, word ptr [esi].U.mid
	mov cl, [esi].U.sm
	mov edx, SIZEOF U
	ret
f	endp
	end
