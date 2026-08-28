; Written by: Logan Greer
; masm_struc_warning.asm -- a MASM constant whose name collides with a NASM
; directive keyword (`WARNING = 0', as in windows.inc) must not break STRUCT.
; Under --masm case-insensitive matching, `warning' would otherwise shadow the
; `warning' directive that stock endstruc uses, making `[warning push]' expand
; to `[0 push]'.  masm.mac overrides endstruc to restore the section directly.
bits 16
WARNING = 0
PALETTEENTRY struc
peRed	db ?
peGreen	db ?
peBlue	db ?
peFlags	db ?
PALETTEENTRY ends
	mov cx, PALETTEENTRY_size        ; = 4          b9 04 00
	mov al, [bx].peFlags             ; [bx+3]       8a 47 03
	mov ah, WARNING                  ; = 0          b4 00
	ret
