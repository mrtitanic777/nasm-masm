; Written by: Logan Greer
; masm_size_var.asm -- MASM `SIZE var' on a data variable is its total byte
; count.  For a labeled buffer/array (`name <dir> N DUP(...)') the fork emits
; `name_size equ $ - name', so SIZE (rewritten to name_size) yields the bytes --
; ldaux's `cFake32BitModuleName DW SIZE szFake32BitModuleName'.
bits 16
buf   DB 128 DUP(0)      ; 128 bytes
arr   DW 10 DUP(?)       ; 20 bytes
pairs DD 4 DUP(0)        ; 16 bytes
	dw SIZE buf         ; 0080
	dw SIZE arr         ; 0014
	dw SIZE pairs       ; 0010
	dw SIZE arr + 2     ; 0016  (mid-expression)
