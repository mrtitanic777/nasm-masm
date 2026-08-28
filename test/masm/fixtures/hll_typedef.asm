; Written by: Logan Greer
; hll_typedef.asm -- B3 tail: TYPEDEF type aliases.
; TYPEDEF makes a name usable as a data directive and gives it a size:
;   TYPEDEF <primitive>   -> that directive + size
;   TYPEDEF PTR x / PROTO -> a pointer: dd / 4 (32-bit flat)
; Validated byte-identical to ML 6.11 (with TYPEDEF after .MODEL, so a pointer
; is the 32-bit 4-byte form):
;   hnd UINT32 7          -> data 07000000     (UINT32 = DWORD)
;   flag BFLAG 1          -> data 01           (BFLAG = BYTE)
;   mov eax, hnd          -> a1 <hnd>          (dword contents)
;   mov bl, flag          -> 8a 1d <flag>      (byte contents)
;   mov ecx, SIZEOF PVOID -> b9 04000000       (pointer = 4)
;   mov edx, SIZEOF BFLAG -> ba 01000000       (byte = 1)
	.386
	.model flat, stdcall
PVOID	TYPEDEF	PTR DWORD
UINT32	TYPEDEF	DWORD
BFLAG	TYPEDEF	BYTE
	.data
hnd	UINT32	7
flag	BFLAG	1
	.code
f	proc
	mov eax, hnd
	mov bl, flag
	mov ecx, SIZEOF PVOID
	mov edx, SIZEOF BFLAG
	ret
f	endp
	end
