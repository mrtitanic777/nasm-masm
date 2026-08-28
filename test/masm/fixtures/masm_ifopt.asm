; Written by: Logan Greer
; masm_ifopt.asm -- undefined ?-option switches default to 0 in IF/IFE.
; cmacros-era code tests conditional-assembly option switches (?CHKSTK1,
; ?RIPAUX, ...) that a given module may never set.  MASM treats an undefined
; symbol in IF/IFE as 0; we replicate that for the `?' convention by defaulting
; each such symbol to 0 before the translated %if.  A real option that IS set
; still wins.
bits 16
?OPTSET = 1
	if ?NOOPT           ; undefined -> 0 -> false
	  dw 1111h
	else
	  dw 2222h          ; taken -> 22 22
	endif
	ife ?NOOPT          ; undefined -> 0 -> IFE true
	  dw 3333h          ; taken -> 33 33
	endif
	if ?OPTSET          ; set to 1 -> true
	  dw 4444h          ; taken -> 44 44
	endif
	end
