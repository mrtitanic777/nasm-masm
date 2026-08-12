; hll_for.asm -- B7 tail: FOR/IRP list iteration and EXITM.
; FOR/IRP lower to a fresh one-arg macro whose body is replayed once per list
; item; EXITM -> %exitmacro. Validated byte-identical to ML 6.11:
;   FOR r, <eax,ebx,ecx> / push r   -> 50 53 51        (push eax/ebx/ecx)
;   IRP v, <1,2,3> / mov eax,v      -> b8 01.. b8 02.. b8 03..
;   emit 0  (IFE 0 -> EXITM)        -> 00               (stops before db 0FFh)
;   emit 7                          -> 07 FF
	.386
	.model flat, stdcall
emit	MACRO n
	  db n
	  IFE n
	    EXITM
	  ENDIF
	  db 0FFh
	ENDM
	.code
f	proc
	FOR r, <eax, ebx, ecx>
	  push r
	ENDM
	IRP v, <1, 2, 3>
	  mov eax, v
	ENDM
	emit 0
	emit 7
	ret
f	endp
	end
