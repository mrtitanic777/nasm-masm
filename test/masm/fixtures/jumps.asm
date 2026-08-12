; jumps.asm -- branch/immediate sizing (the -O1 default under --masm).
; ML optimizes immediate size (imm8 sign-extension) but does NOT shrink a
; forward branch to short: a bare forward `je'/`jmp' stays near (0f 8x / e9),
; while a backward branch whose distance is known uses short, and `short' is
; honored. NASM's own default (-Ox) would shorten the forward branches.
bits 32
back:
        cmp eax, 22h        ; -> 83 f8 22   (imm8 sign-extended, not 3d/81)
        nop
        je   back           ; backward, fits: short 74 xx
        je   fwd            ; forward: near 0f 84 rel32 (not short)
        jmp  fwd            ; forward: near e9 rel32
        jne  short back     ; explicit short honored: 75 xx
        nop
fwd:
        ret
