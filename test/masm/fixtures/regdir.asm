; regdir.asm -- reg,reg direction preference.
; ML encodes a two-register ALU/MOV in the `reg, r/m' (d=1) form: the first
; operand goes in the ModRM.reg field. NASM's default is the `r/m, reg' form.
; e.g. `add eax,ebx' -> 03 c3 (not 01 d8); `xor edx,edx' -> 33 d2 (not 31 d2).
bits 32
add eax, ebx
or  ecx, edx
adc esi, edi
sbb eax, ecx
and ebx, esi
sub ecx, eax
xor edx, edx
cmp eax, ebx
mov ecx, eax
add ax, bx          ; 16-bit form keeps the same direction, with 66 prefix
mov al, bl          ; 8-bit
