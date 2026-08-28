; Written by: Logan Greer
; strings.asm -- MASM documentation operands on string instructions.
; ML writes the string ops with explicit operands that document the implied
; registers/segments; NASM's mnemonics take none. Under --masm the operands are
; accepted and dropped, and the `es:'/`ds:' implied-segment override is cleared
; (it emits no prefix). MOVSD/CMPSD are also SSE mnemonics -- with XMM operands
; they must stay the SSE form, which the last two lines guard.
bits 32
movsd dword ptr es:[edi], dword ptr [esi]   ; -> a5
movsb byte ptr es:[edi], byte ptr [esi]     ; -> a4
stosd dword ptr es:[edi], eax               ; -> ab
stosb byte ptr es:[edi], al                 ; -> aa
lodsd eax, dword ptr [esi]                  ; -> ad
scasd eax, dword ptr es:[edi]               ; -> af
cmpsd dword ptr [esi], dword ptr es:[edi]   ; -> a7
outsb dx, byte ptr [esi]                    ; -> 6e
outsd dx, dword ptr [esi]                   ; -> 6f
insb  byte ptr es:[edi], dx                 ; -> 6c
movsd xmm0, xmm1                            ; -> f2 0f 10 c1  (SSE preserved)
cmpsd xmm0, xmm1, 3                         ; -> f2 0f c2 c1 03
