; masm_dup_count.asm -- a struct member `<dir> N DUP (?)' where N is a non-literal
; expression (an EQU/= constant or a SIZE operator) must reserve N elements, so
; the struct SIZE comes out right.  Both named and anonymous (padding) members.
bits 16
K equ 4
INNER STRUC
i_a dd ?
i_b dd ?
INNER ENDS                      ; SIZE INNER = 8
OUTER STRUC
        DB SIZE INNER DUP (?)   ; anonymous pad: 8 bytes (SIZE INNER)
o_arr   DW K DUP (?)            ; named: 4*2 = 8 bytes
o_end   DW ?                    ; 2 bytes
OUTER ENDS                      ; SIZE OUTER = 18
	dw SIZE OUTER               ; 18 -> 12 00
	dw SIZE INNER               ; 8  -> 08 00
	.errnz SIZE OUTER - 18      ; passes
	ret
