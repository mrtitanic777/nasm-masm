; objseg.asm -- a MASM-segment-wrapped body for the object-backend check.
; The `NAME segment ... use32 ...' scaffolding must produce a 32-bit code
; segment in -f obj (OMF) and -f win32 (COFF), not only in -f bin. This mixes
; several encodings (reg,reg parity, memory operands, accumulator immediate, a
; string op, [reg*2], a sreg move) so the object path is exercised, not just a
; trivial stub. run.py --objects extracts the _TEXT code and diffs the golden.
        .386
_TEXT   segment dword public use32 'CODE'
        assume  cs:_TEXT
        xor     eax, eax                    ; 33 c0
        mov     ecx, eax                    ; 8b c8
        sub     eax, dword ptr [esi + 28h]  ; 2b 46 28
        and     ax, 3fh                     ; 66 25 3f 00
        lea     edx, [ecx*2]                ; 8d 14 4d 00 00 00 00
        mov     word ptr [esi+4], cs        ; 66 8c 4e 04
        stosd   dword ptr es:[edi], eax     ; ab
        ret                                 ; c3
_TEXT   ends
        end
