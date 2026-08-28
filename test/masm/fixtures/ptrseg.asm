; Written by: Logan Greer
; ptrseg.asm -- `ptr' size operator and segment overrides.
; MASM's `<size> ptr [mem]' and `seg:[mem]' forms. `ptr' is a no-op noise word
; after the size; a segment override written outside the brackets folds into
; the effective address (ES source override -> 26 prefix).
bits 32
mov eax, dword ptr [esi+28h]        ; -> 8b 46 28
add dword ptr [esi+28h], ecx        ; -> 01 4e 28
mov al,  byte ptr es:[edi]          ; -> 26 8a 07
mov ax,  word ptr [ebx]             ; -> 66 8b 03
inc dword ptr [eax]                 ; -> ff 00
