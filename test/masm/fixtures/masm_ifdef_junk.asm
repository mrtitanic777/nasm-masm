; Written by: Logan Greer
; masm_ifdef_junk.asm -- MASM IFDEF/IFNDEF reads a single symbol; trailing text
; on the line is ignored.  A reconstructed source sometimes forgets the `;'
; before a comment (`ifndef WOW - We thunk this API, too slow'), which must not
; derail the translated %ifndef.
bits 16
ifndef WOW - We thunk this API, it is too slow
	db 1			; taken: WOW is not defined
else
	db 2
endif
ifdef WOW some junk here too
	db 3
else
	db 4			; taken
endif
