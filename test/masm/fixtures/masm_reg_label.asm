; masm_reg_label.asm -- MASM code freely uses names that NASM reserves as
; registers as ordinary labels.  Two mechanisms:
;  * 64-bit GPR names (rax..r15) cannot be registers in 16/32-bit code, so the
;    tokeniser returns them as identifiers under --masm (all contexts).
;  * FPU stack names st0..st7 ARE valid registers, so only a `stN:' label or a
;    `stN' operand of a NON-FPU instruction (FPU mnemonics start with `f') is
;    escaped to `$stN'; a real `fadd st1' keeps the register.
;  * dr8..dr31 / cr9..cr31 are NASM register names but not real 16/32-bit
;    registers, so `dr20:' / `jz dr20' are labels too.
bits 16
	jz   rax               ; rax is a label (forward jz, near under -O1)
	jnz  st1               ; st1 is a label
	jz   dr20              ; dr20 is a label
	jmp  short rax
	nop
dr20:
	nop
rax:
	fld  st1               ; real FPU register    d9 c1
	fadd st0, st1          ; real FPU registers   d8 c1
st1:
	mov  ax, offset rax    ; label address
	ret
