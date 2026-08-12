; hll_andor.asm -- B6 tail: boolean && / || with short-circuit evaluation.
; Validated byte-identical to ML 6.11 (-Ox for ML's short jumps):
;   .IF a && b   each term jumps to the skip label when false:
;                cmp eax,1; jbe skip; cmp eax,10; jae skip; body; skip:
;   .IF a || b   non-last term jumps INTO the body when true, last skips when
;                false: cmp ecx,1; jz body; cmp ecx,2; jnz skip; body:; ...; skip:
; The golden below is nasm's -O1 default (near forward branches); -Ox reproduces
; ML's short-jump bytes exactly.
; (ML lowers `reg == 0' to `or reg,reg' rather than `cmp reg,0'; nasm keeps the
; general `cmp', a benign encoding tie -- so this fixture avoids `== 0' terms.)
	.386
	.model flat, stdcall
	.code
f	proc
	.if eax > 1 && eax < 10
	  mov eax, 1
	.endif
	.if ecx == 1 || ecx == 2
	  mov ecx, 9
	.endif
	ret
f	endp
	end
