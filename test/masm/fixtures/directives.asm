; Written by: Logan Greer
; directives.asm -- the machine-generated directive scaffolding.
; Exercises the masm package end to end: listing directives (title/page) and
; ASSUME are dropped, `.386' and the USE32 segment attribute set 32-bit code,
; `NAME segment'/`NAME ends'/`end' map to a NASM section. The body must
; assemble as 32-bit (no 66/67 prefixes) with reg,reg parity applied.
title   mymodule
page    ,132
        .386
_TEXT   segment dword public use32 'CODE'
        assume  cs:_TEXT
        xor eax, eax        ; -> 33 c0   (reg,reg parity, 32-bit: no prefixes)
        mov ah, 4           ; -> b4 04
        sub eax, dword ptr [esi + 28h]  ; -> 2b 46 28
        ret                 ; -> c3
_TEXT   ends
        end
