; Written by: Logan Greer
; hll_proc.asm -- B5: PROC with parameters + INVOKE (stdcall).
; Frame, params at [ebp+8+4n], leave/ret <parambytes>, INVOKE push-and-call.
; Validated byte-identical to ML (558bec 8b4508 03450c c9 c20800 | 6a04 6a03 ..).
	.386
	.model flat, stdcall
	.code
addtwo	proc x:DWORD, y:DWORD
	mov eax, x
	add eax, y
	ret
addtwo	endp
main	proc
	invoke addtwo, 3, 4
	ret
main	endp
	end
