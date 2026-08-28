; Written by: Logan Greer
; hll_instr.asm -- B7 tail: INSTR (substring search position).
; NAME INSTR [start,] <s1>, <s2> -> 1-based position of s2 in s1 (from start,
; default 1), 0 if absent. Literal <...> args are searched at assembly time.
; Validated byte-identical to ML 6.11:
;   INSTR <hello world>,<world> = 7  -> mov eax,7 = b8 07000000
;   INSTR <hello world>,<xyz>   = 0  -> mov ebx,0 = bb 00000000
;   INSTR 3,<ababab>,<ab>       = 3  -> mov ecx,3 = b9 03000000
	.386
	.model flat, stdcall
p1	INSTR	<hello world>, <world>
p2	INSTR	<hello world>, <xyz>
p3	INSTR	3, <ababab>, <ab>
	.code
f	proc
	mov eax, p1
	mov ebx, p2
	mov ecx, p3
	ret
f	endp
	end
