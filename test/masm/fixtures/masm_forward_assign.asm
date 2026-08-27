; masm_forward_assign.asm -- a MASM `=' is a redefinable numeric equate that
; becomes a NASM `%assign' (evaluated at preprocess time), so it cannot name a
; symbol defined LATER.  Real headers do exactly that (winkern.inc's
; `GA_INTFLAGS = ...GA_CODE_DATA...' sits ABOVE `GA_CODE_DATA EQU 02h').  The
; front-end DEFERS such an assignment -- holds it aside and emits it once every
; forward dependency is defined -- so it resolves to a plain preprocess literal
; with no forward `equ' and no pass instability.  A pre-scanned `const_tab'
; keeps the deferral limited to genuine EQU/= constants: a `=' that names a
; non-constant (a struct size, a label) is emitted immediately, as before.
bits 16
; FWD names A1/A2, both EQU'd BELOW it -> deferred, then resolved to a literal.
FWD = A1 + (A2 shl 8)
A1  EQU 0Dh
A2  EQU 82h
	and ax, not FWD         ; FWD = 0x820D -> and ax,0x7DF2   25 f2 7d
	mov bx, FWD             ; -> mov bx,0x820D                bb 0d 82
; a `=' that only names backward constants is never deferred (a counter):
N = 0
N = N + 1
N = N + 1
	mov cx, N               ; -> mov cx,2                     b9 02 00
	ret
