; hll_addr.asm -- B5 tail: INVOKE ... ADDR of a global vs a local.
; ADDR is marked so INVOKE distinguishes a label (push its offset, like ML) from
; a stack local (lea its [ebp-N] address, then push). Validated byte-identical
; to ML 6.11 (forms):
;   invoke sink, addr gvar  -> push offset gvar   = 68 <gvar>
;   invoke sink, addr lvar  -> lea eax,[ebp-4]     = 8d 45 fc
;                              push eax            = 50
	.386
	.model flat, stdcall
	.data
gvar	dd 0
	.code
sink	proc arg:dword
	ret
sink	endp
f	proc
	local lvar:dword
	invoke sink, addr gvar
	invoke sink, addr lvar
	ret
f	endp
	end
