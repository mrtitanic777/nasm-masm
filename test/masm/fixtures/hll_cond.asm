; hll_cond.asm -- B8: conditional assembly.
; MASM IF/IFE/IFDEF/IFNDEF/ELSE/ELSEIF/ENDIF -> NASM %if-family. Validated
; byte-identical to ML: DEBUG=1 selects mov eax,1; IFDEF FOO (undefined) is
; skipped; IFNDEF FOO emits inc ecx -> b8 01000000 41 c3.
	.386
	.model flat, stdcall
DEBUG	=	1
	.code
f	proc
IF DEBUG
	mov eax, 1
ELSE
	mov eax, 2
ENDIF
IFDEF FOO
	inc eax
ENDIF
IFNDEF FOO
	inc ecx
ENDIF
	ret
f	endp
	end
