; Written by: Logan Greer
; sreg.asm -- redundant operand-size prefix on segment-register memory moves.
; A segment register is 16-bit, so ML prefixes a sreg<->memory move in 32-bit
; code with 66; NASM drops the redundant prefix (8c/8e are inherently 16-bit)
; and even ignores an explicit `word ptr'. Under --masm the o16 prefix is
; forced for these memory forms. Register moves size from the GPR and agree.
bits 32
mov word ptr [esi+4], cs    ; -> 66 8c 4e 04
mov word ptr [esi+0ah], ss  ; -> 66 8c 56 0a
mov es, word ptr [ebp-4]    ; -> 66 8e 45 fc
mov fs, word ptr [ebp+3ch]  ; -> 66 8e 65 3c
