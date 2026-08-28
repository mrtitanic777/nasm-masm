; Written by: Logan Greer
; hll_struct_inst.asm -- B3 tail: static struct instances + instance-member access.
; `label STRUCTTYPE <i0,i1,...>' emits a labelled data block (one field per
; member, in declaration order); a bare `label.member' then means its contents,
; typed by the member's width. Validated byte-identical to ML 6.11 encodings:
;   p PT <11h,22h>          -> data 11000000 22000000
;   r RC <1,2>              -> data 0100 0200
;   mov eax, p.x            -> a1 <disp>          (dword load, moffs)
;   mov p.y, eax            -> a3 <disp+4>        (dword store, moffs)
;   movzx ecx, r.hi         -> 0f b7 0d <r+2>     (word member, size inferred)
;   add p.x, 5              -> 83 05 <p> 05       (dword read-modify-write)
; (Disps here are resolved -f bin absolutes; ML leaves 0 + a reloc. A `<>'
; all-default instance would go to BSS under ML -- not exercised here.)
PT	STRUCT
x	DWORD	?
y	DWORD	?
PT	ENDS
RC	STRUCT
lo	WORD	?
hi	WORD	?
RC	ENDS
	.386
	.model flat, stdcall
	.data
p	PT	<11h, 22h>
r	RC	<1, 2>
	.code
f	proc
	mov eax, p.x
	mov p.y, eax
	movzx ecx, r.hi
	add p.x, 5
	ret
f	endp
	end
