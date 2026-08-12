; hll_macroparam.asm -- MASM macro parameter semantics: a parameter used as the
; target of `name = value' sets the PASSED symbol (textual substitution), which
; the cmacros.inc `outif' idiom relies on. Params are substituted as %1..%N
; throughout the body (not via `%define'), so `ifndef name'/`name = value' work.
; Validated byte-identical to ML 6.11:
;   setflag foo,5 -> foo undefined -> foo=5 -> (if foo) foo=1  => mov eax,1
;   setflag bar,0 -> bar undefined -> bar=0 -> (if bar false)  => mov ebx,0
	.386
	.model flat, stdcall
setflag	MACRO name, val
	ifndef name
	  name = val
	endif
	if name
	  name = 1
	endif
	ENDM
	.code
f	proc
	setflag foo, 5
	setflag bar, 0
	mov eax, foo
	mov ebx, bar
	ret
f	endp
	end
