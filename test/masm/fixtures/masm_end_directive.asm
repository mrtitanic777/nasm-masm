; Written by: Logan Greer
; masm_end_directive.asm -- MASM `END [entrypoint]' terminates the module.
; Anything after END is not assembled (real MASM drops it); reconstructed
; sources sometimes leave commentary or C pseudo-code past END.  ENDM/ENDP/
; ENDS/ENDIF and a label/EQU named `end' must NOT be mistaken for it.
bits 16
	nop
here:
	inc ax
	end here
    this is not assembly at all -- regs.ip += faultlen;
    if (x < 0) { garbage(); }
    %$&*   +++ ][
