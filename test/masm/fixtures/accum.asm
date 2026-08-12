; accum.asm -- 16-bit accumulator-immediate preference.
; For a 16-bit `<alu> AX, imm' ML uses the accumulator opcode with a 16-bit
; immediate (AND AX,imm -> 25 iw, CMP -> 3d, OR -> 0d ...), which ties the
; length of the sign-extended-imm8 group form (83 /r ib) that NASM prefers.
; Only 16-bit AX ties: 8-bit AL already prefers the accumulator form and
; 32-bit EAX prefers the shorter imm8 group form -- both shown here as the
; cases that must NOT change.
bits 32
and ax, 3fh         ; -> 66 25 3f 00   (accumulator, not 66 83 e0 3f)
cmp ax, 1           ; -> 66 3d 01 00
or  ax, 8           ; -> 66 0d 08 00
add ax, 100h        ; -> 66 05 00 01
sub ax, 0ffe7h      ; -> 66 2d e7 ff
and eax, 22h        ; -> 83 e0 22      (32-bit: group imm8 stays, shorter)
and al, 3fh         ; -> 24 3f         (8-bit: accumulator already)
and bx, 3fh         ; -> 66 83 e3 3f   (non-accumulator reg: group form)
