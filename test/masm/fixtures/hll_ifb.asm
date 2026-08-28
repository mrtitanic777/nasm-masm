; Written by: Logan Greer
; hll_ifb.asm -- B8 tail: IFB/IFNB, IFIDN/IFDIF text conditionals, optional
; macro parameters. MASM macro params are all optional (an omitted one is
; blank), so `%macro NAME 0-n' and IFB tests for the blank. Text comparisons
; map to NASM's %ifidn/%ifnidn (angle brackets stripped). Validated
; byte-identical to ML 6.11:
;   ld eax       -> IFB<src> true  -> xor eax,eax (33 c0); IFIDN eax -> inc (40)
;   ld ecx, 5    -> IFB<src> false -> mov ecx,5 (b9 05..); IFDIF eax -> dec (49)
	.386
	.model flat, stdcall
ld	MACRO dst, src
	  IFB <src>
	    xor dst, dst
	  ELSE
	    mov dst, src
	  ENDIF
	  IFIDN <dst>, <eax>
	    inc dst
	  ENDIF
	  IFDIF <dst>, <eax>
	    dec dst
	  ENDIF
	ENDM
	.code
f	proc
	ld eax
	ld ecx, 5
	ret
f	endp
	end
