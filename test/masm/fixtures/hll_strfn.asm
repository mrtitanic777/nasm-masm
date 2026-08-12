; hll_strfn.asm -- B7 tail: string functions SIZESTR / CATSTR / SUBSTR.
; Compile-time text operations that yield numeric or text equates:
;   SIZESTR <text>       -> length of the text (numeric)
;   CATSTR a,b,...        -> concatenated text (a token, via %+)
;   SUBSTR text,start,len -> substring (retokenised so it chains into SIZESTR)
; Validated byte-identical to ML 6.11:
;   sz  SIZESTR <hello>      = 5   -> mov eax,5 = b8 05000000
;   ct  CATSTR <ab>,<cde>          (= "abcde")
;   csz SIZESTR ct           = 5   -> mov ebx,5 = bb 05000000
;   sb  SUBSTR <abcdef>,2,3        (= "bcd")
;   ssz SIZESTR sb           = 3   -> mov ecx,3 = b9 03000000
	.386
	.model flat, stdcall
sz	SIZESTR	<hello>
ct	CATSTR	<ab>, <cde>
csz	SIZESTR	ct
sb	SUBSTR	<abcdef>, 2, 3
ssz	SIZESTR	sb
	.code
f	proc
	mov eax, sz
	mov ebx, csz
	mov ecx, ssz
	ret
f	endp
	end
