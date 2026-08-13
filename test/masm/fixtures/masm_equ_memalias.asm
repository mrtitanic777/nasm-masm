; masm_equ_memalias.asm -- EQU aliasing a MEMORY operand with member access.
; cmacros frame accessors spell locals as `NAME EQU [ptr].struct.field'
; (e.g. `wParam EQU [pFrame].wp_wParam').  NASM's `equ' rejects a memory
; operand, so this binds textually (%define) after rewriting `].field' member
; access into `+ field]'.  The alias then assembles like the memory operand.
bits 16
struc FR
  .lo: resw 1
  .hi: resw 1
endstruc
wLo	equ [bx].FR.lo
wHi	equ [bx].FR.hi
	mov ax, wLo         ; -> mov ax,[bx+0] = 8b 07
	mov dx, wHi         ; -> mov dx,[bx+2] = 8b 57 02
	ret                 ; -> c3
