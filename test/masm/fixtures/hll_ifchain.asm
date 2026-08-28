; Written by: Logan Greer
; hll_ifchain.asm -- B6 tail: .ELSEIF chains, flag conditions, signed compares.
; Validated byte-identical to ML 6.11 (with ML's short jumps, i.e. -Ox):
;   .IF/.ELSEIF/.ELSE      each arm: cmp; jbe skip; body; jmp done  (unsigned >)
;   .IF CARRY?             -> jnc skip (73)     .IF ZERO?  -> jnz (75)
;   .IF !ZERO?             -> jz  skip (74)     .IF SIGN?  -> jns (79)
;   .IF SDWORD PTR eax > 5 -> jle skip (7E)     signed; plain > stays jbe (76)
; The golden below is nasm's -O1 default (forward branches near, not short);
; -Ox reproduces ML's short-jump bytes exactly. Both are byte-checked in dev.
	.386
	.model flat, stdcall
	.code
f	proc
	.if eax > 5
	  mov eax, 1
	.elseif eax > 2
	  mov eax, 2
	.else
	  mov eax, 3
	.endif
	.if CARRY?
	  inc eax
	.endif
	.if !ZERO?
	  inc ecx
	.endif
	.if SDWORD PTR eax > 5
	  mov edx, 1
	.endif
	ret
f	endp
	end
