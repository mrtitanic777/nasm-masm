; masm_localv_size.asm -- a cmacros `localV name, size' often gives the size as
; a `<...>'-wrapped expression (`<SIZE EXE_HDR>', `<SIZE S + 2>') so its space
; does not split the macro argument.  NASM separates macro args on commas, not
; the `<>' group, so the front-end unwraps it (`localV n, SIZE S') -- the size
; then flows through the SIZE-operator rewrite and reaches localV as one arg.
bits 16
S struc
s_a dw ?
s_b dd ?
S ends
%assign __off 0
%imacro localV 2
    %assign __off __off + (%2)
    %ixdefine %1 [bp - __off]
%endmacro
	localV buf, <SIZE S>        ; SIZE S = 6 -> [bp-6]
	lea bx, buf                 ; lea bx,[bp-6]   8d 5e fa
	localV key, <SIZE S + 2>    ; 6+2=8 -> [bp-14]
	lea di, key                 ; lea di,[bp-14]  8d 7e f2
	ret
