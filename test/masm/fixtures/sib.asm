; sib.asm -- [reg*2] addressing form.
; NASM folds [reg*2] (scale 2, no base) into [reg+reg] to drop the disp32; ML
; keeps the index*2+disp32 SIB form. A summed [reg+reg] still folds in both
; modes, and a real base+index*scale is unaffected.
bits 32
lea ecx, [ecx*2]        ; -> 8d 0c 4d 00 00 00 00   (index*2 + disp32, kept)
lea eax, [eax*2]        ; -> 8d 04 45 00 00 00 00
lea edx, [edx+edx]      ; -> 8d 14 12               (summed -> base+index)
lea esi, [eax+ecx*2]    ; -> 8d 34 48               (base + index*2, normal)
lea edi, [ebx*4]        ; -> 8d 3c 9d 00 00 00 00   (scale 4 always index+disp32)
