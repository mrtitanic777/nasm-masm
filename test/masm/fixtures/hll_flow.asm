; Written by: Logan Greer
; hll_flow.asm -- B6: high-level control flow.
; .IF/.ELSE/.ENDIF, .WHILE/.ENDW, .REPEAT/.UNTIL, .BREAK nested in .IF.
; Conditions lower to CMP + unsigned jcc. With -Ox the jumps are short and
; byte-identical to ML; the golden here is the --masm default (-O1 => near
; jumps, matching the MSVC-compiled corpus), which locks the default behaviour.
	.386
	.model flat, stdcall
	.code
f	proc
	.if eax == 5
	  inc eax
	.else
	  dec eax
	.endif
	.while ecx > 0
	  dec ecx
	  .if ecx == 3
	    .break
	  .endif
	.endw
	.repeat
	  inc edx
	.until edx == 10
	ret
f	endp
	end
