; hll_local.asm -- B5 tail: PROC USES + LOCAL.
; USES saves/restores registers around the body; LOCAL reserves [ebp-N] stack
; slots (add esp,-total). Validated byte-identical to ML for each separately.
	.386
	.model flat, stdcall
	.code
f	proc uses esi edi, x:DWORD
	LOCAL a:DWORD, b:DWORD
	mov esi, x
	mov a, esi
	mov b, esi
	mov eax, b
	ret
f	endp
	end
