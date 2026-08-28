; Written by: Logan Greer
; masm_shift_prec.asm -- MASM SHL/SHR precedence (above +/-, unlike NASM <<).
; MASM `a shl b + c' is `(a shl b) + c'; NASM `a << b + c' would be `a<<(b+c)'.
; The front-end emits `a * (1 << b)' / `a / (1 << b)' -- `*'/`/' have MASM's
; precedence, and a shl b == a*2^b, a shr b == a/2^b exactly.  Char literals
; (`'/' shl 8') are handled too (the closing quote is a value-ender).
bits 16
	mov bx, '/' shl 8 + '\'  ; ('/'<<8)+'\' = 2F5C -> bb 5c 2f
	mov ax, 'x' shl 8 or '0' ; 7800|30 = 7830   -> b8 30 78
	mov cx, 3 shl 4 + 1      ; (3<<4)+1 = 49     -> b9 31 00
	mov dx, 100h shr 4       ; 100/16 = 10       -> ba 10 00
	mov si, 1 shl (2+1)      ; 1<<3 = 8          -> be 08 00
	ret
