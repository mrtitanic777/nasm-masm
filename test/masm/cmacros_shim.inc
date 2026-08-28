; Written by: Logan Greer
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
;;   name&OFFSET / name&BASE equates cmacros code may reference.
;;   <ln>OFFSET is the segment-relative-offset OPERATOR: `codeOFFSET foo' means
;;   "offset of foo" (213 dataOFFSET + 109 codeOFFSET uses).  Bind it to NASM's
;;   OFFSET keyword (case-insensitive) so it yields foo's ADDRESS -- and, for a
;;   DATA label, suppresses the MASM data-label->[mem] conversion that a bare
;;   `foo' would trigger (`mov ax,codeOFFSET szName' must load the address, not
;;   the contents).  <ln>BASE is the segment/selector value.
%imacro createSeg 2-*
    %define __cs_%2 %1
    ;; A segment's USE size (16/32) is fixed when it is DEFINED, from the CPU
    ;; mode in effect here -- NOT from whatever CPU directive is active later
    ;; when a body is emitted into it.  Win3.1 code defines its segments under
    ;; .286 and only later flips to .386p to use 386 instructions inside those
    ;; still-USE16 segments; capture the bitness now so sBegin can re-assert it.
    %assign __csbits_%2 __?MASM_BITS?__
    %idefine %[%2]OFFSET OFFSET
    %ixdefine %[%2]BASE %1
    ;; Declare the real segment so its NAME is a usable selector value
    ;; (`cmp dx, _NRESTEXT') even in a module that never sBegins it.  This
    ;; switches the current section, but createSeg runs at include time before
    ;; any code, and sBegin sets the section again before a body is emitted.
    [segment %1]
%endmacro
;; defGrp group, logicalseg, ...  -- create a real NASM group so the group name
;; is a usable value (`cmp dx, IGROUP').  Map each logical member to its real
;; segment (createSeg's __cs_<logical>).
%imacro defGrp 1-*
    %xdefine __dg_grp %1
    %xdefine __dg_secs
    %rep %0 - 1
        %rotate 1
        %ifdef __cs_%1
            %xdefine __dg_secs __dg_secs %[__cs_%1]
        %else
            %xdefine __dg_secs __dg_secs %1
        %endif
    %endrep
    [group __dg_grp __dg_secs]
%endmacro
%imacro assumes 0-*
%endmacro

%imacro sBegin 1
    ;; Switch to the logical segment's real section AND keep NASM's section-
    ;; reactivation macro (__?SECT?__) current, so a MASM SEGMENT/ENDS pair (or a
    ;; STRUCT) opened inside this body can resume THIS section when it closes.
    %ifdef __cs_%1
        %xdefine __?SECT?__ [section %[__cs_%1]]
    %else
        %xdefine __?SECT?__ [section %1]
    %endif
    __?SECT?__
    ;; Re-assert the segment's definition-time USE size (see createSeg): a switch
    ;; inherits the current live BITS, which a later .386p would have bumped to
    ;; 32 even though this segment is USE16 -- bloating every instruction with a
    ;; spurious operand-size prefix (and pushing `short' jumps out of range).
    %ifdef __csbits_%1
        [bits __csbits_%1]
    %endif
%endmacro
%imacro sEnd 0-1
%endmacro

%imacro DataBegin 0-*
    %xdefine __?SECT?__ [section _DATA]
    __?SECT?__
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
%assign __sav_have 0           ; pending `Save <regs>' count consumed by cCall

;; Save <reg,...> -- record registers the NEXT cCall must preserve across the
;; call (MASM's `save' sets a bitmask consumed by cCall's mpush/mpop).  cCall
;; pushes them before the arguments and pops them (reverse) after the call.
;; Each register is stored in a numbered smacro __sav_reg<i>.
%imacro save 1-*
    %assign __sav_have 0
    %rep %0
        %defstr __sav_s %1
        %strlen __sav_l __sav_s
        %assign __sav_a 1
        %assign __sav_b __sav_l
        %substr __sav_c __sav_s 1,1
        %ifidn __sav_c, '<'
            %assign __sav_a 2
        %endif
        %substr __sav_c __sav_s __sav_l,1
        %ifidn __sav_c, '>'
            %assign __sav_b __sav_b - 1
        %endif
        %if __sav_b >= __sav_a
            %substr __sav_r __sav_s __sav_a,__sav_b-__sav_a+1
            %assign __sav_have __sav_have + 1
            %deftok __sav_tmp __sav_r
            %xdefine __sav_reg%[__sav_have] __sav_tmp
        %endif
        %rotate 1
    %endrep
%endmacro
%idefine Save save

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
    ;; The name may arrive `<...>'-wrapped (cProcVDO passes `cProc <name>,...');
    ;; strip a leading `<' / trailing `>' before emitting the entry label.
    %defstr __cp_ns %1
    %strlen __cp_nn __cp_ns
    %assign __cp_na 1
    %assign __cp_nb __cp_nn
    %substr __cp_nc __cp_ns 1,1
    %ifidn __cp_nc, '<'
        %assign __cp_na 2
    %endif
    %substr __cp_nc __cp_ns __cp_nn,1
    %ifidn __cp_nc, '>'
        %assign __cp_nb __cp_nb - 1
    %endif
    %substr __cp_nr __cp_ns __cp_na,__cp_nb-__cp_na+1
    %deftok __cp_nt __cp_nr
__cp_nt:
%endmacro

;; Parameters are used WITHOUT brackets (`mov ax,p' / `les si,p'), so bind them
;; to the memory operand [bp+off] (unsized -- the instruction infers the size,
;; and a size prefix would break `les').  Locals are used WITH brackets in the
;; source (`mov [v],cl'), so bind them to the bare offset bp-off.
%imacro parmB 1
    %ixdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 2
%endmacro
%imacro parmW 1
    %ixdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 2
%endmacro
;; A DWORD parm/local gets the cmacros far-pointer/dword half accessors.
;; __cp_dhalves name, lowoff -- lowoff is the low word's bp-relative offset
;; (signed): the prefix forms off_<n>/seg_<n> (cmacros.inc `off_&n equ n' /
;; `seg_&n equ n[2]') and the member forms <n>.off/.lo (low) and <n>.sel/.hi/
;; .seg (high), covering the SEGOFF (off/sel) and HILO (lo/hi) layouts.
%imacro __cp_dhalves 2
    %ixdefine off_%1 word [bp + (%2)]
    %ixdefine seg_%1 word [bp + (%2) + 2]
    %ixdefine %1.off word [bp + (%2)]
    %ixdefine %1.lo  word [bp + (%2)]
    %ixdefine %1.sel word [bp + (%2) + 2]
    %ixdefine %1.hi  word [bp + (%2) + 2]
    %ixdefine %1.seg word [bp + (%2) + 2]
%endmacro
%imacro parmD 1
    %ixdefine %1 [bp + __cp_parmo]
    __cp_dhalves %1, __cp_parmo
    %assign __cp_parmo __cp_parmo + 4
%endmacro
;; DP/CP = data/code far pointer (4), Q = qword (8), T = 6-byte far pointer.
%imacro parmDP 1
    %ixdefine %1 [bp + __cp_parmo]
    __cp_dhalves %1, __cp_parmo
    %assign __cp_parmo __cp_parmo + 4
%endmacro
%imacro parmCP 1
    %ixdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 4
%endmacro
%imacro parmQ 1
    %ixdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 8
%endmacro
%imacro parmT 1
    %ixdefine %1 [bp + __cp_parmo]
    %assign __cp_parmo __cp_parmo + 6
%endmacro

;; Scalar locals (B/W/D) are referenced BARE in MASM source (`mov foo,ax'),
;; where MASM resolves the name to its stack slot [bp-off].  Bind them to the
;; bracketed memory operand so the bare reference assembles as memory (the
;; explicit-bracket form `[foo]' does not occur for scalars in the corpus).
%imacro localB 1
    %assign __cp_localo __cp_localo + 2
    %ixdefine %1 [bp - __cp_localo]
%endmacro
%imacro localW 1
    %assign __cp_localo __cp_localo + 2
    %ixdefine %1 [bp - __cp_localo]
%endmacro
%imacro localD 1
    %assign __cp_localo __cp_localo + 4
    %ixdefine %1 [bp - __cp_localo]
    __cp_dhalves %1, -__cp_localo
%endmacro
;; A buffer/pointer local (localV/DP/CP/Q/T) is taken by ADDRESS (`lea bx,buf')
;; or loaded whole (`les di,ptr'), i.e. used WITHOUT brackets -- so bind it to
;; the memory operand [bp-off] (unlike a scalar local, which is used as `[v]'
;; and binds to the bare offset).  The name points at the buffer's low byte.
;; localV name,size -- size may be a MASM hex `NNh'.
%imacro localV 2
    %assign __cp_localo __cp_localo + %2
    %ixdefine %1 [bp - __cp_localo]
%endmacro
%imacro localDP 1
    %assign __cp_localo __cp_localo + 4
    %ixdefine %1 [bp - __cp_localo]
    __cp_dhalves %1, -__cp_localo
%endmacro
%imacro localCP 1
    %assign __cp_localo __cp_localo + 4
    %ixdefine %1 [bp - __cp_localo]
%endmacro
%imacro localQ 1
    %assign __cp_localo __cp_localo + 8
    %ixdefine %1 [bp - __cp_localo]
%endmacro
%imacro localT 1
    %assign __cp_localo __cp_localo + 6
    %ixdefine %1 [bp - __cp_localo]
%endmacro

;; cBegin -- prologue.  cEnd -- epilogue (retf for FAR, ret for NEAR).
;; `cBegin nogen' / `cEnd nogen' suppress the standard frame: the body sets up
;; no BP frame and returns on its own (often by jumping to shared exit code), so
;; emitting push bp/mov bp,sp or pop bp/ret here would be wrong bytes -- and,
;; accumulated across a run of nogen procs, would push `short' jumps out of
;; range (3ginterf).  Honor it.
%imacro cBegin 0-1
    %ifidni %1, nogen
    %else
        push bp
        mov bp, sp
        %if __cp_localo > 0
            sub sp, __cp_localo
        %endif
    %endif
%endmacro
%imacro cEnd 0-1
    %ifidni %1, nogen
    %else
        %if __cp_localo > 0
            mov sp, bp
        %endif
        pop bp
        %if __cp_far
            retf
        %else
            ret
        %endif
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

;; DKernelThunk api [, framebytes] -- a WOW (Windows-on-Windows) 16-bit API
;; thunk stub.  The real macro lives in a WOW kernel include (wowkc.inc) absent
;; from this source tree; this functional equivalent lets kthunks.asm assemble:
;; publish the entry, tag it with a running thunk id, far-call the WOW32
;; dispatcher (WOW16Call), and return (cleaning `framebytes' of args if given).
;; Not byte-validated -- kthunks is not in the parity corpus.
%assign __dkt_id 0
%imacro DKernelThunk 1-2
    global %1
  %1:
    mov ax, __dkt_id
    %assign __dkt_id __dkt_id + 1
    call WOW16Call
    %if %0 >= 2
        retf %2
    %else
        retf
    %endif
%endmacro
;; KernelThunk -- the non-"D" sibling (GetProfileString et al); same stub.
%imacro KernelThunk 1-2
    global %1
  %1:
    mov ax, __dkt_id
    %assign __dkt_id __dkt_id + 1
    call WOW16Call
    %if %0 >= 2
        retf %2
    %else
        retf
    %endif
%endmacro

;; cCall func, <arg,...> [, convention] -- push args left-to-right (PASCAL) then
;; call.  The <...> group arrives bracket-split (`<ax', `bx>'); strip and push.
%imacro cCall 1-*
    %xdefine __cc_fn %1
    ;; A pending `Save <regs>' -- push them (in order) before the arguments.
    %if __sav_have > 0
        %assign __cc_svi 1
        %rep __sav_have
            push __sav_reg%[__cc_svi]
            %assign __cc_svi __cc_svi + 1
        %endrep
    %endif
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
            ;; A `farPtr'-declared argument (a bare identifier) pushes its two
            ;; words; any other argument pushes one word.  Only test `%ifdef' on
            ;; an argument that is a clean identifier -- otherwise the
            ;; constructed `__isfp_<arg>' is not a legal macro name (`[bp+4]').
            %assign __cc_id 1
            %assign __cc_i __cc_a
            %rep __cc_b - __cc_a + 1
                %substr __cc_ch __cc_s __cc_i,1
                %if !((__cc_ch >= 'a' && __cc_ch <= 'z') || \
                      (__cc_ch >= 'A' && __cc_ch <= 'Z') || \
                      (__cc_ch >= '0' && __cc_ch <= '9') || \
                      __cc_ch == '_' || __cc_ch == '@' || \
                      __cc_ch == '?' || __cc_ch == '$')
                    %assign __cc_id 0
                %endif
                %assign __cc_i __cc_i + 1
            %endrep
            %if __cc_id
                %ifdef __isfp_%[__cc_t]
                    push __fp1_%[__cc_t]
                    push __fp2_%[__cc_t]
                %else
                    push __cc_t
                %endif
            %else
                push __cc_t
            %endif
        %endif
    %endrep
    call __cc_fn
    ;; Restore a pending `Save <regs>' -- pop them in reverse after the call.
    %if __sav_have > 0
        %assign __cc_svi __sav_have
        %rep __sav_have
            pop __sav_reg%[__cc_svi]
            %assign __cc_svi __cc_svi - 1
        %endrep
        %assign __sav_have 0
    %endif
%endmacro
;; farPtr n,s,o -- declare `n' as a far pointer whose value is the pair (s,o).
;; MASM generates a push-macro named `n'; here `n' is instead recorded as a
;; marker (__isfp_n) plus its two words, which cCall expands to `push s / push o'
;; when `n' appears in its argument list.  (Every kernel use is a cCall arg.)
%imacro farPtr 3
    %define  __isfp_%1 1
    %xdefine __fp1_%1 %2
    %xdefine __fp2_%1 %3
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
