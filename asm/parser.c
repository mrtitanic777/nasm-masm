/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright 1996-2025 The NASM Authors - All Rights Reserved */
/* --masm MASM-compatibility mode Copyright 2026 Logan Greer */

/*
 * parser.c   source line parser for the Netwide Assembler
 */

#include "compiler.h"

#include "nctype.h"

#include "nasm.h"
#include "insns.h"
#include "nasmlib.h"
#include "error.h"
#include "stdscan.h"
#include "eval.h"
#include "parser.h"
#include "floats.h"
#include "assemble.h"
#include "tables.h"


static int end_expression_next(void);

static struct tokenval tokval;

/*
 * MASM data-label semantics (--masm).
 *
 * MASM treats a data label as its memory *contents*; NASM treats it as its
 * *address*.  To emulate MASM, we record each data label's element size when
 * its DB/DW/DD/... definition is parsed, and in the operand parser a bare
 * reference to such a label becomes a sized memory reference `[label]'.
 * `OFFSET label' keeps NASM's bare-label (address) semantics.
 */
static struct hash_table masm_types;    /* label name -> (intptr_t)element size */

/*
 * Fold a symbol to the canonical lower case used as the masm_types key, so the
 * data-label type survives the case variation MASM allows (`CurTDB' vs
 * `curTDB').  Matches the label-table folding in labels.c:find_label().
 * Returns a pointer into `buf' (caller-provided, >= strlen(name)+1).
 */
static const char *masm_fold(const char *name, char *buf, size_t bufsz)
{
    size_t i;
    for (i = 0; name[i] && i + 1 < bufsz; i++)
        buf[i] = nasm_tolower(name[i]);
    buf[i] = '\0';
    return buf;
}

static void masm_type_set(const char *name, int size)
{
    struct hash_insert hi;
    char fold[256];
    const char *key = masm_fold(name, fold, sizeof fold);
    void **loc = hash_find(&masm_types, key, &hi);
    if (loc)
        *loc = (void *)(intptr_t)size;
    else
        hash_add(&hi, nasm_strdup(key), (void *)(intptr_t)size);
}

static int masm_type_get(const char *name)
{
    char fold[256];
    const char *key = masm_fold(name, fold, sizeof fold);
    void **loc = hash_find(&masm_types, key, NULL);
    return loc ? (int)(intptr_t)*loc : 0;
}

/* Public entry (see nasm.h): let the --masm preprocessor record the type of a
 * typed EXTRN / externW-D-B so a cross-module data label reads as its contents.
 * Idempotent, and the table persists across passes, so repeated calls are fine.
 */
void masm_type_note(const char *name, int size)
{
    if (size > 0)
        masm_type_set(name, size);
}

/* Public entry (see nasm.h): the registered MASM data-type size of `name', or
 * 0 if none.  Lets the --masm preprocessor tell an already-defined member label
 * (a struct instance's `p.x') from a far-pointer reinterpret (`ptr.sel'). */
int masm_type_query(const char *name)
{
    return masm_type_get(name);
}

static opflags_t masm_size_bits(int size)
{
    switch (size) {
    case 1:  return BITS8;
    case 2:  return BITS16;
    case 4:  return BITS32;
    case 8:  return BITS64;
    case 10: return BITS80;
    case 16: return BITS128;
    default: return 0;
    }
}

static int masm_dataop_size(int opcode)
{
    switch (opcode) {
    case I_DB: return 1;
    case I_DW: return 2;
    case I_DD: return 4;
    case I_DQ: return 8;
    case I_DT: return 10;
    case I_DO: return 16;
    default:   return 0;
    }
}

/*
 * Human-readable description of a token, intended for error messages.
 * The resulting string needs to be freed.
 */
static char *tokstr(const struct tokenval *tok)
{
    if (tok->t_type == TOKEN_EOS) {
        return nasm_strdup("end of line");
    } else if (tok->t_len) {
        return nasm_asprintf("`%.*s'", tok->t_len, tok->t_start);
    } else {
        return nasm_strdup("invalid token");
    }
}

static void process_size_override(insn *result, operand *op)
{
    if (tasm_compatible_mode) {
        switch (tokval.t_integer) {
            /* For TASM compatibility a size override inside the
             * brackets changes the size of the operand, not the
             * address type of the operand as it does in standard
             * NASM syntax. Hence:
             *
             *  mov     eax,[DWORD val]
             *
             * is valid syntax in TASM compatibility mode. Note that
             * you lose the ability to override the default address
             * type for the instruction, but we never use anything
             * but 32-bit flat model addressing in our code.
             */
        case S_BYTE:
            op->type |= BITS8;
            break;
        case S_WORD:
            op->type |= BITS16;
            break;
        case S_DWORD:
        case S_LONG:
            op->type |= BITS32;
            break;
        case S_QWORD:
            op->type |= BITS64;
            break;
        case S_TWORD:
            op->type |= BITS80;
            break;
        case S_OWORD:
            op->type |= BITS128;
            break;
        default:
            nasm_nonfatal("invalid operand size specification");
            break;
        }
    } else {
        /* Standard NASM compatible syntax */
        switch (tokval.t_integer) {
        case S_NOSPLIT:
            op->eaflags |= EAF_TIMESTWO;
            break;
        case S_REL:
            op->eaflags |= EAF_REL;
            break;
        case S_ABS:
            op->eaflags |= EAF_ABS;
            break;
        case S_BYTE:
            op->disp_size = 8;
            op->eaflags |= EAF_BYTEOFFS;
            break;
        case P_A16:
        case P_A32:
        case P_A64:
            if (result->prefixes[PPS_ASIZE] &&
                result->prefixes[PPS_ASIZE] != tokval.t_integer)
                nasm_nonfatal("conflicting address size specifications");
            else
                result->prefixes[PPS_ASIZE] = tokval.t_integer;
            break;
        case S_WORD:
            op->disp_size = 16;
            op->eaflags |= EAF_WORDOFFS;
            break;
        case S_DWORD:
        case S_LONG:
            op->disp_size = 32;
            op->eaflags |= EAF_WORDOFFS;
            break;
        case S_QWORD:
            op->disp_size = 64;
            op->eaflags |= EAF_WORDOFFS;
            break;
        default:
            nasm_nonfatal("invalid size specification in"
                          " effective address");
            break;
        }
    }
}

/*
 * Braced keywords are parsed here.  opmask and zeroing
 * decorators can be placed in any order.  e.g. zmm1 {k2}{z} or zmm2
 * {z}{k3} decorator(s) are placed at the end of an operand.
 */
static bool parse_decorators(decoflags_t *decoflags)
{
    int i, j;

    i = tokval.t_type;

    while (true) {
        switch (i) {
        case TOKEN_OPMASK:
            if (*decoflags & OPMASK_MASK) {
                nasm_nonfatal("opmask k%"PRIu64" is already set",
                              *decoflags & OPMASK_MASK);
                *decoflags &= ~OPMASK_MASK;
            }
            *decoflags |= VAL_OPMASK(nasm_regvals[tokval.t_integer]);
            break;
        case TOKEN_DECORATOR:
            j = tokval.t_integer;
            switch (j) {
            case BRC_Z:
                *decoflags |= Z_MASK;
                break;
            case BRC_1TO2:
            case BRC_1TO4:
            case BRC_1TO8:
            case BRC_1TO16:
            case BRC_1TO32:
                *decoflags |= BRDCAST_MASK | VAL_BRNUM(j - BRC_1TO2);
                break;
            default:
                nasm_nonfatal("{%s} is not an expected decorator",
                              tokval.t_charptr);
                break;
            }
            break;
        case ',':
        case TOKEN_EOS:
            return false;
        default:
            nasm_nonfatal("only a series of valid decorators expected");
            return true;
        }
        i = stdscan(NULL, &tokval);
    }
}

static inline unused_func
const expr *next_expr(const expr *e, const expr **next_list)
{
    e++;
    if (!e->type) {
        if (next_list) {
            e = *next_list;
            *next_list = NULL;
        } else {
            e = NULL;
        }
    }
    return e;
}

static inline void init_operand(operand *op, unsigned int opidx)
{
    nasm_zero(*op);

    op->basereg  = -1;
    op->indexreg = -1;
    op->segment  = NO_SEG;
    op->wrt      = NO_SEG;
    op->opidx    = opidx;
}

static int parse_mref(operand *op, const expr *e)
{
    int b, i, s;        /* basereg, indexreg, scale */
    int64_t o;          /* offset */

    b = op->basereg;
    i = op->indexreg;
    s = op->scale;
    o = op->offset;

    for (; e->type; e++) {
        if (!e->value)          /* Operand multiplied by zero */
            continue;

        if (e->type <= EXPR_REG_END) {
            opflags_t flags = nasm_reg_flags[e->type];
            bool is_gpr = is_class(REG_GPR, flags);

            if (is_gpr && e->value == 1 && b == -1) {
                /* It can be basereg */
                b = e->type;
            } else if (i == -1) {
                /* Must be index register */
                i = e->type;
                s = e->value;
            } else {
                if (b == -1)
                    nasm_nonfatal("invalid effective address: two index registers");
                else if (!is_gpr)
                    nasm_nonfatal("invalid effective address: impossible register");
                else
                    nasm_nonfatal("invalid effective address: too many registers");
                return -1;
            }
        } else if (e->type == EXPR_UNKNOWN) {
            op->opflags |= OPFLAG_UNKNOWN;
        } else if (e->type == EXPR_SIMPLE) {
            o += e->value;
        } else if  (e->type == EXPR_WRT) {
            op->wrt = e->value;
        } else if (e->type >= EXPR_SEGBASE) {
            if (e->value == 1) {
                if (op->segment != NO_SEG) {
                    nasm_nonfatal("invalid effective address: multiple base segments");
                    return -1;
                }
                op->segment = e->type - EXPR_SEGBASE;
            } else if (e->value == -1 &&
                       e->type == location.segment + EXPR_SEGBASE &&
                       !(op->opflags & OPFLAG_RELATIVE)) {
                op->opflags |= OPFLAG_RELATIVE;
            } else {
                nasm_nonfatal("invalid effective address: impossible segment base multiplier");
                return -1;
            }
        } else {
            nasm_nonfatal("invalid effective address: bad subexpression type");
            return -1;
        }
    }

    op->basereg  = b;
    op->indexreg = i;
    op->scale    = s;
    op->offset   = o;
    return 0;
}

static void mref_set_optype(operand *op)
{
    int b = op->basereg;
    int i = op->indexreg;
    int s = op->scale;
    opflags_t size;

    /* It is memory, but it can match any r/m operand */
    op->type |= MEMORY_ANY;

    nasm_assert(i == -1 || s > 0);

    if (!(op->eaflags & (EAF_FS|EAF_GS)))
        op->eaflags |= EAF_NOTFSGS;

    if (b != -1) {
        opflags_t bclass = nasm_reg_flags[b];
        op->type &= bclass | ~RN_L16;
    } else if (i == -1) {
        opflags_t flag = MEM_OFFS;
        if (globl.bits == 64) {
            if (op->eaflags & EAF_ABS) {
                /* Do nothing */
            } else if (op->eaflags & EAF_REL) {
                flag = IP_REL;
            } else {
                if (globl.rel & op->eaflags)
                    flag = IP_REL;
                if (!(globl.reldef & op->eaflags)) {
                    static int64_t pass_last_seen;
                    if (pass_count() != pass_last_seen) {
                        nasm_warn(WARN_IMPLICIT_ABS_DEPRECATED,
                                  "implicit DEFAULT ABS is deprecated");
                        pass_last_seen = pass_count();
                    }
                }
            }
        }
        op->type |= flag;
    }

    if (i != -1) {
        opflags_t iclass = nasm_reg_flags[i];
        op->type &= iclass | ~RN_L16;

        if (is_class(XMMREG,iclass))
            op->type |= XMEM;
        else if (is_class(YMMREG,iclass))
            op->type |= YMEM;
        else if (is_class(ZMMREG,iclass))
            op->type |= ZMEM;
    }

    size = op->type & SIZE_MASK;
    if (!size || size == BITS16)
        op->type |= RM_SEL;
}

/*
 * Convert an expression vector returned from evaluate() into an
 * extop structure.  Return zero on success.  Note that the eop
 * already has dup and elem set, so we can't clear it here.
 */
static int value_to_extop(expr *vect, extop *eop, int32_t myseg)
{
    eop->type = EOT_DB_NUMBER;
    eop->val.num.offset = 0;
    eop->val.num.segment = eop->val.num.wrt = NO_SEG;
    eop->val.num.relative = false;

    for (; vect->type; vect++) {
        if (!vect->value)       /* zero term, safe to ignore */
            continue;

        if (vect->type <= EXPR_REG_END) /* false if a register is present */
            return -1;

        if (vect->type == EXPR_UNKNOWN) /* something we can't resolve yet */
            return 0;

        if (vect->type == EXPR_SIMPLE) {
            /* Simple number expression */
            eop->val.num.offset += vect->value;
            continue;
        }
        if (eop->val.num.wrt == NO_SEG && !eop->val.num.relative &&
            vect->type == EXPR_WRT) {
            /* WRT term */
            eop->val.num.wrt = vect->value;
            continue;
        }

        if (!eop->val.num.relative &&
            vect->type == EXPR_SEGBASE + myseg && vect->value == -1) {
            /* Expression of the form: foo - $ */
            eop->val.num.relative = true;
            continue;
        }

        if (eop->val.num.segment == NO_SEG &&
            vect->type >= EXPR_SEGBASE && vect->value == 1) {
            eop->val.num.segment = vect->type - EXPR_SEGBASE;
            continue;
        }

        /* Otherwise, badness */
        return -1;
    }

    /* We got to the end and it was all okay */
    return 0;
}

/*
 * Parse an extended expression, used by db et al. "elem" is the element
 * size; initially comes from the specific opcode (e.g. db == 1) but
 * can be overridden.
 */
static int parse_eops(extop **result, bool critical, int elem)
{
    extop *eop = NULL, *prev = NULL;
    extop **tail = result;
    int sign;
    int i = tokval.t_type;
    int oper_num = 0;
    bool do_subexpr = false;

    *tail = NULL;

    /* End of string is obvious; ) ends a sub-expression list e.g. DUP */
    for (i = tokval.t_type; i != TOKEN_EOS; i = stdscan(NULL, &tokval)) {
        bool skip;
        char endparen = ')';   /* Is a right paren the end of list? */

        if (i == ')')
            break;

        if (!eop) {
            nasm_new(eop);
            eop->dup  = 1;
            eop->elem = elem;
            do_subexpr = false;
        }
        sign = +1;

        /*
         * MASM (--masm): `dw OFFSET label' / `dd OFFSET label' -- OFFSET is a
         * no-op in a data initializer (a bare label is already its address),
         * so skip a leading OFFSET keyword and let the label parse as the
         * value.  The regular operand parser handles OFFSET separately.
         */
        if (masm_mode && i == TOKEN_ID &&
            !nasm_stricmp(tokval.t_charptr, "offset"))
            i = stdscan(NULL, &tokval);

        if (i == TOKEN_QMARK) {
            eop->type = EOT_DB_RESERVE;
            skip = true;
        } else if (do_subexpr && i == '(') {
            extop *subexpr;

            stdscan(NULL, &tokval); /* Skip paren */
            if (parse_eops(&eop->val.subexpr, critical, eop->elem) < 0)
                goto fail;

            subexpr = eop->val.subexpr;
            if (!subexpr) {
                /* Subexpression is empty */
                eop->type = EOT_NOTHING;
            } else if (!subexpr->next) {
                /*
                 * Subexpression is a single element, flatten.
                 * Note that if subexpr has an allocated buffer associated
                 * with it, freeing it would free the buffer, too, so
                 * we need to move subexpr up, not eop down.
                 */
                if (!subexpr->elem)
                    subexpr->elem = eop->elem;
                subexpr->dup *= eop->dup;
                nasm_free(eop);
                eop = subexpr;
            } else {
                eop->type = EOT_EXTOP;
            }

            /* We should have ended on a closing paren */
            if (tokval.t_type != ')') {
                char *tp = tokstr(&tokval);
                nasm_nonfatal("expected `)' after subexpression, got %s", tp);
                nasm_free(tp);
                goto fail;
            }
            endparen = 0;       /* This time the paren is not the end */
            skip = true;
        } else if (i == '%') {
            /* %(expression_list) */
            do_subexpr = true;
            continue;
        } else if (i == TOKEN_SIZE) {
            /* Element size override */
            eop->elem = tokval.t_inttwo;
            do_subexpr = true;
            continue;
        } else if (i == TOKEN_STR && end_expression_next()) {
            /*
             * end_expression_next() is to distinguish this from
             * a string used as part of an expression...
             */
            eop->type            = EOT_DB_STRING;
            eop->val.string.data = tokval.t_charptr;
            eop->val.string.len  = tokval.t_inttwo;
            skip = true;
        } else if (i == TOKEN_STRFUNC) {
            bool parens = false;
            const char *funcname = tokval.t_charptr;
            enum strfunc func = tokval.t_integer;

            i = stdscan(NULL, &tokval);
            if (i == '(') {
                parens = true;
                endparen = 0;
                i = stdscan(NULL, &tokval);
            }
            if (i != TOKEN_STR) {
                char *tp = tokstr(&tokval);
                nasm_nonfatal("%s must be followed by a string constant, got %s",
                              funcname, tp);
                nasm_free(tp);
                eop->type = EOT_NOTHING;
            } else {
                eop->type = EOT_DB_STRING_FREE;
                eop->val.string.len =
                    string_transform(tokval.t_charptr, tokval.t_inttwo,
                                     &eop->val.string.data, func);
                if (eop->val.string.len == (size_t)-1) {
                    nasm_nonfatal("invalid input string to %s", funcname);
                    eop->type = EOT_NOTHING;
                }
            }
            if (parens && i && i != ')') {
                i = stdscan(NULL, &tokval);
                if (i != ')')
                    nasm_nonfatal("unterminated %s function", funcname);
            }
            skip = i != ',';
        } else if (i == '-' || i == '+') {
            const struct stdscan_state *save = stdscan_get();
            struct tokenval tmptok;

            sign = (i == '-') ? -1 : 1;
            if (stdscan(NULL, &tmptok) != TOKEN_FLOAT) {
                stdscan_set(save);
                goto is_expression;
            } else {
                tokval = tmptok;
                goto is_float;
            }
        } else if (i == TOKEN_FLOAT) {
            enum floatize fmt;
        is_float:
            eop->type = EOT_DB_FLOAT;

            fmt = float_deffmt(eop->elem);
            if (fmt == FLOAT_ERR) {
                nasm_nonfatal("no %d-bit floating-point format supported",
                              eop->elem << 3);
                eop->val.string.len = 0;
            } else if (eop->elem < 1) {
                nasm_nonfatal("floating-point constant"
                              " encountered in unknown instruction");
                /*
                 * fix suggested by Pedro Gimeno... original line was:
                 * eop->type = EOT_NOTHING;
                 */
                eop->val.string.len = 0;
            } else {
                eop->val.string.len = eop->elem;

                eop = nasm_realloc(eop, sizeof(extop) + eop->val.string.len);
                eop->val.string.data = (char *)eop + sizeof(extop);
                if (!float_const(tokval.t_charptr, sign,
                                 (uint8_t *)eop->val.string.data, fmt))
                    eop->val.string.len = 0;
            }
            if (!eop->val.string.len)
                eop->type = EOT_NOTHING;
            skip = true;
        } else {
            /* anything else, assume it is an expression */
            expr *value;

        is_expression:
            value = evaluate(stdscan, NULL, &tokval, NULL,
                             critical, NULL);
            i = tokval.t_type;
            if (!value)                  /* Error in evaluator */
                goto fail;
            if (tokval.t_flag & TFLAG_DUP) {
                /* Expression followed by DUP */
                if (!is_simple(value)) {
                    nasm_nonfatal("non-constant argument supplied to DUP");
                    goto fail;
                } else if (value->value < 0) {
                    nasm_nonfatal("negative argument supplied to DUP");
                    goto fail;
                }
                eop->dup *= (size_t)value->value;
                do_subexpr = true;
                continue;
            }
            if (value_to_extop(value, eop, location.segment)) {
                nasm_nonfatal("expression is not simple or relocatable");
            }
            skip = false;
        }

        if (eop->dup == 0 || eop->type == EOT_NOTHING) {
            nasm_free(eop);
        } else if (eop->type == EOT_DB_RESERVE &&
                   prev && prev->type == EOT_DB_RESERVE &&
                   prev->elem == eop->elem) {
            /* Coalesce multiple EOT_DB_RESERVE */
            prev->dup += eop->dup;
            nasm_free(eop);
        } else {
            /* Add this eop to the end of the chain */
            prev = eop;
            *tail = eop;
            tail = &eop->next;
        }

        oper_num++;
        eop = NULL;             /* Done with this operand */

        if (skip) {
            /* Consume the (last) token if that didn't happen yet */
            i = stdscan(NULL, &tokval);
        }

        /*
         * We're about to call stdscan(), which will eat the
         * comma that we're currently sitting on between
         * arguments. However, we'd better check first that it
         * _is_ a comma.
         */
        if (i == TOKEN_EOS || i == endparen)	/* Already at end? */
            break;
        if (i != ',') {
            char *tp = tokstr(&tokval);
            nasm_nonfatal("comma expected after operand, got %s", tp);
            nasm_free(tp);
            goto fail;
        }
    }

    return oper_num;

fail:
    if (eop)
        nasm_free(eop);
    return -1;
}

/* Return true if not a prefix token */
static bool add_prefix(insn *result)
{
    enum prefix_pos slot;

    switch (tokval.t_type) {
    case TOKEN_SPECIAL:
        if (tokval.t_integer == S_STRICT) {
            result->opt |= OPTIM_STRICT_INSTR;
            return true;
        } else {
            return false;
        }
    case TOKEN_PREFIX:
        slot = tokval.t_inttwo;
        break;
    case TOKEN_REG:
        slot = PPS_SEG;
        if (!IS_SREG(tokval.t_integer))
            return false;
        break;
    default:
        return false;
    }

    if (result->prefixes[slot]) {
        if (result->prefixes[slot] == tokval.t_integer)
            nasm_warn(WARN_OTHER, "instruction has redundant prefixes");
        else
            nasm_nonfatal("instruction has conflicting prefixes");
    }
    result->prefixes[slot] = tokval.t_integer;

    return true;
}

/* Set value-specific immediate flags. */
static inline opflags_t set_imm_flags(struct operand *op, enum optimization opt)
{
    const bool strict = (op->type & STRICT) || (opt & OPTIM_STRICT_OPER);
    const int64_t n = op->offset;

    if (!(op->type & IMMEDIATE))
        return op->type;

    if (op->opflags & OPFLAG_UNKNOWN) {
        /* Be optimistic in pass 1 */
        if (!strict || !(op->type & SIZE_MASK))
            op->type |= UNITY|FOURBITS;
        if (!strict)
            op->type |= SBYTEDWORD|SBYTEWORD|UDWORD|SDWORD;
        op->type |= IMM_KNOWN;  /* Unknowable in pass 1 */
        return op->type;
    }

    if (!(op->opflags & OPFLAG_SIMPLE))
        return op->type;

    op->type |= IMM_KNOWN;

    if (!strict || !(op->type & SIZE_MASK)) {
        if (n == 1)
            op->type |= UNITY;

        /*
         * Allow FOURBITS matching for negative values, so things
         * like ~0 work
         */
        if (n >= -16 && n <= 15)
            op->type |= FOURBITS;
    }

    if (strict)
        return op->type;

    if ((int32_t)n == (int8_t)n)
        op->type |= SBYTEDWORD;
    if ((int16_t)n == (int8_t)n)
        op->type |= SBYTEWORD;
    if ((uint64_t)n == (uint32_t)n)
        op->type |= UDWORD;
    if ((int64_t)n == (int32_t)n)
        op->type |= SDWORD;

    return op->type;
}

/*
 * The x86 string instructions. MASM lets these be written with explicit
 * "documentation" operands (e.g. `stosd dword ptr es:[edi], eax'); NASM's
 * mnemonics take none (esi/edi/eax/dx are implicit). Used by parse_line() to
 * strip such operands under --masm.
 */
static bool masm_is_string_op(enum opcode op)
{
    switch (op) {
    case I_MOVSB: case I_MOVSW: case I_MOVSD: case I_MOVSQ:
    case I_STOSB: case I_STOSW: case I_STOSD: case I_STOSQ:
    case I_LODSB: case I_LODSW: case I_LODSD: case I_LODSQ:
    case I_SCASB: case I_SCASW: case I_SCASD: case I_SCASQ:
    case I_CMPSB: case I_CMPSW: case I_CMPSD: case I_CMPSQ:
    case I_INSB:  case I_INSW:  case I_INSD:
    case I_OUTSB: case I_OUTSW: case I_OUTSD:
        return true;
    default:
        return false;
    }
}

insn *parse_line(char *buffer, insn *result, const int bits)
{
    bool insn_is_label = false;
    struct eval_hints hints;
    int opnum;
    bool critical;
    bool first;
    bool colonless_label;
    bool recover;
    bool far_jmp_ok;
    bool have_prefixes;
    int i;

    nasm_static_assert(P_none == 0);

restart_parse:
    first               = true;
    colonless_label     = false;

    stdscan_reset(buffer);
    i = stdscan(NULL, &tokval);

    nasm_zero(*result);
    result->times       = 1;        /* No TIMES either yet */
    result->opcode      = I_none;   /* No opcode */
    result->times       = 1;        /* No TIMES either yet */
    result->loc         = location; /* Current assembly position */
    result->bits        = bits;     /* Current assembly mode */
    result->opt         = optimizing; /* Optimization flags */

    /* Ignore blank lines */
    if (i == TOKEN_EOS)
        goto fail;

    if (i == TOKEN_ID || (insn_is_label && i == TOKEN_INSN)) {
        /* there's a label here */
        struct tokenval label = tokval;
        first = false;
        result->label = tokval.t_charptr;
        i = stdscan(NULL, &tokval);
        colonless_label = i != ':';
        if (i == ':') {         /* skip over the optional colon */
            i = stdscan(NULL, &tokval);
        } else if (i == 0) {
            nasm_warn(WARN_LABEL_ORPHAN,
                      "label `%*s' alone on a line without a colon might be in error",
                      (int)label.t_len, label.t_start);
        }
        if (i != TOKEN_INSN || tokval.t_integer != I_EQU) {
            /*
             * FIXME: location.segment could be NO_SEG, in which case
             * it is possible we should be passing 'absolute.segment'. Look into this.
             * Work out whether that is *really* what we should be doing.
             * Generally fix things. I think this is right as it is, but
             * am still not certain.
             */
            define_label(result->label,
                         in_absolute ? absolute.segment : location.segment,
                         location.offset, true);
        }
    }

    have_prefixes = false;

    /* Process things that go before the opcode */
    while (i) {
        if (i == TOKEN_TIMES) {
            /* TIMES is a very special prefix */
            expr *value;

            i = stdscan(NULL, &tokval);
            value = evaluate(stdscan, NULL, &tokval, NULL,
                             pass_stable(), NULL);
            i = tokval.t_type;
            if (!value)                  /* Error in evaluator */
                goto fail;
            if (!is_simple(value)) {
                nasm_nonfatal("non-constant argument supplied to TIMES");
                result->times = 1;
            } else {
                result->times = value->value;
                /* negative values handled in assemble.c: process_insn() */
            }
        } else {
            if (!add_prefix(result))
                break;
            have_prefixes = true;
            i = stdscan(NULL, &tokval);
        }

        first = false;
    }

    if (i != TOKEN_INSN) {
        if (!i) {
            if (have_prefixes) {
                /*
                 * Instruction prefixes are present, but no actual
                 * instruction. This is allowed: at this point we
                 * invent a notional instruction of RESB 0.
                 *
                 * Note that this can be combined with TIMES, so do
                 * not clear *result!
                 *
                 */
                result->opcode          = I_RESB;
                result->operands        = 1;
                result->oprs[0].type    = IMM_NORMAL;
                result->oprs[0].opflags = OPFLAG_SIMPLE;
                result->oprs[0].offset  = 0;
                result->oprs[0].segment = result->oprs[0].wrt = NO_SEG;
                set_imm_flags(&result->oprs[0], result->opt);
            }
        } else if (!first) {
            /*
             * What was meant to be an instruction may very well have
             * been mistaken for a label here, so print out both, unless
             * it is unambiguous.
             */
            nasm_nonfatal("instruction expected, found `%s%s%.*s'",
                          colonless_label ? result->label : "",
                          colonless_label ? " " : "",
                          tokval.t_len, tokval.t_start);
        } else if (!result->label) {
            nasm_nonfatal("label, instruction or prefix expected at start of line, found `%.*s'",
                          tokval.t_len, tokval.t_start);
        }
        return result;
    }

    result->opcode = tokval.t_integer;

    /*
     * INCBIN cannot be satisfied with incorrectly
     * evaluated operands, since the correct values _must_ be known
     * on the first pass. Hence, even in pass one, we set the
     * `critical' flag on calling evaluate(), so that it will bomb
     * out on undefined symbols.
     */
    critical = pass_final() || (result->opcode == I_INCBIN);

    if (opcode_is_db(result->opcode) || result->opcode == I_INCBIN) {
        int oper_num;

        i = stdscan(NULL, &tokval);

        if (first && i == ':') {
            /* Really a label */
            insn_is_label = true;
            goto restart_parse;
        }
        first = false;
        oper_num = parse_eops(&result->eops, critical, db_bytes(result->opcode));
        if (oper_num < 0)
            goto fail;

        if (result->opcode == I_INCBIN) {
            /*
             * Correct syntax for INCBIN is that there should be
             * one string operand, followed by one or two numeric
             * operands.
             */
            if (!result->eops || result->eops->type != EOT_DB_STRING)
                nasm_nonfatal("`incbin' expects a file name");
            else if (result->eops->next &&
                     result->eops->next->type != EOT_DB_NUMBER)
                nasm_nonfatal("`incbin': second parameter is"
                              " non-numeric");
            else if (result->eops->next && result->eops->next->next &&
                     result->eops->next->next->type != EOT_DB_NUMBER)
                nasm_nonfatal("`incbin': third parameter is"
                              " non-numeric");
            else if (result->eops->next && result->eops->next->next &&
                     result->eops->next->next->next)
                nasm_nonfatal("`incbin': more than three parameters");
            else
                return result;
            /*
             * If we reach here, one of the above errors happened.
             * Throw the instruction away.
             */
            goto fail;
        } else {
            /* DB et al */
            result->operands = oper_num;
            if (oper_num == 0)
                nasm_warn(WARN_DB_EMPTY, "no operand for data declaration");
            /*
             * MASM: a labelled data declaration types the label with its
             * element size, so a later bare reference to it means its contents.
             */
            if (masm_mode && result->label) {
                int dsz = masm_dataop_size(result->opcode);
                if (dsz)
                    masm_type_set(result->label, dsz);
            }
        }
        return result;
    }

    /*
     * Now we begin to parse the operands. There may be up to MAX_OPERANDS
     * of these, separated by commas, and terminated by a zero token.
     */
    far_jmp_ok = result->opcode == I_JMP || result->opcode == I_CALL;

    /* Initialize operand structures */
    for (opnum = 0; opnum < MAX_OPERANDS; opnum++)
        init_operand(&result->oprs[opnum], opnum);

    for (opnum = 0; opnum < MAX_OPERANDS; opnum++) {
        operand *op = &result->oprs[opnum];
        expr *value;            /* used most of the time */
        bool mref = false;      /* is this going to be a memory ref? */
        int bracket = 0;        /* is it a [] mref, or a "naked" mref? */
        bool mib;               /* compound (mib) mref? */
        int setsize = 0;
        decoflags_t brace_flags = 0;    /* flags for decorators in braces */
        /*
         * A far-pointer load (LDS/LES/LFS/LGS/LSS) sizes its memory operand by
         * the destination register, not by an explicit `dword ptr'/`fword ptr'
         * cast: MASM writes `les bx, dword ptr [m]', which NASM rejects as an
         * operand-size mismatch.  Under --masm, swallow such a size on these
         * opcodes so the load is left unsized (see also the bare-label case).
         */
        bool masm_farload = masm_mode &&
            (result->opcode == I_LDS || result->opcode == I_LES ||
             result->opcode == I_LFS || result->opcode == I_LGS ||
             result->opcode == I_LSS);

        i = stdscan(NULL, &tokval);
        if (first && i == ':') {
            insn_is_label = true;
            goto restart_parse;
        }

        first = false;
        if (opnum == 0) {
            /*
             * Allow braced prefix tokens like {evex} after the opcode
             * mnemonic proper, but before the first operand. This is
             * currently not allowed for non-braced prefix tokens.
             */
            while ((tokval.t_flag & TFLAG_BRC) && add_prefix(result))
                i = stdscan(NULL, &tokval);
        }

        if (i == TOKEN_EOS)
            break;

        op->type = 0; /* so far, no override */

        /*
         * Naked special immediate token. Terminates the expression
         * without requiring a post-comma.
         */
        if (i == TOKEN_BRCCONST) {
            op->type    = IMMEDIATE; /* But not IMM_NORMAL! */
            op->opflags = OPFLAG_SIMPLE;
            op->offset  = tokval.t_integer;
            op->segment = NO_SEG;
            op->wrt     = NO_SEG;
            op->iflag   = tokval.t_inttwo;
            set_imm_flags(op, result->opt);
            i = stdscan(NULL, &tokval);
            if (i != ',')
                stdscan_pushback(&tokval);
            continue;           /* Next operand */
        }

        /* size specifiers */
        while (i == TOKEN_SPECIAL || i == TOKEN_SIZE) {
            switch (tokval.t_integer) {
            case S_BYTE:
                if (!setsize) {   /* we want to use only the first */
                    result->opt |= OPTIM_NO_Jcc_RELAX | OPTIM_NO_JMP_RELAX;
                    op->type |= BITS8;
                }
                setsize = 1;
                break;
            case S_WORD:
                if (!setsize)
                    op->type |= BITS16;
                setsize = 1;
                break;
            case S_DWORD:
            case S_LONG:
                if (!setsize && !masm_farload)
                    op->type |= BITS32;
                setsize = 1;
                break;
            case S_QWORD:
                if (!setsize)
                    op->type |= BITS64;
                setsize = 1;
                break;
            case S_TWORD:
                if (!setsize)
                    op->type |= BITS80;
                setsize = 1;
                break;
            case S_OWORD:
                if (!setsize)
                    op->type |= BITS128;
                setsize = 1;
                break;
            case S_YWORD:
                if (!setsize)
                    op->type |= BITS256;
                setsize = 1;
                break;
            case S_ZWORD:
                if (!setsize)
                    op->type |= BITS512;
                setsize = 1;
                break;
            case S_TO:
                op->type |= TO;
                break;
            case S_STRICT:
                op->type |= STRICT;
                break;
            case S_FAR:
                op->type |= FAR;
                break;
            case S_NEAR:
                /* This is not legacy behavior, even if it perhaps should be */
                result->opt |= OPTIM_NO_Jcc_RELAX | OPTIM_NO_JMP_RELAX;
                op->type |= NEAR;
                break;
            case S_SHORT:
                result->opt |= OPTIM_NO_Jcc_RELAX | OPTIM_NO_JMP_RELAX;
                op->type |= SHORT;
                break;
            case S_ABS:
                op->type |= ABS;
                break;
            default:
                nasm_nonfatal("invalid operand size specification");
            }
            i = stdscan(NULL, &tokval);
        }

        /*
         * MASM data-label semantics (--masm): a bare data label used as an
         * operand means its CONTENTS -- a memory reference [label] sized by the
         * label's declared type.  `OFFSET label' keeps NASM's bare-label
         * (address) semantics.
         */
        if (masm_mode && !mref && i == TOKEN_ID) {
            if (!nasm_stricmp(tokval.t_charptr, "offset")) {
                i = stdscan(NULL, &tokval);     /* consume OFFSET; label stays an address */
            } else {
                int msz = masm_type_get(tokval.t_charptr);
                if (msz) {
                    mref = true;                /* -> [label] */
                    /*
                     * LES/LDS/LFS/LGS/LSS load a far pointer whose operand
                     * size follows the destination register (16/32), not the
                     * pointer's byte width, so a `dword'/`fword' size from the
                     * label's type conflicts: `lds dx, myptr' would become
                     * `lds dx, dword [myptr]' = invalid operand sizes.  Leave
                     * the far-pointer load unsized and let the register decide.
                     */
                    bool farload = result->opcode == I_LDS ||
                                   result->opcode == I_LES ||
                                   result->opcode == I_LFS ||
                                   result->opcode == I_LGS ||
                                   result->opcode == I_LSS;
                    if (!setsize && !farload)
                        op->type |= masm_size_bits(msz);
                }
            }
        }

        if (i == '[' || i == '&') {
            /* memory reference */
            mref = true;
            bracket += (i == '[');
            i = stdscan(NULL, &tokval);
        } else if (i == TOKEN_MASM_PTR) {
            /*
             * `<size> PTR <operand>'.  A following `[' or a bare data label is a
             * (sized) memory reference; but MASM also writes `WORD PTR <const>'
             * for a sized IMMEDIATE (`mov [m], WORD PTR la_freefixedsize', where
             * the constant already carries the operation size).  So under --masm
             * a number or a non-data-label identifier after PTR stays an
             * immediate (mref false); everything else is memory as before.
             */
            i = stdscan(NULL, &tokval);
            if (masm_mode &&
                (i == TOKEN_NUM ||
                 (i == TOKEN_ID && masm_type_get(tokval.t_charptr) == 0))) {
                /* sized immediate: leave mref false, i holds the constant */
            } else {
                mref = true;    /* mref_more handles `[', size overrides, etc. */
            }
        }

    mref_more:
        if (mref) {
            bool done = false;
            bool nofw = false;

            while (!done) {
                switch (i) {
                case TOKEN_SPECIAL:
                case TOKEN_SIZE:
                case TOKEN_PREFIX:
                    process_size_override(result, op);
                    break;

                case '[':
                    bracket++;
                    break;

                case ',':
                    stdscan_pushback(&tokval);      /* rewind the comma */
                    tokval.t_type = TOKEN_NUM;
                    tokval.t_integer = 0;
                    done = nofw = true;
                    break;

                case TOKEN_MASM_FLAT:
                    i = stdscan(NULL, &tokval);
                    if (i != ':') {
                        nasm_nonfatal("unknown use of FLAT in MASM emulation");
                        nofw = true;
                    }
                    done = true;
                    break;

                default:
                    done = nofw = true;
                    break;
                }

                if (!nofw)
                    i = stdscan(NULL, &tokval);
            }
        }

        value = evaluate(stdscan, NULL, &tokval,
                         &op->opflags, critical, &hints);
        i = tokval.t_type;
        if (!value)                  /* Error in evaluator */
            goto fail;

        if (i == '[' && !bracket) {
            /* displacement[regs] syntax */
            mref = true;
            parse_mref(op, value); /* Process what we have so far */
            goto mref_more;
        }

        if (i == ':') {
            bool ok_reg = is_register(value->type) &&
                value->value == 1 && !value[1].type;

            if (!mref && ok_reg && !IS_SREG(value->type)) {
                /*
                 * Register pair syntax; this terminates the expression
                 * as if it had ended in a comma, but sets the COLON flag
                 * on the operand further down.
                 */
            } else if (mref || (ok_reg && IS_SREG(value->type))) {
                /* segment override? */
                mref = true;

                /*
                 * Process the segment override.
                 */
                if (!ok_reg || !IS_SREG(value->type)) {
                    nasm_nonfatal("invalid segment override");
                } else if (result->prefixes[PPS_SEG]) {
                    nasm_nonfatal("instruction has conflicting segment overrides");
                } else {
                    result->prefixes[PPS_SEG] = value->type;
                    switch (value->type) {
                    case R_FS:
                        op->eaflags |= EAF_FS;
                        break;
                    case R_GS:
                        op->eaflags |= EAF_GS;
                        break;
                    default:
                        break;
                    }
                }

                i = stdscan(NULL, &tokval); /* then skip the colon */
                goto mref_more;
            }
        }

        mib = false;
        if (mref && bracket && i == ',') {
            /* [seg:base+offset,index*scale] syntax (mib) */
            operand o2;         /* Index operand */

            if (parse_mref(op, value))
                goto fail;

            i = stdscan(NULL, &tokval); /* Eat comma */
            value = evaluate(stdscan, NULL, &tokval, &op->opflags,
                             critical, &hints);
            i = tokval.t_type;
            if (!value)
                goto fail;

            init_operand(&o2, 0);
            if (parse_mref(&o2, value))
                goto fail;
            if (o2.basereg != -1 && o2.indexreg == -1) {
                o2.indexreg = o2.basereg;
                o2.scale = 1;
                o2.basereg = -1;
            }

            if (op->indexreg != -1 || o2.basereg != -1 || o2.offset != 0 ||
                o2.segment != NO_SEG || o2.wrt != NO_SEG) {
                nasm_nonfatal("invalid mib expression");
                goto fail;
            }

            op->indexreg = o2.indexreg;
            op->scale = o2.scale;

            if (op->basereg != -1) {
                op->hintbase = op->basereg;
                op->hinttype = EAH_MAKEBASE;
            } else if (op->indexreg != -1) {
                op->hintbase = op->indexreg;
                op->hinttype = EAH_NOTBASE;
            } else {
                op->hintbase = -1;
                op->hinttype = EAH_NOHINT;
            }

            mib = true;
        }

        recover = false;
        if (mref) {
            /*
             * MASM (--masm): a param/local bound to a memory operand `[bp-o]'
             * may itself be bracketed in the source (`mov bx,[wLen]'), which
             * expands to `[[bp-o]]'.  Collapse the redundant inner bracket(s)
             * so the whole thing parses as one mref; a fold operator after an
             * inner close (`[nBytes+2]' -> `[[bp-o]+2]') re-enters the mref
             * accumulator, exactly as the trailing-`[idx]' fold does below.
             */
            if (masm_mode) {
                while (bracket > 1 && i == ']') {
                    bracket--;
                    i = stdscan(NULL, &tokval);
                    if (i == '[' || i == '+' || i == '-') {
                        if (parse_mref(op, value))
                            goto fail;
                        goto mref_more;
                    }
                }
            }
            if (bracket == 1) {
                if (i == ']') {
                    bracket--;
                    i = stdscan(NULL, &tokval);
                    /*
                     * MASM (--masm) folds a trailing `[idx]' or `+/- disp' into
                     * the SAME memory operand: a param/local expands to a bound
                     * `[bp-o]', so `Limit[2]' is `[bp-o][2]' and `Limit+2' is
                     * `[bp-o]+2', both meaning `[bp-o+2]'.  parse_mref seeds from
                     * op's current base/index/offset and accumulates, so fold the
                     * expression parsed so far and re-enter to parse the rest.
                     */
                    if (masm_mode && (i == '[' || i == '+' || i == '-')) {
                        if (parse_mref(op, value))
                            goto fail;
                        goto mref_more;
                    }
                } else {
                    nasm_nonfatal("expecting ] at end of memory operand");
                    recover = true;
                }
            } else if (bracket == 0) {
                /* Do nothing */
            } else if (bracket > 0) {
                nasm_nonfatal("excess brackets in memory operand");
                recover = true;
            } else if (bracket < 0) {
                nasm_nonfatal("unmatched ] in memory operand");
                recover = true;
            }

            if (i == TOKEN_DECORATOR || i == TOKEN_OPMASK) {
                /* parse opmask (and zeroing) after an operand */
                recover = parse_decorators(&brace_flags);
                i = tokval.t_type;
            }
            if (!recover && i != 0 && i != ',') {
                nasm_nonfatal("comma, decorator or end of line expected, got `%*s'",
                              (int)tokval.t_len, tokval.t_start);
                recover = true;
            }
        } else {                /* immediate operand */
            if (i != 0 && i != ',' && i != ':' &&
                i != TOKEN_DECORATOR && i != TOKEN_OPMASK) {
                nasm_nonfatal("comma, colon, decorator or end of "
                              "line expected after operand");
                recover = true;
            } else if (i == ':') {
                op->type |= COLON;
            } else if (i == TOKEN_DECORATOR || i == TOKEN_OPMASK) {
                /* parse opmask (and zeroing) after an operand */
                recover = parse_decorators(&brace_flags);
            }
        }
        if (recover) {
            do {                /* error recovery */
                i = stdscan(NULL, &tokval);
            } while (i != 0 && i != ',');
        }

        /*
         * now convert the exprs returned from evaluate()
         * into operand descriptions...
         */
        op->decoflags |= brace_flags;

        if (mref) {             /* it's a memory reference */
            /* A mib reference was fully parsed already */
            if (!mib) {
                if (parse_mref(op, value))
                    goto fail;
                op->hintbase = hints.base;
                op->hinttype = hints.type;
            }
            mref_set_optype(op);
            /*
             * MASM (--masm): an explicit `ds:' override on a memory operand whose
             * default segment is already DS is redundant, and MASM (like every
             * mainstream x86 assembler but NASM) emits no prefix byte for it.
             * NASM would emit a 3Eh prefix, which both diverges from MASM's bytes
             * and, accumulated over a run of such operands, pushes `short' jumps
             * out of rel8 range.  Drop it -- but only when the default segment is
             * really DS: a stack register in the addressing mode (BP/SP/EBP/ESP)
             * makes the default SS, so the `ds:' is meaningful and must stay.
             * ESP is only ever a base, but NASM may hold it as the index pending
             * the SIB base/index swap, so check both slots.
             */
            if (masm_mode && result->prefixes[PPS_SEG] == R_DS) {
                int b = op->basereg, x = op->indexreg;
                if (b != R_BP && b != R_SP && b != R_EBP && b != R_ESP &&
                    x != R_BP && x != R_SP && x != R_EBP && x != R_ESP)
                    result->prefixes[PPS_SEG] = 0;
            }
        } else if ((op->type & FAR) && !far_jmp_ok) {
                nasm_nonfatal("invalid use of FAR operand specifier");
                recover = true;
        } else {                /* it's not a memory reference */
            const enum expr_classes eclass = expr_class(value);

            if (!(eclass & ~(EC_RELOC | EC_UNKNOWN))) {
                /* It is an immediate */
		bool size_was_specified = false;
                op->offset    = reloc_value(value);
                op->segment   = reloc_seg(value);
                op->wrt       = reloc_wrt(value);
                if (eclass & EC_SELFREL)
                    op->opflags |= OPFLAG_RELATIVE;
                if (!(eclass & ~EC_SIMPLE))
                    op->opflags |= OPFLAG_SIMPLE;
                if (eclass & EC_UNKNOWN)
                    op->opflags |= OPFLAG_UNKNOWN;

                op->type |= IMM_NORMAL;
                set_imm_flags(op, result->opt);

		/*
		 * Catch a missing size specifier when dealing with a label -
		 * which might result in non-intentional handling of only part
		 * of the label's address instead of the whole value.
		 */
		for(int j = 0 ; j <= opnum ; j++)
		    size_was_specified |= !!(result->oprs[j].xsize & SIZE_MASK);

		/*
		 * Raise a 'label operation missing size specifier' error when:
		 * 1. No size specifier was mentioned up to this point.
		 * 2. One of the operands is a memory reference.
		 * 3. The current operand is a label and not a simple immediate.
		 */
		if (!size_was_specified && (op->opflags & OPFLAG_FORWARD)) {
		    for(int j = 0 ; j <= opnum ; j++) {
			if ((result->oprs[j].type & OPTYPE_MASK) == MEMORY)
			    nasm_fatal("size wasn't specified, some data may be omitted");
		    }
		}

                /*
                 * Special hack: if the previous operand was a colon
                 * immediate operand with an explicit size, and this
                 * one does not have an explicit size, move the size
                 * specifier to this operand. This handles the case:
                 * "jmp dword foo:bar" (really being "jmp foo:dword bar".)
                 */
                if (opnum > 0 &&
                    unlikely(is_class(op[-1].type, IMM_NORMAL|COLON))) {
                    opflags_t nsize = op->type    & SIZE_MASK;
                    opflags_t osize = op[-1].type & SIZE_MASK;
                    if (osize && !nsize) {
                        op->type    ^= osize;
                        op[-1].type ^= osize;
                    }
                }
            } else if (value->type == EXPR_RDSAE) {
                /*
                 * it's not an operand but a rounding or SAE decorator.
                 * put the decorator information in the (opflag_t) type field
                 * of previous operand.
                 */
                opnum--; op--;
                switch (value->value) {
                case BRC_RN:
                case BRC_RU:
                case BRC_RD:
                case BRC_RZ:
                case BRC_SAE:
                    op->decoflags |= (value->value == BRC_SAE ? SAE : ER);
                    result->evex_rm = value->value;
                    break;
                default:
                    nasm_nonfatal("invalid decorator");
                    break;
                }
            } else {            /* it's a register */
                opflags_t rs;
                uint64_t regset_size = 0;

                if (value->type >= EXPR_SIMPLE || value->value != 1) {
                    nasm_nonfatal("invalid operand type");
                    goto fail;
                }

                /*
                 * We do not allow any kind of expression, except for
                 * reg+value in which case it is a register set.
                 */
                for (i = 1; value[i].type; i++) {
                    if (!value[i].value)
                        continue;

                    switch (value[i].type) {
                    case EXPR_SIMPLE:
                        if (!regset_size) {
                            regset_size = value[i].value + 1;
                            break;
                        }
                        /* fallthrough */
                    default:
                        nasm_nonfatal("invalid operand type");
                        goto fail;
                    }
                }

                if ((regset_size & (regset_size - 1)) ||
                    regset_size >= (UINT64_C(1) << REGSET_BITS)) {
                    nasm_nonfatalf(ERR_PASS2, "invalid register set size");
                    regset_size = 0;
                }

                /*
                 * Clear overrides, except TO which applies to FPU regs
                 * and colon which is used in register pair syntax
                 */
                if (op->type & ~(TO | COLON)) {
                    /*
                     * we want to produce a warning iff the specified size
                     * is different from the register size
                     */
                    rs = op->type & (SIZE_MASK & ~NEAR);
                } else {
                    rs = 0;
                }

                /*
                 * Make sure we're not out of nasm_reg_flags, still
                 * probably this should be fixed when we're defining
                 * the label.
                 *
                 * An easy trigger is
                 *
                 *      e equ 0x80000000:0
                 *      pshufw word e-0
                 *
                 */
                if (value->type < EXPR_REG_START ||
                    value->type > EXPR_REG_END) {
                        nasm_nonfatal("invalid operand type");
                        goto fail;
                }

                op->type      &= TO | COLON;
                op->type      |= REGISTER;
                op->type      |= nasm_reg_flags[value->type];
                op->type      |= (regset_size >> 1) << REGSET_SHIFT;
                op->decoflags |= brace_flags;
                op->basereg   = value->type;

                if (rs) {
                    opflags_t opsize = nasm_reg_flags[value->type] & (SIZE_MASK & ~NEAR);
                    if (!opsize) {
                        op->type |= rs; /* For non-size-specific registers, permit size override */
                    } else if (opsize != rs) {
                        nasm_warn(WARN_REGSIZE, "invalid register size specification ignored");
                    }
                }
            }
        }

        /* remember the position of operand having broadcasting/ER mode */
        if (op->decoflags & (BRDCAST_MASK | ER | SAE)) {
            result->evex_brerop = op;
            op->bcast = true;
            op->xsize = op->decoflags & BRSIZE_MASK;
        } else {
            op->bcast = false;
            op->xsize = op->type & SIZE_MASK;
        }
    }

    result->operands = opnum; /* set operand count */

    /*
     * MASM lets the string instructions carry documentation operands, e.g.
     * `stosd dword ptr es:[edi], eax' or `movsd es:[edi], [esi]'. NASM's string
     * mnemonics take none, so under --masm accept and drop them and encode the
     * implicit form. MOVSD/CMPSD are also SSE mnemonics, so only strip when no
     * operand is an XMM register. (A non-default *source* segment override would
     * need a prefix, but the disassembly corpus emits those as `db' -- every
     * mnemonic-form string op there uses the default segment.)
     */
    if (masm_mode && opnum > 0 && masm_is_string_op(result->opcode)) {
        bool sse = false;
        int k;
        for (k = 0; k < opnum; k++)
            sse |= is_class(XMMREG, result->oprs[k].type);
        if (!sse) {
            result->operands = 0;
            /*
             * The operands documented the implied segments (ES for the
             * destination, DS for the source), which emit no prefix. Drop the
             * segment override captured from e.g. `es:[edi]' so we encode the
             * bare string op. (A non-default source override is a `db' in the
             * corpus, never a mnemonic form, so this never discards a real one.)
             */
            result->prefixes[PPS_SEG] = P_none;
        }
    }

    /*
     * MASM emits a redundant operand-size prefix on a segment-register move to
     * or from memory in 32-bit code (`mov word ptr [m], cs' -> 66 8c ..): the
     * value moved is 16-bit, so ML prefixes it, whereas NASM drops the prefix
     * because 8c/8e are inherently 16-bit and treats `word ptr' as redundant.
     * Force o16 under --masm so the bytes match. (Register moves take their size
     * from the GPR and already agree, so only the memory forms need this.)
     */
    if (masm_mode && bits == 32 && result->opcode == I_MOV &&
        result->operands == 2 &&
        result->prefixes[PPS_OSIZE] == P_none &&
        ((is_class(REG_SREG, result->oprs[0].type) &&
          is_class(MEMORY,   result->oprs[1].type)) ||
         (is_class(MEMORY,   result->oprs[0].type) &&
          is_class(REG_SREG, result->oprs[1].type)))) {
        result->prefixes[PPS_OSIZE] = P_O16;
    }

    /*
     * MASM sizes a `mov [mem], OFFSET x' store of a relocatable address by the
     * segment's word size; NASM leaves the memory operand unsized, defaulting
     * the fixup to a byte relocation the obj backend rejects.  Under --masm,
     * size an unsized memory operand to word (16-bit) / dword (32-bit) when the
     * other operand is a relocatable immediate (a symbol address, segment set).
     */
    if (masm_mode && result->operands == 2) {
        operand *mem = NULL, *imm = NULL;
        int k;
        for (k = 0; k < 2; k++) {
            if (is_class(MEMORY, result->oprs[k].type))
                mem = &result->oprs[k];
            else if (is_class(IMMEDIATE, result->oprs[k].type))
                imm = &result->oprs[k];
        }
        if (mem && imm && !(mem->type & SIZE_MASK) && imm->segment != NO_SEG)
            mem->type |= (bits >= 32) ? BITS32 : BITS16;
    }

    return result;

fail:
    result->opcode = I_none;
    return result;
}

static int end_expression_next(void)
{
    struct tokenval tv;
    const struct stdscan_state *save;
    int i;

    save = stdscan_get();
    i = stdscan(NULL, &tv);
    stdscan_set(save);

    return (i == ',' || i == ';' || i == ')' || !i);
}

static void free_eops(extop *e)
{
    extop *next;

    while (e) {
        next = e->next;
        switch (e->type) {
        case EOT_EXTOP:
            free_eops(e->val.subexpr);
            break;

        case EOT_DB_STRING_FREE:
            nasm_free(e->val.string.data);
            break;

        default:
            break;
        }

        nasm_free(e);
        e = next;
    }
}

void cleanup_insn(insn * i)
{
    free_eops(i->eops);
}
