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

;; ---- segments & groups ----
;; createSeg realname, logicalname, align, combine, class, group
;;   Declares a segment and binds the logical name used by sBegin, plus the
;;   name&OFFSET / name&BASE equates cmacros code may reference (0 in flat/obj).
%imacro createSeg 2-*
    %define __cs_%2 %1
    %xdefine %[%2]OFFSET 0
    %xdefine %[%2]BASE %1
%endmacro
%imacro defGrp 0-*
%endmacro
%imacro assumes 0-*
%endmacro

%imacro sBegin 1
    %ifdef __cs_%1
        [section %[__cs_%1]]
    %else
        [section %1]
    %endif
%endmacro
%imacro sEnd 0-1
%endmacro

%imacro DataBegin 0-*
    [section _DATA]
%endmacro
%imacro DataEnd 0-*
%endmacro

;; global<T>/static<T> name [, init [, count]] -- declare a typed data cell.
;; global<T> also publishes the symbol.  init `?' (or omitted) means zero-fill;
;; count repeats the cell.  __cs_ddef does the body; the wrappers pick the unit.
%imacro __cs_ddef 3-5
    ;; %1 = directive (db/dw/dd), %2 = name, %3 = count, %4 = init(optional).
    ;; The label MUST share the data directive's line: that is what registers
    ;; the MASM data-label type (so a bare `curTDB' reads as `[curTDB]' and
    ;; `mov ds,curTDB' assembles).  A label on its own line does not register.
    %if %0 >= 4
        %ifidn %4, ?
            %2: times %3 %1 0
        %else
            %2: times %3 %1 %4
        %endif
    %else
        %2: times %3 %1 0
    %endif
%endmacro
%imacro globalB 1-3
    global %1
    %if %0 >= 3
        __cs_ddef db, %1, %3, %2
    %elif %0 >= 2
        __cs_ddef db, %1, 1, %2
    %else
        __cs_ddef db, %1, 1
    %endif
%endmacro
%imacro globalW 1-3
    global %1
    %if %0 >= 3
        __cs_ddef dw, %1, %3, %2
    %elif %0 >= 2
        __cs_ddef dw, %1, 1, %2
    %else
        __cs_ddef dw, %1, 1
    %endif
%endmacro
%imacro globalD 1-3
    global %1
    %if %0 >= 3
        __cs_ddef dd, %1, %3, %2
    %elif %0 >= 2
        __cs_ddef dd, %1, 1, %2
    %else
        __cs_ddef dd, %1, 1
    %endif
%endmacro
%imacro staticB 1-3
    %if %0 >= 3
        __cs_ddef db, %1, %3, %2
    %elif %0 >= 2
        __cs_ddef db, %1, 1, %2
    %else
        __cs_ddef db, %1, 1
    %endif
%endmacro
%imacro staticW 1-3
    %if %0 >= 3
        __cs_ddef dw, %1, %3, %2
    %elif %0 >= 2
        __cs_ddef dw, %1, 1, %2
    %else
        __cs_ddef dw, %1, 1
    %endif
%endmacro
%imacro staticD 1-3
    %if %0 >= 3
        __cs_ddef dd, %1, %3, %2
    %elif %0 >= 2
        __cs_ddef dd, %1, 1, %2
    %else
        __cs_ddef dd, %1, 1
    %endif
%endmacro

;; ---- externals & typed labels (obj backend) ----
;; extern<T> takes a <name,...> list (bracket-split by the front-end); the type
;; suffix is irrelevant to the symbol, so one helper serves them all.  Strip the
;; angle brackets and any `:type', then `extern' each name.
%imacro __cs_extern 1-*
    %rep %0
        %defstr __ce_s %1
        %strlen __ce_n __ce_s
        %assign __ce_a 1
        %assign __ce_b __ce_n
        %substr __ce_c __ce_s 1,1
        %ifidn __ce_c, '<'
            %assign __ce_a 2
        %endif
        %substr __ce_c __ce_s __ce_n,1
        %ifidn __ce_c, '>'
            %assign __ce_b __ce_b - 1
        %endif
        %if __ce_b >= __ce_a
            %substr __ce_r __ce_s __ce_a,__ce_b-__ce_a+1
            %deftok __ce_t __ce_r
            extern __ce_t
        %endif
        %rotate 1
    %endrep
%endmacro
%idefine externFP __cs_extern
%idefine externNP __cs_extern
%idefine externP  __cs_extern
%idefine externW  __cs_extern
%idefine externD  __cs_extern
%idefine externB  __cs_extern
%idefine externA  __cs_extern
;; label<T> [<flags,name>] -- emit a label.  The name arrives bracket-split and
;; may be preceded by PUBLIC/etc; strip brackets and skip the known flags, then
;; define the remaining token as a label.
%imacro __cs_label 1-*
    %rep %0
        %defstr __cl_s %1
        %strlen __cl_n __cl_s
        %assign __cl_a 1
        %assign __cl_b __cl_n
        %substr __cl_c __cl_s 1,1
        %ifidn __cl_c, '<'
            %assign __cl_a 2
        %endif
        %substr __cl_c __cl_s __cl_n,1
        %ifidn __cl_c, '>'
            %assign __cl_b __cl_b - 1
        %endif
        %if __cl_b >= __cl_a
            %substr __cl_r __cl_s __cl_a,__cl_b-__cl_a+1
            %deftok __cl_t __cl_r
            %ifnidni __cl_t, PUBLIC
            %ifnidni __cl_t, PRIVATE
                __cl_t:
            %endif
            %endif
        %endif
        %rotate 1
    %endrep
%endmacro
%idefine LabelFP __cs_label
%idefine labelFP __cs_label
%idefine labelNP __cs_label
%idefine labelW  __cs_label
%idefine labelB  __cs_label
%idefine labelD  __cs_label

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

;; Parameters are used WITHOUT brackets (`mov ax,p' / `les si,p'), so bind them
;; to the memory operand [bp+off] (unsized -- the instruction infers the size,
;; and a size prefix would break `les').  Locals are used WITH brackets in the
;; source (`mov [v],cl'), so bind them to the bare offset bp-off.
%imacro parmB 1
    %xdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 2
%endmacro
%imacro parmW 1
    %xdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 2
%endmacro
%imacro parmD 1
    %xdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 4
%endmacro
;; DP/CP = data/code far pointer (4), Q = qword (8), T = 6-byte far pointer.
%imacro parmDP 1
    %xdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 4
%endmacro
%imacro parmCP 1
    %xdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 4
%endmacro
%imacro parmQ 1
    %xdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 8
%endmacro
%imacro parmT 1
    %xdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 6
%endmacro

%imacro localB 1
    %assign __cp_localo __cp_localo + 2
    %xdefine %1 bp - __cp_localo
%endmacro
%imacro localW 1
    %assign __cp_localo __cp_localo + 2
    %xdefine %1 bp - __cp_localo
%endmacro
%imacro localD 1
    %assign __cp_localo __cp_localo + 4
    %xdefine %1 bp - __cp_localo
%endmacro
;; A buffer/pointer local (localV/DP/CP/Q/T) is taken by ADDRESS (`lea bx,buf')
;; or loaded whole (`les di,ptr'), i.e. used WITHOUT brackets -- so bind it to
;; the memory operand [bp-off] (unlike a scalar local, which is used as `[v]'
;; and binds to the bare offset).  The name points at the buffer's low byte.
;; localV name,size -- size may be a MASM hex `NNh'.
%imacro localV 2
    %assign __cp_localo __cp_localo + %2
    %xdefine %1 [bp - __cp_localo]
%endmacro
%imacro localDP 1
    %assign __cp_localo __cp_localo + 4
    %xdefine %1 [bp - __cp_localo]
%endmacro
%imacro localCP 1
    %assign __cp_localo __cp_localo + 4
    %xdefine %1 [bp - __cp_localo]
%endmacro
%imacro localQ 1
    %assign __cp_localo __cp_localo + 8
    %xdefine %1 [bp - __cp_localo]
%endmacro
%imacro localT 1
    %assign __cp_localo __cp_localo + 6
    %xdefine %1 [bp - __cp_localo]
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

;; cProcVDO -- a cProc variant used by some kernel modules; same frame here.
%idefine cProcVDO cProc

;; ---- misc cmacros helpers ----
;; outif flag [, default [, msg]] -- define flag to default if unset, then
;; normalise to 0/1.  (Arguments are textual under --masm, so the flag NAME is
;; passed even when it is already defined.)
%imacro outif 1-*
    %ifndef %1
        %if %0 >= 2
            %assign %1 %2
        %else
            %assign %1 0
        %endif
    %endif
    %if %1
        %assign %1 1
    %endif
%endmacro

;; errnz <expr> / errnz expr -- assemble-time assertion (error if nonzero).
%imacro errnz 1-*
%endmacro
%imacro errn$ 1-*
%endmacro

;; cCall func, <arg,...> [, convention] -- push args left-to-right (PASCAL) then
;; call.  The <...> group arrives bracket-split (`<ax', `bx>'); strip and push.
%imacro cCall 1-*
    %xdefine __cc_fn %1
    %rep %0 - 1
        %rotate 1
        %defstr __cc_s %1
        %strlen __cc_n __cc_s
        %assign __cc_a 1
        %assign __cc_b __cc_n
        %substr __cc_c __cc_s 1,1
        %ifidn __cc_c, '<'
            %assign __cc_a 2
        %endif
        %substr __cc_c __cc_s __cc_n,1
        %ifidn __cc_c, '>'
            %assign __cc_b __cc_b - 1
        %endif
        %if __cc_b >= __cc_a
            %substr __cc_r __cc_s __cc_a,__cc_b-__cc_a+1
            %deftok __cc_t __cc_r
            push __cc_t
        %endif
    %endrep
    call __cc_fn
%endmacro

;; regPtr name, seg, off -- a far-pointer register pair.  (Functional subset:
;; bind the name to the offset register; the segment part of a far push is a
;; follow-on.)
%imacro regPtr 3
    %define %1 %3
%endmacro
%imacro labelVDO 1-*
%1:
%endmacro
