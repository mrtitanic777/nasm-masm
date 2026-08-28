; Written by: Logan Greer
; masm_operand_wordops.asm -- MASM word operators in INSTRUCTION OPERANDS.
; and/or/xor/shl/shr/not are translated to ~ & | ^ << >> when they appear as
; operators in an operand, but the LEADING MNEMONIC of the same spelling is
; left intact (a token at line start / after `label:' is the instruction).
; Real Win3.1 kernel uses `and si, not (LA_BUSY + LA_MOVEABLE)' (atom.asm).
bits 16
LA_BUSY     equ 1
LA_MOVEABLE equ 2
	; --- operator in operand position (translated) ---
	and si, not (LA_BUSY + LA_MOVEABLE) ; ~3 = FFFC -> 81 e6 fc ff
	mov ax, LA_BUSY or LA_MOVEABLE      ; 1|2 = 3   -> b8 03 00
	mov bx, 0Ch and 6                   ; 0C&6 = 4  -> bb 04 00
	or  cx, 8 shl 2                     ; 8<<2 = 20 -> 81 c9 20 00
	test dx, 1 xor 3                    ; 1^3 = 2   -> f7 c2 02 00
	mov bp, 40h shr 3                   ; 40>>3 = 8 -> bd 08 00
	; --- same spellings as LEADING MNEMONICS (untouched) ---
	not si                              ; -> f7 d6
	shl ax, 1                           ; -> d1 e0
	and bx, cx                          ; -> 21 cb
	or  dx, bx                          ; -> 09 da
	end
