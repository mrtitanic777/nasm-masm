; masm_rep_stringop.asm -- MASM writes string ops with an explicit (documentary)
; sized operand: `rep stos dword ptr es:[edi]'.  NASM has only the b/w/d/q-
; suffixed mnemonics, so the front-end picks the suffix from the size keyword,
; drops the operand, and carries any REP-family prefix through.
bits 16
	rep   stos  dword ptr es:[edi]   ; -> rep stosd     66 f3 ab
	rep   movs  dword ptr es:[edi], dword ptr ds:[esi] ; rep movsd  66 f3 a5
	repne scas  word ptr es:[di]     ; -> repne scasw   f2 af
	stos  byte ptr es:[di]           ; -> stosb         aa
	lods  byte ptr es:[si]           ; -> lodsb         ac
