; masm_lds_farptr.asm -- LES/LDS/etc. load a far pointer whose operand size
; follows the destination register, not the pointer variable's dword width.
; A bare data label means its contents ([var]); for a `dd' far pointer that
; would otherwise be sized `dword', conflicting with `lds dx,...' (16-bit reg).
; The parser leaves the far-pointer load unsized so the register decides.
bits 16
_DATA segment
lpProc	dd	0
_DATA ends
_TEXT segment
	lds	dx, lpProc       ; -> lds dx,[lpProc]   c5 16 [addr]
	les	di, lpProc       ; -> les di,[lpProc]   c4 3e [addr]
_TEXT ends
