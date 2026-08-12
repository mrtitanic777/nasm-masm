;; cmacros_shim.inc -- a NASM-native reimplementation of the core Win16 cmacros
;; procedure & segment macros, for `nasm --masm'.  It provides cProc/parmX/localX
;; /cBegin/cEnd/sBegin/sEnd with the same call interface as Microsoft's
;; cmacros.inc, but built from NASM %macros -- so it sidesteps cmacros' generated-
;; macro (`&macro'/`&endm') machinery, which NASM's numbered-parameter model can't
;; express.  Scope: NEAR/FAR frames, C-order parameters, USES saves.  (Full
;; PASCAL reverse-order offsets and every cmacros knob are future work.)
;;
;; Written as raw NASM directives (this file is our own), so it is not subject to
;; the MASM->NASM front-end translation.

%imacro sBegin 1
    [section %1]
%endmacro
%imacro sEnd 0-1
%endmacro

;; ---- procedure frame state ----
%assign __cp_far   0
%assign __cp_parmo 0            ; running parameter offset from BP
%assign __cp_localo 0          ; running local size (subtracted from SP)
%define __cp_uses

;; cProc name [, flag ...]  -- FAR/NEAR/PUBLIC recognised; the <...> group MASM
;; writes is passed by the front-end as separate tokens, so scan every argument.
%imacro cProc 1-*
    %assign __cp_far 0
    %assign __cp_localo 0
    %define __cp_uses
    ;; MASM writes the flags as a <...> group; the front-end passes it as
    ;; separate tokens with the brackets attached (`<PUBLIC', `FAR>'), so accept
    ;; the bare, leading-`<' and trailing-`>' spellings of each flag.
    %rep %0 - 1
        %rotate 1
        %ifidni %1, far
            %assign __cp_far 1
        %elifidni %1, <far
            %assign __cp_far 1
        %elifidni %1, far>
            %assign __cp_far 1
        %elifidni %1, near
            %assign __cp_far 0
        %elifidni %1, <near
            %assign __cp_far 0
        %elifidni %1, near>
            %assign __cp_far 0
        %endif
    %endrep
    %rotate 1                   ; back to the name
    %if __cp_far
        %assign __cp_parmo 6    ; [bp]=oldbp [bp+2]=ret off [bp+4]=ret seg
    %else
        %assign __cp_parmo 4    ; [bp]=oldbp [bp+2]=ret off
    %endif
%1:
%endmacro

%imacro parmB 1
    %xdefine %1 byte [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 2
%endmacro
%imacro parmW 1
    %xdefine %1 word [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 2
%endmacro
%imacro parmD 1
    %xdefine %1 dword [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 4
%endmacro

%imacro localB 1
    %assign __cp_localo __cp_localo + 2
    %xdefine %1 byte [bp - __cp_localo]
%endmacro
%imacro localW 1
    %assign __cp_localo __cp_localo + 2
    %xdefine %1 word [bp - __cp_localo]
%endmacro
%imacro localD 1
    %assign __cp_localo __cp_localo + 4
    %xdefine %1 dword [bp - __cp_localo]
%endmacro

;; cBegin -- prologue.  cEnd -- epilogue (retf for FAR, ret for NEAR).
%imacro cBegin 0-1
    push bp
    mov bp, sp
    %if __cp_localo > 0
        sub sp, __cp_localo
    %endif
%endmacro
%imacro cEnd 0-1
    %if __cp_localo > 0
        mov sp, bp
    %endif
    pop bp
    %if __cp_far
        retf
    %else
        ret
    %endif
%endmacro
