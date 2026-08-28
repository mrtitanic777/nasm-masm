; Written by: Logan Greer
; hll_record.asm -- B3 tail: RECORD (bit-packed fields).
; `NAME RECORD f:w, ...' packs fields most-significant-first into one integer
; (byte/word/dword by total width). Each field name is its shift count; MASK f
; is the field's bit mask, WIDTH f its width; an instance packs its values.
; Validated byte-identical to ML 6.11 (DATE = yr:7 mo:4 dy:5 = 16 bits = word):
;   today DATE <100,6,15> -> (100<<9)|(6<<5)|15 = 0C8CFh  -> data cf c8
;   mov ax, today   -> 66 a1 <today>          (word record load)
;   mov bx, MASK mo -> 66 bb e0 01            (mask 01E0h, bits 8-5)
;   mov cx, MASK yr -> 66 b9 00 fe            (mask FE00h, bits 15-9)
;   mov dl, mo      -> b2 05                   (mo shift count = 5)
;   mov dh, WIDTH mo-> b6 04                   (mo width = 4)
DATE	RECORD	yr:7, mo:4, dy:5
	.386
	.model flat, stdcall
	.data
today	DATE	<100, 6, 15>
	.code
f	proc
	mov ax, word ptr today
	mov bx, MASK mo
	mov cx, MASK yr
	mov dl, mo
	mov dh, WIDTH mo
	ret
f	endp
	end
