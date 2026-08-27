/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright 1996-2025 The NASM Authors - All Rights Reserved */

/*
 * preproc.c   macro preprocessor for the Netwide Assembler
 */

/* Typical flow of text through preproc
 *
 * pp_getline gets tokenized lines, either
 *
 *   from a macro expansion
 *
 * or
 *   {
 *   read_line  gets raw text from stdmacs, predef, or current input file
 *   tokenize   converts to tokens
 *   }
 *
 * expand_mmac_params is used to expand %1 etc., unless a macro is being
 * defined or a false conditional is being processed
 * (%0, %1, %+1, %-1, %%foo
 *
 * do_directive checks for directives
 *
 * expand_smacro is used to expand single line macros
 *
 * expand_mmacro is used to expand multi-line macros
 *
 * detoken is used to convert the line back to text
 */

#include "compiler.h"

#include "nctype.h"

#include "nasm.h"
#include "nasmlib.h"
#include "assemble.h"
#include "error.h"
#include "preproc.h"
#include "hashtbl.h"
#include "quote.h"
#include "stdscan.h"
#include "eval.h"
#include "tokens.h"
#include "tables.h"
#include "listing.h"
#include "dbginfo.h"

/*
 * This is a very slow option, but it can catch some
 * serious problems...
 */
#ifndef DEBUG_MMACRO_REFCOUNTS
# define DEBUG_MMACRO_REFCOUNTS 0
#endif

/*
 * Preprocessor execution options that can be controlled by %pragma or
 * other directives.  This structure is initialized to zero on each
 * pass; this *must* reflect the default initial state.
 */
static struct pp_config {
    bool noaliases;
    bool sane_empty_expansion;
} ppconf;

/*
 * Preprocessor debug-related flags
 */
static enum pp_debug_flags {
    PDBG_MMACROS      = 1,      /* Collect mmacro information */
    PDBG_SMACROS      = 2,      /* Collect smacro information */
    PDBG_LIST_SMACROS = 4,      /* Smacros to list file (list option 's') */
    PDBG_INCLUDE      = 8       /* Collect %include information */
} ppdbg;

/*
 * Preprocessor options configured on the command line
 */
static enum preproc_opt ppopt;

typedef struct SMacro SMacro;
typedef struct MMacro MMacro;
typedef struct Context Context;
typedef struct Token Token;
typedef struct Line Line;
typedef struct Include Include;
typedef struct Cond Cond;

/*
 * The current set of multi-line macros we have defined.
 */
static struct hash_table mmacros;

/*
 * The current set of single-line macros we have defined.
 */
static struct hash_table smacros;

/*
 * The multi-line macro we are currently defining, or the %rep
 * block we are currently reading, if any.
 */
static MMacro *defining;

/*
 * Map of preprocessor directives that are also preprocessor functions;
 * if they are at the beginning of a line they are a function if and
 * only if they are followed by a (
 */
static bool pp_op_may_be_function[PP_count];

/*
 * This is the internal form which we break input lines up into.
 * Typically stored in linked lists.
 *
 * Note that `type' serves a double meaning: TOKEN_SMAC_START_PARAMS is
 * not necessarily used as-is, but is also used to encode the number
 * and expansion type of substituted parameter. So in the definition
 *
 *     %define a(x,=y) ( (x) & ~(y) )
 *
 * the token representing `x' will have its type changed to
 * tok_smac_param(0) but the one representing `y' will be
 * tok_smac_param(1); see the accessor functions below.
 *
 * TOKEN_INTERNAL_STR is a string which has been unquoted, but should
 * be treated as if it was a quoted string. The code is free to change
 * one into the other at will. TOKEN_NAKED_STR is a text token which
 * should be treated as a string, but which MUST NOT be turned into a
 * quoted string. TOKEN_INTERNAL_STRs can contain any character,
 * including NUL, but TOKEN_NAKED_STR must be a valid C string.
 */

static inline enum token_type tok_smac_param(int param)
{
    return TOKEN_SMAC_START_PARAMS + param;
}
static int smac_nparam(enum token_type toktype)
{
    return toktype - TOKEN_SMAC_START_PARAMS;
}
static bool is_smac_param(enum token_type toktype)
{
    return toktype >= TOKEN_SMAC_START_PARAMS;
}

/*
 * This is tuned so struct Token should be 64 bytes on 64-bit systems
 * and 32 bytes on 32-bit systems. It enables them to be nicely cache
 * aligned, and the text to still be kept inline for nearly all
 * tokens.
 *
 * We prohibit tokens of length > MAX_TEXT even though length here is
 * an unsigned int; this avoids problems if the length is passed
 * through an interface with type "int", and is absurdly large anyway.
 *
 * Use INT_MAX >> 2 to try to at least try to avoid risking wraparound
 * even in the fairly extreme case when "int" is incorrectly used and
 * two lengths are added. That is still 512GB with a 32-bit int...
 *
 * Earlier versions of the source code incorrectly stated that
 * examining the text string alone can be unconditionally valid. This
 * is incorrect, as some token types strip parts of the string,
 * e.g. indirect tokens.
 *
 * The pointer for out of line token strings are located at the end of
 * the buffer to maximize the likelihood of incorrectly examining
 * text.a or text.p.ptr giving a null-terminated empty string or a
 * NULL pointer, respectively, rather than something more dangerous.
 * It still not something that should happen.
 */
#define INLINE_TEXT (7*sizeof(char *)-sizeof(enum token_type) \
                     -sizeof(unsigned int)-1)
#define MAX_TEXT (INT_MAX >> 2)

struct Token {
    Token *next;
    unsigned int len;
    enum token_type type;
    union {
        char a[INLINE_TEXT+1];
        struct {
            char pad[INLINE_TEXT+1 - sizeof(char *)];
            char *ptr;
        } p;
    } text;
};

/*
 * Note on the storage of both SMacro and MMacros: the hash table
 * indexes them case-insensitively, and we then have to go through a
 * linked list of potential case aliases (and, for MMacros, parameter
 * ranges); this is to preserve the matching semantics of the earlier
 * code.  If the number of case aliases for a specific macro is a
 * performance issue, you may want to reconsider your coding style.
 */

/*
 * Function call tp obtain the expansion of an smacro
 */
typedef Token *(*ExpandSMacro)(const SMacro *s, Token **params, int nparams);

/*
 * Store the definition of a single-line macro.
 *
 * Note: for user-defined macros, SPARM_VARADIC and SPARM_DEFAULT are
 * currently never set, and SPARM_OPTIONAL is set if and only
 * if SPARM_GREEDY is set.
 */
enum sparmflags {
    SPARM_PLAIN     =   0,
    SPARM_EVAL      =   1,  /* Evaluate as a numeric expression (=) */
    SPARM_STR       =   2,  /* Convert to quoted string ($) */
    SPARM_NOSTRIP   =   4,  /* Don't strip braces (!) */
    SPARM_GREEDY    =   8,  /* Greedy final parameter (+) */
    SPARM_VARADIC   =  16,  /* Any number of separate arguments */
    SPARM_OPTIONAL  =  32,  /* Optional argument */
    SPARM_CONDQUOTE =  64,  /* With SPARM_STR, don't re-quote a string */
    SPARM_UNSIGNED  = 128   /* With SPARM_EVAL, generate unsigned numbers */
};

struct smac_param {
    Token name;
    enum sparmflags flags;
    char radix;                 /* Radix type for SPARM_EVAL */
    const Token *def;           /* Default, if any */
};

struct SMacro {
    SMacro *next;               /* MUST BE FIRST - see free_smacro() */
    char *name;
    Token *expansion;
    ExpandSMacro expand;
    intorptr expandpvt;
    struct smac_param *params;
    int nparam;                 /* length of the params structure */
    int nparam_min;             /* allows < nparam arguments */
    int in_progress;
    bool recursive;
    bool varadic;               /* greedy or supports > nparam arguments */
    bool casesense;
    bool alias;                 /* This is an alias macro */
};

/*
 * "No listing" flags. Inside a loop (%rep..%endrep) we may have
 * macro listing suppressed with .nolist, but we still need to
 * update line numbers for error messages and debug information...
 * unless we are nested inside an actual .nolist macro.
 */
enum nolist_flags {
    NL_LIST   = 1,              /* Suppress list output */
    NL_LINE   = 2               /* Don't update line information */
};

/*
 * Store the definition of a multi-line macro. This is also used to
 * store the interiors of `%rep...%endrep' blocks, which are
 * effectively self-re-invoking multi-line macros which simply
 * don't have a name or bother to appear in the hash tables. %rep
 * blocks are signified by having a NULL `name' field.
 *
 * In a MMacro describing a `%rep' block, the `in_progress' field
 * isn't merely boolean, but gives the number of repeats left to
 * run.
 *
 * The `next' field is used for storing MMacros in hash tables; the
 * `next_active' field is for stacking them on istk entries.
 *
 * When a MMacro is being expanded, `params', `iline', `nparam',
 * `paramlen', `rotate' and `unique' are local to the invocation.
 */

/*
 * Expansion stack. Note that .mmac can point back to the macro itself,
 * whereas .mstk cannot.
 */
struct mstk {
    MMacro *mstk;               /* Any expansion, real macro or not */
    MMacro *mmac;               /* Highest level actual mmacro */
};

struct MMacro {
    MMacro *next;
#if 0
    MMacroInvocation *prev;     /* previous invocation */
#endif
    size_t refcnt;              /* references to this macro */
#if DEBUG_MMACRO_REFCOUNTS
    struct {
        size_t cnt;
        MMacro *next;
    } refdbg;
#endif
    char *name;
    int nparam_min, nparam_max;
    enum nolist_flags nolist;   /* is this macro listing-inhibited? */
    bool casesense;
    bool plus;                  /* is the last parameter greedy? */
    bool capture_label;         /* macro definition has %00; capture label */
    int32_t in_progress;        /* is this macro currently being expanded? */
    int32_t max_depth;          /* maximum number of recursive expansions allowed */
    Token *dlist;               /* All defaults as one list */
    Token **defaults;           /* Parameter default pointers */
    int ndefs;                  /* number of default parameters */
    Line *expansion;

    struct mstk mstk;           /* Macro expansion stack */
    struct mstk dstk;           /* Macro definitions stack */
    Token **params;             /* actual parameters */
    Token *iline;               /* invocation line */
    struct src_location where;  /* location of definition */
    unsigned int nparam, rotate;
    char *iname;                /* name invoked as */
    int *paramlen;
    uint64_t unique;
    uint64_t condcnt;           /* number of if blocks... */
    struct {                    /* Debug information */
        struct debug_macro_def *def; /* Definition */
        struct debug_macro_inv *inv; /* Current invocation (if any) */
    } dbg;
};

/* Store the definition of a multi-line macro, as defined in a
 * previous recursive macro expansion.
 */
#if 0

struct MMacroInvocation {
    MMacroInvocation *prev;     /* previous invocation */
    Token **params;             /* actual parameters */
    Token *iline;               /* invocation line */
    unsigned int nparam, rotate;
    int *paramlen;
    uint64_t unique;
    uint64_t condcnt;
};

#endif

#if DEBUG_MMACRO_REFCOUNTS
static void check_mmacro_refcounts(void);
static MMacro *refdbg_list;
#else
# define check_mmacro_refcounts() ((void)0)
#endif

/*
 * MMacros are reference counted: each of the following adds a reference:
 * - adding to a linked list (via ->next)
 * - mstk.mstk, mstk.mmac, dstk.mstk dstk.mmac
 * - defining
 * - Line::finishes
 * - src_macro
 *
 * These functions help manage the reference counts.
 */
static void free_mmacro(MMacro *m);

static MMacro *get_mmacro(MMacro *m)
{
    if (m)
        m->refcnt++;
    return m;
}

static MMacro *pop_mmacro(MMacro **mp, MMacro *next)
{
    MMacro *m = *mp;
    *mp = next;

    if (m) {
        nasm_assert(m->refcnt > 0);
        if (!--m->refcnt) {
            if (m->name) {
                nasm_debug(2, "freeing macro `%s'", m->name);
                check_mmacro_refcounts();
            }
            nasm_assert(!m->next);
            free_mmacro(m);
        }
    }
    return next;
}

static void put_mmacro(MMacro **mp)
{
    pop_mmacro(mp, NULL);
}

static void pop_mstk(struct mstk *msp, MMacro *nextp)
{
    struct mstk next = { NULL, NULL };
    if (nextp) {
        /* Read before possible freeing action */
        next = nextp->mstk;
    }

    pop_mmacro(&msp->mmac, next.mmac);
    pop_mmacro(&msp->mstk, next.mstk);
}

 /*
 * The context stack is composed of a linked list of these.
 */
struct Context {
    Context *next;
    const char *name;
    struct hash_table localmac;
    uint64_t number;
    unsigned int depth;
};


static inline const char *tok_text(const struct Token *t)
{
    return (t->len <= INLINE_TEXT) ? t->text.a : t->text.p.ptr;
}

/*
 * Returns a mutable pointer to the text buffer. The text can be changed,
 * but the length MUST NOT CHANGE, in either direction; nor is it permitted
 * to pad with null characters to create an artificially shorter string.
 */
static inline char *tok_text_buf(struct Token *t)
{
    return (t->len <= INLINE_TEXT) ? t->text.a : t->text.p.ptr;
}

static inline unsigned int tok_check_len(size_t len)
{
    if (unlikely(len > MAX_TEXT))
	nasm_fatal("impossibly large token");

    return len;
}

static inline bool tok_text_match(const struct Token *a, const struct Token *b)
{
    return a->len == b->len && !memcmp(tok_text(a), tok_text(b), a->len);
}

static inline unused_func bool
tok_match(const struct Token *a, const struct Token *b)
{
    return a->type == b->type && tok_text_match(a, b);
}

/* strlen() variant useful for set_text() and its variants */
static size_t tok_strlen(const char *str)
{
    return strnlen(str, MAX_TEXT+1);
}

/*
 * Set the text field to a copy of the given string; the length if
 * not given should be obtained with tok_strlen().
 */
static Token *set_text(struct Token *t, const char *text, size_t len)
{
    char *textp;

    if (t->len > INLINE_TEXT)
	nasm_free(t->text.p.ptr);

    nasm_zero(t->text);

    t->len = len = tok_check_len(len);
    textp = (len > INLINE_TEXT)
	? (t->text.p.ptr = nasm_malloc(len+1)) : t->text.a;
    memcpy(textp, text, len);
    textp[len] = '\0';
    return t;
}

/*
 * Set the text field to the existing pre-allocated string, either
 * taking over or freeing the allocation in the process.
 */
static Token *set_text_free(struct Token *t, char *text, unsigned int len)
{
    char *textp;

    if (t->len > INLINE_TEXT)
	nasm_free(t->text.p.ptr);

    nasm_zero(t->text);

    t->len = len = tok_check_len(len);
    if (len > INLINE_TEXT) {
	textp = t->text.p.ptr = text;
    } else {
	textp = memcpy(t->text.a, text, len);
	nasm_free(text);
    }
    textp[len] = '\0';

    return t;
}

/*
 * Allocate a new buffer containing a copy of the text field
 * of the token.
 */
static char *dup_text(const struct Token *t)
{
    size_t size = t->len + 1;
    char *p = nasm_malloc(size);

    return memcpy(p, tok_text(t), size);
}

/*
 * Multi-line macro definitions are stored as a linked list of
 * these, which is essentially a container to allow several linked
 * lists of Tokens.
 *
 * Note that in this module, linked lists are treated as stacks
 * wherever possible. For this reason, Lines are _pushed_ on to the
 * `expansion' field in MMacro structures, so that the linked list,
 * if walked, would give the macro lines in reverse order; this
 * means that we can walk the list when expanding a macro, and thus
 * push the lines on to the `expansion' field in _istk_ in reverse
 * order (so that when popped back off they are in the right
 * order). It may seem cockeyed, and it relies on my design having
 * an even number of steps in, but it works...
 *
 * Some of these structures, rather than being actual lines, are
 * markers delimiting the end of the expansion of a given macro.
 * This is for use in the cycle-tracking and %rep-handling code.
 * Such structures have `finishes' non-NULL, and `first' NULL. All
 * others have `finishes' NULL, but `first' may still be NULL if
 * the line is blank.
 *
 * The "suppressed" flag is used by %exitmacro and %exitrep as well as
 * zero-count loops; it indicates that no output should be generated
 * the output should be suppressed, but cleanups should still be
 * performed.
 */
struct Line {
    Line *next;
    MMacro *finishes;
    Token *first;
    struct src_location where;      /* Where defined */
    bool suppressed;
};

/*
 * To handle an arbitrary level of file inclusion, we maintain a
 * stack (ie linked list) of these things.
 *
 * Note: when we issue a message for a continuation line, we want to
 * issue it for the actual *start* of the continuation line. This means
 * we need to remember how many lines to skip over for the next one.
 */
struct Include {
    Include *next;
    Cond *conds;
    Line *expansion;
    FILE *fp;
    unsigned char *data;        /* Data preloaded */
    size_t datasz;              /* Total preloaded data */
    size_t datapos;             /* Index into preloaded data buffer */
    uint64_t nolist;            /* Listing inhibit counter */
    uint64_t noline;            /* Line number update inhibit counter */
    struct mstk mstk;
    struct src_location where;  /* Filename and current line number */
    int32_t lineinc;            /* Increment given by %line */
    int32_t lineskip;           /* Accounting for passed continuation lines */
};

/*
 * File real name hash, so we don't have to re-search the include
 * path for every pass (and potentially more than that if a file
 * is used more than once.)
 */
struct hash_table FileHash;

/*
 * Counters to trap on insane macro recursion or processing.
 * Note: for smacros these count *down*, for mmacros they count *up*.
 */
struct deadman {
    int64_t total;              /* Total number of macros/tokens */
    int64_t levels;             /* Descent depth across all macros */
    bool triggered;             /* Already triggered, no need for error msg */
};

static struct deadman smacro_deadman, mmacro_deadman;

/*
 * Conditional assembly: we maintain a separate stack of these for
 * each level of file inclusion. (The only reason we keep the
 * stacks separate is to ensure that a stray `%endif' in a file
 * included from within the true branch of a `%if' won't terminate
 * it and cause confusion: instead, rightly, it'll cause an error.)
 */
enum cond_state {
    /*
     * These states are for use just after %if or %elif: IF_TRUE
     * means the condition has evaluated to truth so we are
     * currently emitting, whereas IF_FALSE means we are not
     * currently emitting but will start doing so if a %else comes
     * up. In these states, all directives are admissible: %elif,
     * %else and %endif. (And of course %if.)
     */
    COND_IF_TRUE, COND_IF_FALSE,
    /*
     * These states come up after a %else: ELSE_TRUE means we're
     * emitting, and ELSE_FALSE means we're not. In ELSE_* states,
     * any %elif or %else will cause an error.
     */
    COND_ELSE_TRUE, COND_ELSE_FALSE,
    /*
     * These states mean that we're not emitting now, and also that
     * nothing until %endif will be emitted at all. COND_DONE is
     * used when we've had our moment of emission
     * and have now started seeing %elifs. COND_NEVER is used when
     * the condition construct in question is contained within a
     * non-emitting branch of a larger condition construct,
     * or if there is an error.
     */
    COND_DONE, COND_NEVER
};
struct Cond {
    Cond *next;
    enum cond_state state;
};
#define emitting(x) ( (x) == COND_IF_TRUE || (x) == COND_ELSE_TRUE )

/*
 * These defines are used as the possible return values for do_directive
 */
#define NO_DIRECTIVE_FOUND  0
#define DIRECTIVE_FOUND     1

/*
 * Condition codes. Note that we use c_ prefix not C_ because C_ is
 * used in nasm.h for the "real" condition codes. At _this_ level,
 * we treat CXZ and ECXZ as condition codes, albeit non-invertible
 * ones, so we need a different enum...
 */
static const char * const conditions[] = {
    "a", "ae", "b", "be", "c", "cxz", "e", "ecxz", "g", "ge", "l", "le",
    "na", "nae", "nb", "nbe", "nc", "ne", "ng", "nge", "nl", "nle", "no",
    "np", "ns", "nz", "o", "p", "pe", "po", "rcxz", "s", "z"
};
enum pp_conds {
    c_A, c_AE, c_B, c_BE, c_C, c_CXZ, c_E, c_ECXZ, c_G, c_GE, c_L, c_LE,
    c_NA, c_NAE, c_NB, c_NBE, c_NC, c_NE, c_NG, c_NGE, c_NL, c_NLE, c_NO,
    c_NP, c_NS, c_NZ, c_O, c_P, c_PE, c_PO, c_RCXZ, c_S, c_Z,
    c_none = -1
};
static const enum pp_conds inverse_ccs[] = {
    c_NA, c_NAE, c_NB, c_NBE, c_NC, -1, c_NE, -1, c_NG, c_NGE, c_NL, c_NLE,
    c_A, c_AE, c_B, c_BE, c_C, c_E, c_G, c_GE, c_L, c_LE, c_O, c_P, c_S,
    c_Z, c_NO, c_NP, c_PO, c_PE, -1, c_NS, c_NZ
};

/*
 * Directive names.
 */
/* If this is a an IF, ELIF, ELSE or ENDIF keyword */
static int is_condition(enum preproc_token arg)
{
    return PP_IS_COND(arg) || (arg == PP_ELSE) || (arg == PP_ENDIF);
}

static int StackSize = 4;
static const char *StackPointer = "ebp";
static int ArgOffset = 8;
static int LocalOffset = 0;

static Context *cstk;
static Include *istk;
static const struct strlist *ipath_list;

static struct strlist *deplist;

static uint64_t unique;     /* unique identifier numbers */

static Line *predef = NULL;
static bool do_predef;
static enum preproc_mode pp_mode;

static uint64_t nested_mac_count;
static uint64_t nested_rep_count;

/*
 * The number of macro parameters to allocate space for at a time.
 */
#define PARAM_DELTA 16

/*
 * The standard macro set: defined in macros.c in a set of arrays.
 * This gives our position in any macro set, while we are processing it.
 * The stdmacset is an array of such macro sets.
 */
static macros_t **stdmaclist;
static macros_t *stdmacset[8];

/*
 * Map of which %use packages have been loaded
 */
static bool *use_loaded;

/*
 * Forward declarations.
 */
static void pp_start_stdmac(void);
static void pp_add_stdmac(macros_t *macros);
static Token *expand_mmac_params(Token * tline);
static Token *expand_smacro(Token * tline);
static Token *expand_smacro_noreset(Token * tline);
static Token *expand_id(Token * tline);
static Context *get_ctx(const char *name, const char **namep);
static Token *make_tok_num(Token *next, int64_t val);
static Token *
make_tok_num_radix(Token *next, int64_t val, char radix, bool uns);
static int64_t get_tok_num(const Token *t, bool *err);
static Token *make_tok_qstr(Token *next, const char *str);
static Token *make_tok_qstr_len(Token *next, const char *str, size_t len);
static Token *make_tok_char(Token *next, char op);
static Token *new_Token(Token * next, enum token_type type,
                        const char *text, size_t txtlen);
static Token *new_Token_free(Token * next, enum token_type type,
                             char *text, size_t txtlen);
static Token *dup_Token(Token *next, const Token *src);
static Token *new_White(Token *next);
static Token *free_Token(Token *t);
static Token *do_delete_Token(Token **tp);
#define delete_Token(tp) do_delete_Token(&(tp))
static Token *steal_Token(Token *dst, Token *src);
static const struct use_package *
get_use_pkg(Token *t, const char *dname, const char **name);
static void mark_smac_params(Token *tline, const SMacro *tmpl,
                             enum token_type type);

/* Safe extraction of token type */
static inline enum token_type tok_type(const Token *x)
{
    return x ? x->type : TOKEN_EOS;
}

/* Safe test for token type, false on x == NULL */
static inline bool tok_is(const Token *x, enum token_type t)
{
    return tok_type(x) == t;
}
/* True if token is any other kind other that "c", but not NULL */
static inline bool tok_isnt(const Token *x, enum token_type t)
{
    return x && x->type != t;
}

/* Whitespace token? */
static inline bool tok_white(const Token *x)
{
    return tok_is(x, TOKEN_WHITESPACE);
}

/* A string? */
static inline bool tok_string(const Token *x)
{
    return x && (x->type == TOKEN_STR || x->type == TOKEN_INTERNAL_STR);
}

/* A macro identifier? */
static bool tok_macro_id(const Token *x)
{
    return x && (x->type == TOKEN_ID || x->type == TOKEN_LOCAL_MACRO);
}

/* A macro or preprocessor function identifier? */
static bool tok_macro_or_func_id(const Token *x)
{
    return x && (x->type == TOKEN_ID ||
                 x->type == TOKEN_PREPROC_ID ||
                 x->type == TOKEN_LOCAL_MACRO);
}

/* Skip a token, checking for NULL */
static inline Token *skip_tok(Token *x)
{
    return x ? x->next : NULL;
}

/* Skip past any whitespace */
static inline Token *skip_white(Token *x)
{
    while (tok_white(x))
        x = x->next;

    return x;
}

/* Skip past a token and any whitespace after it */
static inline Token *skip_tok_white(Token *x)
{
    return skip_white(skip_tok(x));
}

/* Delete any whitespace */
static Token *zap_white(Token *x)
{
    while (tok_white(x))
        x = free_Token(x);

    return x;
}

/*
 * Unquote a token if it is a string, and set its type to
 * TOKEN_INTERNAL_STR.
 */

/*
 * Common version for any kind of quoted string; see asm/quote.c for
 * information about the arguments.
 */
static const char *unquote_token_anystr(Token *t, uint32_t badctl, char qstart)
{
    size_t nlen, olen;
    char *p;

    if (t->type != TOKEN_STR)
	return tok_text(t);

    olen = t->len;
    p = (olen > INLINE_TEXT) ? t->text.p.ptr : t->text.a;
    t->len = nlen = nasm_unquote_anystr(p, NULL, badctl, qstart);
    t->type = TOKEN_INTERNAL_STR;

    if (olen <= INLINE_TEXT || nlen > INLINE_TEXT)
        return p;

    nasm_zero(t->text.a);
    memcpy(t->text.a, p, nlen);
    nasm_free(p);
    return t->text.a;
}

/* Unquote any string, can produce any arbitrary binary output */
static const char *unquote_token(Token *t)
{
    return unquote_token_anystr(t, 0, STR_NASM);
}

/*
 * Same as unquote_token(), but error out if the resulting string
 * contains unacceptable control characters.
 */
static const char *unquote_token_cstr(Token *t)
{
    return unquote_token_anystr(t, BADCTL, STR_NASM);
}

/*
 * Convert a TOKEN_INTERNAL_STR token to a quoted
 * TOKEN_STR tokens.
 */
static Token *quote_any_token(Token *t);
static inline unused_func
Token *quote_token(Token *t)
{
    if (likely(!tok_is(t, TOKEN_INTERNAL_STR)))
	return t;

    return quote_any_token(t);
}

/*
 * Convert *any* kind of token to a quoted
 * TOKEN_STR token.
 */
static Token *quote_any_token(Token *t)
{
    size_t len = t->len;
    char *p;

    p = nasm_quote(tok_text(t), &len);
    t->type = TOKEN_STR;
    return set_text_free(t, p, len);
}

/*
 * In-place reverse a list of tokens.
 */
static Token *reverse_tokens(Token *t)
{
    Token *prev = NULL;
    Token *next;

    while (t) {
        next = t->next;
        t->next = prev;
        prev = t;
        t = next;
    }

    return prev;
}

/*
 * getenv() variant operating on an input token
 */
static const char *pp_getenv(const Token *t, bool warn)
{
    const char *txt = tok_text(t);
    const char *v;
    char *buf = NULL;
    bool is_string = false;

    if (!t)
	return NULL;

    switch (t->type) {
    case TOKEN_ENVIRON:
	txt += 2;		/* Skip leading %! */
	is_string = nasm_isquote(*txt);
	break;

    case TOKEN_STR:
	is_string = true;
	break;

    case TOKEN_INTERNAL_STR:
    case TOKEN_NAKED_STR:
    case TOKEN_ID:
	is_string = false;
	break;

    default:
	return NULL;
    }

    if (is_string) {
	buf = nasm_strdup(txt);
	nasm_unquote_cstr(buf, NULL);
	txt = buf;
    }

    v = getenv(txt);
    if (warn && !v) {
	nasm_warn(WARN_PP_ENVIRONMENT,
                  "nonexistent environment variable `%s'", txt);
	v = "";
    }

    if (buf)
	nasm_free(buf);

    return v;
}

/*
 * Free a linked list of tokens.
 */
static void free_tlist(Token *list)
{
    while (list)
        list = free_Token(list);

}

static void do_delete_tlist(Token **listp)
{
    if (listp) {
        free_tlist(*listp);
        *listp = NULL;
    }
}

#define delete_tlist(tp) do_delete_tlist(&(tp))

/*
 * Free a line
 */
static void free_line(Line *l)
{
    put_mmacro(&l->finishes);
    free_tlist(l->first);
    nasm_free(l);
}

/*
 * Free a linked list of lines.
 */
static void free_llist(Line *list)
{
    Line *l, *tmp;
    list_for_each_safe(l, tmp, list)
        free_line(l);
}

/*
 * Free an array of linked lists of tokens
 */
static void free_tlist_array(Token **array, size_t nlists)
{
    Token **listp = array;

    if (!array)
        return;

    while (nlists--)
        free_tlist(*listp++);

    nasm_free(array);
}

static void do_delete_tlist_array(Token ***arrayp, size_t nlist)
{
    if (arrayp) {
        free_tlist_array(*arrayp, nlist);
        *arrayp = NULL;
    }
}

#define delete_tlist_array(ap,nl) do_delete_tlist_array(&(ap),nl)

/*
 * Duplicate a linked list of tokens.
 */
static Token *dup_tlist(const Token *list, Token ***tailp)
{
    Token *newlist = NULL;
    Token **tailpp = &newlist;
    const Token *t;

    list_for_each(t, list) {
        Token *nt;
        *tailpp = nt = dup_Token(NULL, t);
        tailpp = &nt->next;
    }

    if (tailp) {
        **tailp = newlist;
        *tailp = tailpp;
    }

    return newlist;
}

/*
 * Duplicate a linked list of tokens with a maximum count
 */
static Token *dup_tlistn(const Token *list, size_t cnt, Token ***tailp)
{
    Token *newlist = NULL;
    Token **tailpp = &newlist;
    const Token *t;

    list_for_each(t, list) {
        Token *nt;
        if (!cnt--)
            break;
        *tailpp = nt = dup_Token(NULL, t);
        tailpp = &nt->next;
    }

    if (tailp) {
        **tailp = newlist;
        if (newlist)
            *tailp = tailpp;
    }

    return newlist;
}

/*
 * Duplicate a linked list of tokens in reverse order
 */
static Token *dup_tlist_reverse(const Token *list, Token *tail)
{
    const Token *t;

    list_for_each(t, list)
        tail = dup_Token(tail, t);

    return tail;
}

/*
 * Append an existing tlist to a tail pointer and returns the
 * updated tail pointer.
 */
static Token **steal_tlist(Token *tlist, Token **tailp)
{
    *tailp = tlist;

    if (!tlist)
        return tailp;

    list_last(tlist, tlist);
    return &tlist->next;
}

/*
 * Split a tlist in two by setting the next pointer of one object to NULL
 * and returning the previous value of the next pointer.
 */
static Token *cut_tlist(Token *t)
{
    Token *nt = t->next;
    t->next = NULL;
    return nt;
}

/*
 * Find the pointer to the end of a tlist. If the tlist is empty,
 * return the incoming pointer.
 */
static Token **tlist_endptr(Token **tptr)
{
    Token *t;
    while ((t = *tptr))
        tptr = &t->next;

    return tptr;
}

/*
 * Allocate a new MMacro. This does not claim a refcount; the appropriate
 * get_mmacro() calls need to be added.
 */
static MMacro *new_mmacro(void)
{
    MMacro *m;

    nasm_new(m);
#if DEBUG_MMACRO_REFCOUNT
    m->refdbg.next = refdbg_list;
    refdbg_list = m;
#endif

    return m;
}

/*
 * Clear an MMacro after invocation complete
 */
static void clear_mmacro(MMacro *m)
{
    nasm_delete(m->params);
    nasm_delete(m->iname);
    nasm_delete(m->paramlen);
    delete_tlist(m->iline);
}

/*
 * Free an MMacro
 */
static void free_mmacro(MMacro *m)
{
    nasm_assert(m->refcnt == 0);

    clear_mmacro(m);
#if !DEBUG_MMACRO_REFCOUNTS
    free_tlist(m->dlist);
    /* The actual tokens in m->defaults freed by freeing m->dlist */
    nasm_delete(m->defaults);
    free_llist(m->expansion);
    m->next = NULL;
    nasm_delete(m->name);
    nasm_free(m);
#endif
}

/*
 * Unconditionally free a list of MMacros; used on final cleanup
 */
static void free_mmacro_list(MMacro **list_p)
{
    MMacro *m, *tmp;
    MMacro *list = *list_p;

    *list_p = NULL;
    if (list)
        list->refcnt--;
    list_for_each_safe(m, tmp, list) {
        if (m->next)
            m->next->refcnt--;
        if (m->refcnt)
            nasm_nonfatal("macro %s: refcnt %lld on free, should be 0\n",
                          m->name, (long long)m->refcnt);
        free_mmacro(m);
    }
}

/*
 * Clear or free an SMacro
 */
static void free_smacro_members(SMacro *s)
{
    if (s->params) {
        int i;
        for (i = 0; i < s->nparam; i++) {
	    if (s->params[i].name.len > INLINE_TEXT)
		nasm_free(s->params[i].name.text.p.ptr);
            if (s->params[i].def)
                free_tlist((Token *)s->params[i].def);
	}
        nasm_free(s->params);
    }
    nasm_free(s->name);
    delete_tlist(s->expansion);
}

static void clear_smacro(SMacro *s)
{
    free_smacro_members(s);
    /* Wipe everything except the next pointer */
    memset(&s->name, 0, sizeof(*s) - offsetof(SMacro, name));
}

/*
 * Free an SMacro
 */
static void free_smacro(SMacro *s)
{
    free_smacro_members(s);
    nasm_free(s);
}

/*
 * Free all currently defined macros, and free the hash tables if empty
 */
enum clear_what {
    CLEAR_NONE      = 0,
    CLEAR_DEFINE    = 1,         /* Clear smacros */
    CLEAR_DEFALIAS  = 2,         /* Clear smacro aliases */
    CLEAR_ALLDEFINE = CLEAR_DEFINE|CLEAR_DEFALIAS,
    CLEAR_MMACRO    = 4,
    CLEAR_ALL       = CLEAR_ALLDEFINE|CLEAR_MMACRO
};

static void clear_smacro_table(struct hash_table *smt, enum clear_what what)
{
    struct hash_iterator it;
    const struct hash_node *np;
    bool empty = true;

    /*
     * Walk the hash table and clear out anything we don't want
     */
    hash_for_each(smt, it, np) {
        SMacro *tmp;
        SMacro *s = np->data;
        SMacro **head = (SMacro **)&np->data;

        list_for_each_safe(s, tmp, s) {
            if (what & ((enum clear_what)s->alias + 1)) {
                *head = s->next;
                free_smacro(s);
            } else {
                empty = false;
            }
        }
    }

    /*
     * Free the hash table and keys if and only if it is now empty.
     * Note: we cannot free keys even for an empty list above, as that
     * mucks up the hash algorithm.
     */
    if (empty)
        hash_free_all(smt, true);
}

static void free_smacro_table(struct hash_table *smt)
{
    clear_smacro_table(smt, CLEAR_ALLDEFINE);
}

static void free_mmacro_table(struct hash_table *mmt)
{
    struct hash_iterator it;
    const struct hash_node *np;

    hash_for_each(mmt, it, np) {
        MMacro *m = np->data;
        nasm_free((void *)np->key);
        free_mmacro_list(&m);
    }
    hash_free(mmt);
}

static void free_macros(void)
{
    check_mmacro_refcounts();
    free_smacro_table(&smacros);
    free_mmacro_table(&mmacros);
}

/*
 * Pop the context stack.
 */
static void ctx_pop(void)
{
    Context *c = cstk;

    cstk = cstk->next;

    free_smacro_table(&c->localmac);
    nasm_free((char *)c->name);
    nasm_free(c);
}

/*
 * Search for a key in the hash index; adding it if necessary
 * (in which case we initialize the data pointer to NULL.)
 */
static void **
hash_findi_add(struct hash_table *hash, const char *str)
{
    struct hash_insert hi;
    void **r;
    char *strx;
    size_t l = strlen(str) + 1;

    r = hash_findib(hash, str, l, &hi);
    if (r)
        return r;

    strx = nasm_malloc(l);  /* Use a more efficient allocator here? */
    memcpy(strx, str, l);
    return hash_add(&hi, strx, NULL);
}

/*
 * Like hash_findi, but returns the data element rather than a pointer
 * to it.  Used only when not adding a new element, hence no third
 * argument.
 */
static void *
hash_findix(struct hash_table *hash, const char *str)
{
    void **p;

    p = hash_findi(hash, str, NULL);
    return p ? *p : NULL;
}

static void inject_predefs(void)
{
    Line *pd, *l;

    /*
     * Nasty hack: here we push the contents of
     * `predef' on to the top-level expansion stack,
     * since this is the most convenient way to
     * implement the pre-include and pre-define
     * features.
     */
    list_for_each(pd, predef) {
        nasm_new(l);
        l->next     = istk->expansion;
        l->first    = dup_tlist(pd->first, NULL);

        istk->expansion = l;
    }
    do_predef = false;
}

static uint64_t get_uleb128(const char **pp)
{
    const char *p = *pp;
    unsigned int shcnt = 0;
    uint8_t c;
    uint64_t v = 0;

    do {
        c = *p++;
        v += ((uint64_t)(c & 127)) << shcnt;
        shcnt += 7;
    } while (c & 128);

    *pp = p;
    return v;
}

static char *line_from_stdmac(void)
{
    static const char *stdmacpos = NULL;
    static char *stdmacbuf = NULL;
    char *line;
    size_t len = 0;

    if (!stdmacpos || !*stdmacpos) {
        macros_t *next = *stdmaclist;

        stdmacpos = NULL;
        nasm_delete(stdmacbuf);

        if (!next) {
            if (do_predef)
                inject_predefs();
            return NULL;
        }

        *stdmaclist++ = NULL;
        if (next->dsize == next->zsize)
            stdmacpos = next->zdata; /* Incompressible */
        else
            stdmacpos = stdmacbuf = uncompress_stdmac(next);
    }

    /* Length encoded using uleb128 encoding */
    len = get_uleb128(&stdmacpos);

    line = nasm_malloc(len + 1);
    memcpy(line, stdmacpos, len);
    line[len] = '\0';
    stdmacpos += len;

    return line;
}

/*
 * Read a line from the a file. Return NULL on end of file.
 */
static char *line_from_file(FILE *f)
{
    int c;
    unsigned int size, next;
    const unsigned int delta = BUFSIZ;
    const unsigned int pad = 8;
    bool cont = false;
    char *buffer, *p;

    istk->where.lineno += istk->lineskip + istk->lineinc;
    src_set_linnum(istk->where.lineno);
    istk->lineskip = 0;

    size = delta;
    p = buffer = nasm_malloc(size);

    do {
        c = fgetc(f);

        switch (c) {
        case EOF:
            if (p == buffer) {
                nasm_free(buffer);
                return NULL;
            }
            c = 0;
            break;

        case '\r':
            next = fgetc(f);
            if (next != '\n')
                ungetc(next, f);
            if (cont) {
                cont = false;
                continue;
            }
            c = 0;
            break;

        case '\n':
            if (cont) {
                cont = false;
                continue;
            }
            c = 0;
            break;

        case 032:               /* ^Z = legacy MS-DOS end of file mark */
            c = 0;
            break;

        case '\\':
            next = fgetc(f);
            ungetc(next, f);
            if (next == '\r' || next == '\n') {
                cont = true;
                istk->lineskip += istk->lineinc;
                continue;
            }
            break;
        }

        if (p >= (buffer + size - pad)) {
            buffer = nasm_realloc(buffer, size + delta);
            p = buffer + size - pad;
            size += delta;
        }

        *p++ = c;
    } while (c);

    return buffer;
}

/*
 * Common read routine regardless of source
 */
/*
 * MASM macro / repeat front-end (--masm).
 *
 * MASM's MACRO/ENDM and REPT/ENDM are structurally unlike NASM's %macro/%rep
 * and cannot be emulated in the macro package (a package macro cannot open a
 * %macro, and ENDM cannot be aliased to a closing directive).  So we translate
 * them at the raw-line level, before tokenization:
 *
 *   NAME MACRO a, b   ->  %macro NAME 2   (body top: %define a %1, %define b %2)
 *   ENDM              ->  %endmacro / %endrep  (per the innermost open block)
 *   REPT n / REPEAT n ->  %rep n
 *
 * Named parameters are bound to the positional %1.. via %define lines injected
 * at the top of the body and %undef'd at ENDM.  Extra lines produced from one
 * input line are held in a queue and returned by read_line() in order.
 */
struct masm_ppblk {
    struct masm_ppblk *next;
    bool is_macro;
    bool is_for;                /* FOR/IRP/IRPC: replay the body per list item */
    int nparam;
    char **param;
    char *forname;              /* minted temp-macro name for a FOR/IRP body */
    char **item;                /* the FOR/IRP list items (or IRPC chars) */
    int nitem;
};
static struct masm_ppblk *masm_ppstk;
static int masm_for_seq;        /* uniquifier for FOR/IRP temp-macro names */

struct masm_ppq {
    struct masm_ppq *next;
    char *text;
};
static struct masm_ppq *masm_ppq_head, *masm_ppq_tail;

static void masm_ppq_add(char *text)
{
    struct masm_ppq *q;
    nasm_new(q);
    q->text = text;
    if (masm_ppq_tail)
        masm_ppq_tail->next = q;
    else
        masm_ppq_head = q;
    masm_ppq_tail = q;
}

static char *masm_ppq_get(void)
{
    struct masm_ppq *q = masm_ppq_head;
    char *text;
    if (!q)
        return NULL;
    text = q->text;
    masm_ppq_head = q->next;
    if (!masm_ppq_head)
        masm_ppq_tail = NULL;
    nasm_free(q);
    return text;
}

/* Copy the identifier at *pp (skipping leading blanks) into buf; advance *pp. */
static size_t masm_word(const char **pp, char *buf, size_t bufsz)
{
    const char *p = *pp;
    size_t n = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    while (nasm_isidchar(*p)) {
        if (n + 1 < bufsz)
            buf[n] = *p;
        n++;
        p++;
    }
    buf[n < bufsz ? n : bufsz - 1] = '\0';
    *pp = p;
    return n;
}

static int masm_type_bytes(const char *t);   /* defined below */

/* True if s is a single identifier (letters/digits/_.?$), trailing space aside. */
static bool masm_ident_only(const char *s)
{
    const char *p = s;
    if (!nasm_isidstart(*p) && *p != '?')
        return false;
    while (nasm_isidchar(*p) || *p == '.' || *p == '?' || *p == '$')
        p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return *p == '\0';
}

/*
 * True if s is a single atomic reference -- a bare identifier, or a lone macro
 * parameter placeholder (%{N} / %N) with nothing else.  Such a right-hand side
 * of `LHS = ref' is a textual alias, safe to bind with %define (deferring the
 * reference to the use site) rather than %assign (which forces immediate,
 * possibly-forward, evaluation).
 */
static bool masm_atom_ref(const char *s)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (masm_ident_only(p))
        return true;
    if (*p == '%') {
        p++;
        if (*p == '{') {
            p++;
            while (*p && *p != '}')
                p++;
            if (*p != '}')
                return false;
            p++;
        } else {
            if (!nasm_isdigit(*p))
                return false;
            while (nasm_isdigit(*p))
                p++;
        }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        return *p == '\0';
    }
    return false;
}

/* A MASM conditional-assembly keyword (so it is never a STRUC member line). */
static bool masm_is_cond(const char *w)
{
    return !nasm_stricmp(w, "if")   || !nasm_stricmp(w, "ife")    ||
           !nasm_stricmp(w, "ifdef")|| !nasm_stricmp(w, "ifndef") ||
           !nasm_stricmp(w, "ifb")  || !nasm_stricmp(w, "ifnb")   ||
           !nasm_stricmp(w, "ifidn")|| !nasm_stricmp(w, "ifdif")  ||
           !nasm_stricmp(w, "else") || !nasm_stricmp(w, "elseif") ||
           !nasm_stricmp(w, "endif")|| !nasm_stricmp(w, "if1")    ||
           !nasm_stricmp(w, "if2");
}

/*
 * If an expression begins with a `<size> PTR' cast (BYTE/WORD/DWORD/... PTR),
 * return a pointer past it; otherwise return the input unchanged.  MASM uses
 * this in equates like `alias EQU byte ptr field' to type an offset alias.
 */
static const char *masm_skip_typeptr(const char *s)
{
    char w1[16], w2[16];
    const char *p = s, *save;
    while (*p == ' ' || *p == '\t')
        p++;
    save = p;
    masm_word(&p, w1, sizeof w1);
    if (!masm_type_bytes(w1))
        return s;
    masm_word(&p, w2, sizeof w2);
    if (nasm_stricmp(w2, "ptr"))
        return s;
    while (*p == ' ' || *p == '\t')
        p++;
    (void)save;
    return p;
}

/*
 * MASM STRUCT bookkeeping.  Definitions translate to a NASM `struc' (offsets +
 * size), but static instances (`pt POINT <a,b>') need the member list to emit
 * each field's initialised data, so keep an ordered table on the side.
 */
struct masm_smember {
    char *name;
    const char *dir;            /* "db"/"dw"/"dd"/"dq"/"dt" (static string) */
    int count;                  /* array count from `N DUP(...)' */
    int shift;                  /* RECORD: bit position of the field's low bit */
    struct masm_smember *next;
};
struct masm_sdef {
    char *name;
    bool is_union;              /* UNION: all members at offset 0, size = max */
    bool is_record;             /* RECORD: bit-packed fields in one integer */
    const char *rdir;           /* RECORD: the packed integer's data directive */
    int usize;                  /* running max member size, for a union */
    struct masm_smember *head, *tail;
    struct masm_sdef *next;
};
static struct masm_sdef *masm_sdefs;      /* all completed struct definitions */
static struct masm_sdef *masm_sdef_cur;   /* the one currently being defined  */
static bool masm_in_struct;
static char masm_comment_delim;           /* inside a COMMENT block: its delimiter */

/*
 * Names declared with cmacros `localV name,size' -- a stack BUFFER local, bound
 * by the shim to a memory operand `[bp-off]' and typically a struct instance
 * (`localV DscBuf,DSC_LEN').  Unlike a scalar/far-pointer local, such a buffer
 * takes `.member' access (`DscBuf.dsc_access'), so record the names here to let
 * the var.field rewrite fire on them (a localV is not a registered DATA label,
 * so masm_type_query can't distinguish it -- and a blanket relax would clobber
 * the far-pointer `.sel'/`.off' half accessors, whose suffixes ARE struct field
 * names in some headers).
 */
static struct masm_lbuf { struct masm_lbuf *next; char *name; } *masm_lbufs;

static bool masm_lbuf_has(const char *name)
{
    struct masm_lbuf *b;
    for (b = masm_lbufs; b; b = b->next)
        if (!nasm_stricmp(b->name, name))
            return true;
    return false;
}

static void masm_lbuf_add(const char *name)
{
    struct masm_lbuf *b;
    if (!name || !*name || masm_lbuf_has(name))
        return;
    nasm_new(b);
    b->name = nasm_strdup(name);
    b->next = masm_lbufs;
    masm_lbufs = b;
}

/*
 * Scalar parameter/local sizes (cmacros parmB/W/D, localB/W/D), keyed by name.
 * These are shim-bound memory operands with no NASM-visible type, so record the
 * byte size the declaration implies so MOVZX/MOVSX can size a bare `parm'/
 * `local' source (`movzx ebx, Selector' where `Selector' is a parmW -> word).
 */
static struct masm_lsz { struct masm_lsz *next; char *name; int bytes; } *masm_lszs;

static int masm_lsz_get(const char *name)
{
    struct masm_lsz *v;
    for (v = masm_lszs; v; v = v->next)
        if (!nasm_stricmp(v->name, name))
            return v->bytes;
    return 0;
}

/* True if `w' is exactly an FPU stack register name ST0..ST7. */
static bool masm_is_streg(const char *w)
{
    return (w[0] == 's' || w[0] == 'S') && (w[1] == 't' || w[1] == 'T') &&
           w[2] >= '0' && w[2] <= '7' && w[3] == '\0';
}

/*
 * Copy `src' to `dst', prefixing a `$' before each standalone ST0..ST7 token
 * (a register name preceded and followed by a non-identifier char).  Used to
 * escape an FPU-register-named LABEL referenced by a non-FPU instruction
 * (`jnz st1' -> `jnz $st1'): the `$' forces symbol, not register, parsing.
 * Returns true if any token was escaped.
 */
static bool masm_escape_streg(char *dst, size_t dstsz, const char *src)
{
    const char *s = src;
    char *o = dst;
    char *end = dst + dstsz - 1;
    bool any = false;
    char prev = ' ';
    while (*s && o < end) {
        if ((prev == '\0' || !nasm_isidchar(prev)) && prev != '$' &&
            (s[0] == 's' || s[0] == 'S') && (s[1] == 't' || s[1] == 'T') &&
            s[2] >= '0' && s[2] <= '7' && !nasm_isidchar(s[3])) {
            if (o + 4 >= end)
                break;
            *o++ = '$';
            *o++ = s[0];
            *o++ = s[1];
            *o++ = s[2];
            prev = s[2];
            s += 3;
            any = true;
            continue;
        }
        prev = *s;
        *o++ = *s++;
    }
    *o = '\0';
    return any;
}

static void masm_lsz_add(const char *name, int bytes)
{
    struct masm_lsz *v;
    if (!name || !*name || masm_lsz_get(name))
        return;
    nasm_new(v);
    v->name = nasm_strdup(name);
    v->bytes = bytes;
    v->next = masm_lszs;
    masm_lszs = v;
}

static struct masm_sdef *masm_sdef_find(const char *name)
{
    struct masm_sdef *s;
    for (s = masm_sdefs; s; s = s->next)
        if (!nasm_stricmp(s->name, name))
            return s;
    return NULL;
}

/* True if `name' is a member field of any completed STRUCT/UNION (not RECORD:
 * record fields are bit shifts, not byte offsets, so `var.field' addressing
 * does not apply to them). */
static bool masm_is_field(const char *name)
{
    struct masm_sdef *s;
    struct masm_smember *m;
    for (s = masm_sdefs; s; s = s->next) {
        if (s->is_record)
            continue;
        for (m = s->head; m; m = m->next)
            if (m->name && !nasm_stricmp(m->name, name))
                return true;
    }
    return false;
}

/* Element size in bytes of a STRUCT/UNION field named `name' (0 if unknown). */
static int masm_field_bytes(const char *name)
{
    struct masm_sdef *s;
    struct masm_smember *m;
    for (s = masm_sdefs; s; s = s->next) {
        if (s->is_record)
            continue;
        for (m = s->head; m; m = m->next)
            if (m->name && !nasm_stricmp(m->name, name)) {
                if (!m->dir) return 0;
                if (!nasm_stricmp(m->dir, "db")) return 1;
                if (!nasm_stricmp(m->dir, "dw")) return 2;
                if (!nasm_stricmp(m->dir, "dd")) return 4;
                if (!nasm_stricmp(m->dir, "dq")) return 8;
                if (!nasm_stricmp(m->dir, "dt")) return 10;
                return 0;
            }
    }
    return 0;
}

/* MASM member type -> NASM data directive (for emitting instance data). */
static const char *masm_type_to_dd(const char *t)
{
    if (!nasm_stricmp(t, "byte")  || !nasm_stricmp(t, "db") ||
        !nasm_stricmp(t, "sbyte")) return "db";
    if (!nasm_stricmp(t, "word")  || !nasm_stricmp(t, "dw") ||
        !nasm_stricmp(t, "sword")) return "dw";
    if (!nasm_stricmp(t, "dword") || !nasm_stricmp(t, "dd") ||
        !nasm_stricmp(t, "sdword")) return "dd";
    if (!nasm_stricmp(t, "qword") || !nasm_stricmp(t, "dq")) return "dq";
    if (!nasm_stricmp(t, "tbyte") || !nasm_stricmp(t, "dt")) return "dt";
    return NULL;
}

/* MASM primitive type -> element size in bytes (0 if not a primitive). */
static int masm_type_bytes(const char *t)
{
    if (!nasm_stricmp(t, "byte")  || !nasm_stricmp(t, "db") ||
        !nasm_stricmp(t, "sbyte")) return 1;
    if (!nasm_stricmp(t, "word")  || !nasm_stricmp(t, "dw") ||
        !nasm_stricmp(t, "sword")) return 2;
    if (!nasm_stricmp(t, "dword") || !nasm_stricmp(t, "dd") ||
        !nasm_stricmp(t, "sdword")) return 4;
    if (!nasm_stricmp(t, "fword") || !nasm_stricmp(t, "df")) return 6;
    if (!nasm_stricmp(t, "qword") || !nasm_stricmp(t, "dq")) return 8;
    if (!nasm_stricmp(t, "tbyte") || !nasm_stricmp(t, "dt")) return 10;
    return 0;
}

/* MASM member type -> NASM reservation directive (for a struc field). */
static const char *masm_type_to_res(const char *t)
{
    if (!nasm_stricmp(t, "byte")  || !nasm_stricmp(t, "db") ||
        !nasm_stricmp(t, "sbyte")) return "resb";
    if (!nasm_stricmp(t, "word")  || !nasm_stricmp(t, "dw") ||
        !nasm_stricmp(t, "sword")) return "resw";
    if (!nasm_stricmp(t, "dword") || !nasm_stricmp(t, "dd") ||
        !nasm_stricmp(t, "sdword")) return "resd";
    if (!nasm_stricmp(t, "qword") || !nasm_stricmp(t, "dq")) return "resq";
    if (!nasm_stricmp(t, "tbyte") || !nasm_stricmp(t, "dt")) return "rest";
    return NULL;
}

/*
 * Rewrite MASM struct-member access and prefix expression operators in an
 * instruction line (B3 + B9):
 *   [reg].STRUCT.member  ->  [reg + STRUCT.member]     (NASM struc offset)
 *   SIZEOF name / TYPE name  ->  name_size             (structs + primitives;
 *                                                       *_size defined in the
 *                                                       package / by `struc')
 *   LOW  x   ->  ((x) & 0FFh)          HIGH x     ->  ((x >> 8) & 0FFh)
 *   LOWWORD x -> ((x) & 0FFFFh)        HIGHWORD x ->  ((x >> 16) & 0FFFFh)
 * (Radix suffixes 1010b / 17q / 0Ah are native NASM; PTR is handled in the
 * parser.  TYPE of a *variable* is deferred - it would need the var's type as
 * a preprocessor symbol.)
 * Returns line unchanged, or a freshly allocated rewritten line (line freed).
 */
/* True if `s' (length 2) is a segment register name. */
static bool masm_is_segreg2(const char *s)
{
    static const char *const sr[] = { "es","cs","ss","ds","fs","gs", NULL };
    int i;
    for (i = 0; sr[i]; i++)
        if (!nasm_strnicmp(s, sr[i], 2))
            return true;
    return false;
}

/*
 * MASM writes a segment override BEFORE the size cast (`es:byte ptr [bx]'),
 * but NASM wants the size first (`byte es:[bx]').  Reorder `<seg>:<size> ptr'
 * to `<size> ptr <seg>:' at the text level.  Returns a new string if anything
 * moved, else NULL.  Skips quoted text and comments.
 */
static char *masm_reorder_segsize(const char *line)
{
    const char *p = line;
    char *out = nasm_malloc(strlen(line) + 16), *o = out;
    bool changed = false;
    char qc = 0;
    while (*p) {
        if (qc) { if (*p == qc) qc = 0; *o++ = *p++; continue; }
        if (*p == '\'' || *p == '"') { qc = *p; *o++ = *p++; continue; }
        if (*p == ';') { strcpy(o, p); o += strlen(o); break; }
        if ((p == line || !nasm_isidchar(p[-1])) &&
            masm_is_segreg2(p) && !nasm_isidchar(p[2]) && p[2] == ':') {
            const char *q = p + 3;                  /* after `<seg>:' */
            char sz[16]; size_t sn = 0;
            const char *r;
            while (*q == ' ' || *q == '\t') q++;
            r = q;
            while (nasm_isidchar(*r) && sn + 1 < sizeof sz) sz[sn++] = *r++;
            sz[sn] = '\0';
            if (sn && masm_type_bytes(sz) > 0) {     /* a size keyword follows */
                const char *t = r;
                while (*t == ' ' || *t == '\t') t++;
                if (!nasm_strnicmp(t, "ptr", 3) && !nasm_isidchar(t[3])) {
                    /* emit `<size> ptr <seg>:' */
                    o += sprintf(o, "%s ptr %c%c:", sz, p[0], p[1]);
                    p = t + 3;
                    changed = true;
                    continue;
                }
            }
        }
        *o++ = *p++;
    }
    *o = '\0';
    if (!changed) { nasm_free(out); return NULL; }
    return out;
}

static char *masm_rewrite_line(char *line)
{
    const char *p;
    char *out, *o, *reord;
    size_t cap;
    bool changed = false;
    int depth = 0;                      /* `[' nesting, for var.field rewrites */

    if ((reord = masm_reorder_segsize(line)) != NULL) {
        nasm_free(line);
        line = reord;
    }
    p = line;

    cap = strlen(line) * 4 + 128;       /* operator wraps can expand the line */
    out = nasm_malloc(cap);
    o = out;

    while (*p) {
        /* MASM `%(expr)' immediate-evaluation operator -> just `(expr)': the
         * `%' forces compile-time evaluation, which NASM does anyway.  (`%('
         * is not valid NASM, so this is unambiguous.) */
        if (*p == '%' && p[1] == '(') {
            *o++ = '(';
            p += 2;
            changed = true;
            continue;
        }
        /* MASM adjacent-bracket addressing `[a][b]' -> `[a + b]' (the brackets
         * concatenate as addition): drop the `][' and insert ` + '. */
        if (*p == ']' && p[1] == '[') {
            o += sprintf(o, " + ");
            p += 2;
            changed = true;
            continue;
        }
        /*
         * Redundant parentheses around a lone identifier: `word ptr (mflags)'.
         * MASM uses `(...)' for grouping, but NASM rejects `(mem)' when the
         * identifier is a memory alias -- and the parens carry no meaning here.
         * Strip `(IDENT)' -> `IDENT' (only a single identifier, not an
         * expression, and not a call-like `name(...)').
         */
        if (*p == '(' && (p == line || !nasm_isidchar(p[-1])) &&
            nasm_isidstart(p[1])) {
            const char *q = p + 1;
            while (nasm_isidchar(*q))
                q++;
            if (*q == ')') {
                o += sprintf(o, "%.*s", (int)(q - (p + 1)), p + 1);
                p = q + 1;
                changed = true;
                continue;
            }
        }
        /* `].member.chain'  ->  ` + member.chain]' */
        if (*p == ']' && p[1] == '.') {
            const char *q = p + 2;
            o += sprintf(o, " + ");
            while (nasm_isidchar(*q) || *q == '.')
                *o++ = *q++;
            if (*q == '[') {
                /* `].member[idx]'  ->  ` + member + idx]': fold the index into
                 * the same bracket group rather than closing here.  Depth is
                 * unchanged -- the index's own `]' still closes the group. */
                o += sprintf(o, " + ");
                p = q + 1;
                changed = true;
                continue;
            }
            *o++ = ']';
            p = q;
            if (depth > 0)              /* this `]' closes a bracket */
                depth--;
            changed = true;
            continue;
        }
        /* An identifier at a word boundary: maybe a prefix operator. */
        if (nasm_isidstart(*p) &&
            (p == line || (!nasm_isidchar(p[-1]) && p[-1] != '.'))) {
            size_t wl = 0;
            const char *pre = NULL, *post = NULL;
            while (nasm_isidchar(p[wl]))
                wl++;

            /*
             * MASM word operators in OPERAND position -> C-style symbolic
             * (`and si, not (X)' -> `and si, ~(X)').  and/or/xor/shl/shr/not
             * are also instruction mnemonics, so only translate when the token
             * is clearly an operator, never the leading mnemonic: a binary op
             * must follow a value (identifier / number / `)' / `]'); the unary
             * `not' must follow an operator, comma or opening bracket.  A token
             * at line start or just after `label:' is the mnemonic -- left as-is.
             */
            {
                const char *sym = NULL;
                bool unary = false;
                char shift = 0;                 /* `l' (shl) or `r' (shr) */
                if      (wl == 3 && !nasm_strnicmp(p, "and", 3)) sym = "&";
                else if (wl == 2 && !nasm_strnicmp(p, "or",  2)) sym = "|";
                else if (wl == 3 && !nasm_strnicmp(p, "xor", 3)) sym = "^";
                else if (wl == 3 && !nasm_strnicmp(p, "shl", 3)) { sym=""; shift='l'; }
                else if (wl == 3 && !nasm_strnicmp(p, "shr", 3)) { sym=""; shift='r'; }
                else if (wl == 3 && !nasm_strnicmp(p, "not", 3)) {
                    sym = "~"; unary = true;
                }
                if (sym) {
                    char prevsig = 0;
                    char *b = o;
                    bool vend, isop;
                    while (b > out && (b[-1] == ' ' || b[-1] == '\t'))
                        b--;
                    if (b > out)
                        prevsig = b[-1];
                    vend = prevsig == ')' || prevsig == ']' ||
                           prevsig == '_' || prevsig == '?' || prevsig == '$' ||
                           prevsig == '\'' || prevsig == '"' ||  /* char/str literal */
                           (prevsig >= '0' && prevsig <= '9') ||
                           (prevsig >= 'A' && prevsig <= 'Z') ||
                           (prevsig >= 'a' && prevsig <= 'z');
                    if (unary)
                        isop = prevsig != 0 && prevsig != ':' && !vend;
                    else
                        isop = vend;
                    if (isop && shift) {
                        /*
                         * MASM SHL/SHR bind ABOVE `+'/`-'; NASM `<<'/`>>' bind
                         * BELOW.  Emit `X * (1 << Y)' / `X / (1 << Y)' instead
                         * (X shl Y == X*2^Y, X shr Y == X/2^Y): `*'/`/' have the
                         * MASM precedence, and the shift amount Y is captured as
                         * a single factor and parenthesised, so `a shl b + c'
                         * becomes `a*(1<<b)+c' = `(a shl b)+c', not `a<<(b+c)'.
                         */
                        const char *q = p + wl;
                        o += sprintf(o, " %c (1 << ", shift == 'l' ? '*' : '/');
                        while (*q == ' ' || *q == '\t')
                            q++;
                        while (*q == '+' || *q == '-' || *q == '~')
                            *o++ = *q++;                /* unary sign */
                        while (*q == ' ' || *q == '\t')
                            q++;
                        if (*q == '(') {                /* balanced group */
                            int d = 0;
                            do {
                                if (*q == '(') d++;
                                else if (*q == ')') d--;
                                *o++ = *q++;
                            } while (*q && d > 0);
                        } else {                        /* number / char / ident */
                            while (nasm_isidchar(*q) || *q == '.' || *q == '$')
                                *o++ = *q++;
                        }
                        *o++ = ')';
                        p = q;
                        changed = true;
                        continue;
                    }
                    if (isop) {
                        o += sprintf(o, "%s", sym);
                        p += wl;
                        changed = true;
                        continue;
                    }
                }
            }

            /* SIZEOF name / TYPE name  ->  name_size
             * MASK field -> MASK_field ; WIDTH field -> WIDTH_field (RECORD) */
            {
                const char *pre = NULL;
                if      (wl == 6 && !nasm_strnicmp(p, "sizeof", 6)) pre = "";
                else if (wl == 4 && !nasm_strnicmp(p, "size",   4)) pre = "";
                else if (wl == 4 && !nasm_strnicmp(p, "type",   4)) pre = "";
                else if (wl == 4 && !nasm_strnicmp(p, "mask",   4)) pre = "MASK_";
                else if (wl == 5 && !nasm_strnicmp(p, "width",  5)) pre = "WIDTH_";
                if (pre) {
                    const char *a = p + wl;
                    while (*a == ' ' || *a == '\t')
                        a++;
                    if (nasm_isidstart(*a)) {
                        const char *ns = a;
                        while (nasm_isidchar(*a) || *a == '.')
                            a++;
                        if (pre[0])             /* MASK_field / WIDTH_field */
                            o += sprintf(o, "%s%.*s", pre, (int)(a - ns), ns);
                        else                    /* name_size */
                            o += sprintf(o, "%.*s_size", (int)(a - ns), ns);
                        p = a;
                        changed = true;
                        continue;
                    }
                }
            }

            /* LOW/HIGH/LOWWORD/HIGHWORD expr  ->  masked expression */
            if      (wl == 7 && !nasm_strnicmp(p, "lowword", 7))
                { pre = "(("; post = ") & 0FFFFh)"; }
            else if (wl == 8 && !nasm_strnicmp(p, "highword", 8))
                { pre = "(("; post = " >> 16) & 0FFFFh)"; }
            else if (wl == 3 && !nasm_strnicmp(p, "low", 3))
                { pre = "(("; post = ") & 0FFh)"; }
            else if (wl == 4 && !nasm_strnicmp(p, "high", 4))
                { pre = "(("; post = " >> 8) & 0FFh)"; }
            if (pre) {
                const char *a = p + wl;
                const char *ts;
                while (*a == ' ' || *a == '\t')
                    a++;
                ts = a;
                if (*a == '(') {            /* balanced parenthesised term */
                    int d = 0;
                    do {
                        if (*a == '(') d++;
                        else if (*a == ')') d--;
                        a++;
                    } while (*a && d > 0);
                } else {                    /* identifier or numeric literal */
                    while (nasm_isidchar(*a) || *a == '.' || *a == '$')
                        a++;
                }
                if (a > ts) {
                    o += sprintf(o, "%s%.*s%s", pre, (int)(a - ts), ts, post);
                    p = a;
                    changed = true;
                    continue;
                }
            }

            /*
             * var.field member access: `myInt2F.sel' -> `[myInt2F + sel]'.
             * `.' is an identifier char, so the whole `base.field' is one token
             * (length wl): split at the first `.'.  Rewrite only when OUTSIDE
             * brackets, the first field component is a struct member, and the
             * base is not itself a struct TYPE (so a type-qualified offset
             * `SEGOFF.sel' is left alone) nor a local label (leading `.').  The
             * field stays bare so its own %idefine (-> STRUCT.field offset)
             * resolves it; any sub-member chain after it is carried along.
             */
            if (depth == 0 && p[0] != '.') {
                size_t dp = 0;
                while (dp < wl && p[dp] != '.')
                    dp++;
                if (dp > 0 && dp + 1 < wl && dp < 120) {
                    char base[128], f1[128];
                    const char *fp = p + dp + 1;
                    size_t fl = 0;
                    while (fp + fl < p + wl && fp[fl] != '.' && fl + 1 < sizeof f1) {
                        f1[fl] = fp[fl];
                        fl++;
                    }
                    char full[128];
                    f1[fl] = '\0';
                    memcpy(base, p, dp);
                    base[dp] = '\0';
                    /*
                     * Skip when `base.field' is itself a defined data label:
                     * that is a struct INSTANCE member (`p POINT <>' defines
                     * `p.x'), already handled -- and sized -- by the data-label
                     * rule.  Only a far-pointer REINTERPRET (`ptr.sel', where
                     * ptr.sel is not a label) needs the `[ptr + sel]' rewrite.
                     */
                    if (wl < sizeof full) {
                        memcpy(full, p, wl);
                        full[wl] = '\0';
                    } else {
                        full[0] = '\0';
                    }
                    if (masm_is_field(f1) && !masm_sdef_find(base) &&
                        (masm_type_query(base) > 0 || masm_lbuf_has(base)) &&
                        masm_type_query(full) == 0) {
                        /*
                         * base must be either a registered DATA label
                         * (extern/DB-DW-DD data) or a `localV' stack BUFFER
                         * (masm_lbufs), so `[base + field]' is a real address --
                         * for the buffer it expands to `[[bp-o] + field]', which
                         * the mref parser collapses to `[bp-o + field]'.  A far
                         * -pointer param/local is neither, so `ptr.sel' keeps
                         * using its seg_/off_/.sel half accessor (whose suffix
                         * names ARE struct fields, so they must not be rewritten
                         * here).
                         */
                        o += sprintf(o, "[%.*s + %.*s]", (int)dp, p,
                                     (int)(wl - dp - 1), p + dp + 1);
                        p += wl;
                        changed = true;
                        continue;
                    }
                }
            }

            /*
             * label[idx] on a data variable: `long_ptr[2]' -> `[long_ptr + 2]'
             * (MASM array / pointer indexing).  Only for a registered data
             * label whose name has no `.', outside brackets -- the index's own
             * `]' closes the group.
             */
            if (depth == 0 && p[wl] == '[' && wl < 120) {
                char nm[128];
                size_t k = 0;
                bool dotted = false;
                for (k = 0; k < wl; k++)
                    if (p[k] == '.') { dotted = true; break; }
                if (!dotted) {
                    memcpy(nm, p, wl);
                    nm[wl] = '\0';
                    if (masm_type_query(nm) > 0) {
                        o += sprintf(o, "[%.*s + ", (int)wl, p);
                        p += wl + 1;        /* past label and `[' */
                        depth++;            /* the index's `]' closes it */
                        changed = true;
                        continue;
                    }
                }
            }

            /* not an operator: copy the whole identifier verbatim */
            memcpy(o, p, wl);
            o += wl;
            p += wl;
            continue;
        }
        if (*p == '[') depth++;
        else if (*p == ']' && depth > 0) depth--;
        *o++ = *p++;
    }
    *o = '\0';

    if (!changed) {
        nasm_free(out);
        return line;
    }
    nasm_free(line);
    return out;
}

/*
 * Textually substitute a macro's parameter names with %1..%N throughout a body
 * line (MASM's model), so that a parameter works in every position -- including
 * as the target of `name = value' (-> `%N = value') and of `ifndef name', which
 * the `%define name %N' smacro approach cannot express.  Substitution skips the
 * insides of '...' and "..." string literals; identifiers are matched whole.
 */
static char *masm_subst_params(char *line, char **param, int nparam)
{
    const char *p = line;
    char *out, *o;
    bool changed = false;
    char q = 0;                         /* current string-quote char, or 0 */

    /* Even with no parameters, this pass still handles the `&' paste and `%sym'
     * immediate operators, which appear in parameterless macro bodies too. */
    out = nasm_malloc(strlen(line) * 4 + 64);
    o = out;

    while (*p) {
        if (q) {                        /* inside a string literal */
            if (*p == q)
                q = 0;
            *o++ = *p++;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            q = *p;
            *o++ = *p++;
            continue;
        }
        /*
         * MASM's `%' immediate operator: `%sym' mid-line means the VALUE of sym.
         * Map to NASM's `%[sym]' (forced expansion).  A line-leading `%' is a
         * NASM directive (%if/%assign/...) and is left alone, as are %1 (digit)
         * and %%/%$/%[ forms.
         */
        if (*p == '%' && (nasm_isalpha(p[1]) || p[1] == '_')) {
            bool leading = true;
            const char *b;
            for (b = line; b < p; b++)
                if (*b != ' ' && *b != '\t') { leading = false; break; }
            if (!leading) {
                const char *s = p + 1;
                while (nasm_isidchar(*s) || *s == '.')
                    s++;
                o += sprintf(o, "%%[%.*s]", (int)(s - (p + 1)), p + 1);
                p = s;
                changed = true;
                continue;
            }
        }
        /*
         * MASM's `&' substitution/paste operator (inside a macro body): drop it
         * where it joins a parameter to adjacent text, so `ln&OFFSET' becomes
         * `%{2}OFFSET' -> `CODEOFFSET' after paste.  As a macro-body operator it
         * is never bitwise-AND (that is the AND keyword), so this is unambiguous.
         */
        if (*p == '&' &&
            ((p > line && nasm_isidchar(p[-1])) || nasm_isidchar(p[1])) &&
            nasm_strnicmp(p + 1, "macro", 5) &&   /* keep &macro / &endm intact: */
            nasm_strnicmp(p + 1, "endm", 4)) {    /* they are nested-macro delims */
            p++;
            changed = true;
            continue;
        }
        if (nasm_isidstart(*p) &&
            (p == line || (!nasm_isidchar(p[-1]) && p[-1] != '%' && p[-1] != '.'))) {
            size_t wl = 0;
            int i, hit = -1;
            while (nasm_isidchar(p[wl]))
                wl++;
            for (i = 0; i < nparam; i++)
                if (strlen(param[i]) == wl && !memcmp(p, param[i], wl)) {
                    hit = i;
                    break;
                }
            if (hit >= 0) {
                o += sprintf(o, "%%{%d}", hit + 1);
                p += wl;
                changed = true;
                continue;
            }
            memcpy(o, p, wl);
            o += wl;
            p += wl;
            continue;
        }
        *o++ = *p++;
    }
    *o = '\0';
    if (!changed) {
        nasm_free(out);
        return line;
    }
    nasm_free(line);
    return out;
}

/*
 * Translate MASM's word operators to NASM's symbolic ones within an expression
 * (an IF/IFE condition or a `=' right-hand side).  Whole-word, case-insensitive,
 * skipping string literals.  EQ NE LT GT LE GE -> == != < > <= >= ; MOD SHL SHR
 * -> % << >> ; AND OR XOR NOT -> & | ^ ~ .  (These words are unambiguous in an
 * expression; as a line's leading mnemonic they are handled elsewhere.)
 */
static void masm_xlat_ops(char *dst, size_t dsz, const char *src)
{
    static const struct { const char *w; const char *op; } ops[] = {
        {"eq","=="}, {"ne","!="}, {"le","<="}, {"ge",">="}, {"lt","<"},
        {"gt",">"}, {"mod","%"}, {"shl","<<"}, {"shr",">>"}, {"and","&"},
        {"or","|"}, {"xor","^"}, {"not","~"}, {NULL,NULL}
    };
    char *o = dst;
    const char *p = src;
    char q = 0;
    while (*p && (size_t)(o - dst) + 8 < dsz) {
        if (q) { if (*p == q) q = 0; *o++ = *p++; continue; }
        if (*p == '\'' || *p == '"') { q = *p; *o++ = *p++; continue; }
        if (nasm_isidstart(*p) &&
            (p == src || (!nasm_isidchar(p[-1]) && p[-1] != '.'))) {
            size_t wl = 0; int i, hit = -1;
            while (nasm_isidchar(p[wl])) wl++;
            for (i = 0; ops[i].w; i++)
                if (strlen(ops[i].w) == wl && !nasm_strnicmp(p, ops[i].w, wl)) {
                    hit = i; break;
                }
            if (hit >= 0) { o += sprintf(o, "%s", ops[hit].op); p += wl; continue; }
            memcpy(o, p, wl); o += wl; p += wl; continue;
        }
        *o++ = *p++;
    }
    *o = '\0';
}

/*
 * MASM treats an undefined symbol in an IF/IFE expression as 0.  We replicate
 * that safely for the `?'-prefixed conditional-assembly OPTION switches
 * (?CHKSTK1, ?RIPAUX, ?DF, ...) that pervade cmacros-era code: before the
 * translated %if, emit a `%ifndef ?X / %assign ?X 0 / %endif' guard for each
 * such symbol referenced in the expression, so an option that no module set
 * defaults to 0/false rather than failing "not defined before use".  Limiting
 * this to the `?' convention avoids masking a genuine typo in an ordinary
 * symbol.  Guards are idempotent, so re-emitting on later uses is harmless.
 */
static void masm_emit_opt_guards(const char *expr)
{
    const char *p = expr;
    char seen[16][64];
    int nseen = 0, i;

    while (*p) {
        if (*p == '?' && nasm_isidstart(p[1]) &&
            (p == expr || (!nasm_isidchar(p[-1]) && p[-1] != '.'))) {
            char nm[64];
            size_t n = 0;
            nm[n++] = *p++;             /* leading '?' */
            while (n + 1 < sizeof nm && nasm_isidchar(*p))
                nm[n++] = *p++;
            nm[n] = '\0';
            for (i = 0; i < nseen; i++)
                if (!strcmp(seen[i], nm))
                    break;
            if (i == nseen && nseen < 16) {
                char g[160];
                strcpy(seen[nseen++], nm);
                snprintf(g, sizeof g, "%%ifndef %s", nm);
                masm_ppq_add(nasm_strdup(g));
                snprintf(g, sizeof g, "%%assign %s 0", nm);
                masm_ppq_add(nasm_strdup(g));
                masm_ppq_add(nasm_strdup("%endif"));
            }
        } else {
            p++;
        }
    }
}

/*
 * MASM anonymous labels: `@@:' defines one, `@F' refers to the NEXT `@@'
 * forward, `@B' to the nearest `@@' backward.  Rewrite each to a counter-
 * generated global label -- single-pass friendly because `@F' is simply the
 * label the next `@@:' will mint (the current counter) and `@B' the last one
 * minted (counter - 1).  Returns a fresh line if anything changed, else NULL.
 * String literals and trailing `;' comments are left untouched.  Regular-code
 * only: inside a macro body one label would be baked into every expansion.
 */
/* MASM data-type keyword -> element size in bytes (0 = not a data type). */
static int masm_type_from_word(const char *w, size_t n)
{
    if (n == 4 && !nasm_strnicmp(w, "byte",  4)) return 1;
    if (n == 4 && !nasm_strnicmp(w, "word",  4)) return 2;
    if (n == 5 && !nasm_strnicmp(w, "dword", 5)) return 4;
    if (n == 5 && !nasm_strnicmp(w, "fword", 5)) return 6;
    if (n == 5 && !nasm_strnicmp(w, "qword", 5)) return 8;
    if (n == 5 && !nasm_strnicmp(w, "tbyte", 5)) return 10;
    return 0;                           /* ABS / NEAR / FAR / PROC / ptr */
}

/*
 * Emit `extern NAME' for each name in a MASM external declaration list, and
 * register its MASM data type so a bare cross-module reference reads as its
 * contents (`mov ds, curTDB' -> `mov ds, [curTDB]').  `deftype' is the element
 * size implied by the directive (externW=2 / externD=4 / externB=1; 0 for
 * code/abs).  A per-name `:type' (bare `EXTRN foo:word') overrides deftype.
 * Angle brackets around a `<a,b,...>' group are stripped.
 */
static char *masm_extern_emit(const char *rest, int deftype)
{
    const char *r = rest;
    bool any = false;

    while (*r && *r != ';') {
        char nm[128];
        size_t nn = 0;
        int sz = deftype;
        while (*r == ' ' || *r == '\t' || *r == ',' || *r == '<' || *r == '>')
            r++;
        if (!*r || *r == ';')
            break;
        while (*r && *r != ',' && *r != ':' && *r != ';' &&
               *r != ' ' && *r != '\t' && *r != '<' && *r != '>') {
            if (nn + 1 < sizeof nm)
                nm[nn++] = *r;
            r++;
        }
        nm[nn] = '\0';
        while (*r == ' ' || *r == '\t')
            r++;
        if (*r == ':') {                /* optional per-name :type */
            const char *ts;
            size_t tn;
            r++;
            while (*r == ' ' || *r == '\t')
                r++;
            ts = r;
            while (*r && *r != ',' && *r != ';' &&
                   *r != ' ' && *r != '\t' && *r != '>')
                r++;
            tn = (size_t)(r - ts);
            sz = masm_type_from_word(ts, tn);
        }
        if (nn) {
            char g[160];
            masm_type_note(nm, sz);
            snprintf(g, sizeof g, "extern %s", nm);
            masm_ppq_add(nasm_strdup(g));
            any = true;
        }
    }
    if (!any)
        masm_ppq_add(nasm_strdup(""));
    return masm_ppq_get();
}

static int masm_anon_seq;

static char *masm_rewrite_anon(const char *line)
{
    const char *p = line;
    char *out, *o;
    bool changed = false;
    char qc = 0;

    out = nasm_malloc(strlen(line) * 2 + 64);
    o = out;
    while (*p) {
        if (qc) { if (*p == qc) qc = 0; *o++ = *p++; continue; }
        if (*p == '\'' || *p == '"') { qc = *p; *o++ = *p++; continue; }
        if (*p == ';') { strcpy(o, p); o += strlen(p); p += strlen(p); break; }
        if (*p == '@' && p[1] == '@' && p[2] == ':' &&
            (p == line || (!nasm_isidchar(p[-1]) && p[-1] != '@'))) {
            o += sprintf(o, "__masm_anon_%d:", masm_anon_seq);
            masm_anon_seq++;
            p += 3;
            changed = true;
            continue;
        }
        if (*p == '@' && (p[1] == 'F' || p[1] == 'f' || p[1] == 'B' ||
                          p[1] == 'b') &&
            !nasm_isidchar(p[2]) && p[2] != '@' &&
            (p == line || (!nasm_isidchar(p[-1]) && p[-1] != '@'))) {
            int n = (p[1] == 'B' || p[1] == 'b') ? masm_anon_seq - 1
                                                 : masm_anon_seq;
            o += sprintf(o, "__masm_anon_%d", n);
            p += 2;
            changed = true;
            continue;
        }
        *o++ = *p++;
    }
    *o = '\0';
    if (!changed) {
        nasm_free(out);
        return NULL;
    }
    return out;
}

static char *masm_pp_xform(char *line)
{
    const char *p = line;
    char w1[256], w2[256], tmp[512];
    size_t l1, l2;

    /*
     * A C-preprocessor-style conditional (`#ifdef WOW', `#endif') -- some MASM
     * sources (and the Win3.1 reconstruction) prefix the IF-family with `#'.
     * NASM reads a leading `#' as a line-number directive, so strip it and let
     * the bare MASM conditional handling below translate the rest.  Only the
     * IF-family is unwrapped; any other `#...' is left for NASM.
     */
    {
        const char *h = line;
        while (*h == ' ' || *h == '\t')
            h++;
        if (*h == '#' && nasm_isidstart(h[1])) {
            char cw[32];
            size_t cn = 0;
            const char *c = h + 1;
            while (nasm_isidchar(*c) && cn + 1 < sizeof cw)
                cw[cn++] = *c++;
            cw[cn] = '\0';
            if (masm_is_cond(cw)) {
                char *nl = nasm_strdup(h + 1);   /* drop the `#' */
                nasm_free(line);
                return masm_pp_xform(nl);
            }
        }
    }

    /*
     * A macro that GENERATES a macro: `NAME &macro [p,...]' opens the inner
     * macro and `&endm' closes it (MASM marks them with `&' to keep them
     * distinct from the outer's own delimiters).  Translate to %macro/%endmacro
     * emitted as body TEXT -- no block-stack push/pop, since NASM defines the
     * inner macro when the outer expands.  Done before substitution so the `&'
     * prefix is still present.
     */
    if (masm_ppstk && masm_ppstk->is_macro) {
        const char *t = line;
        const char *am;
        while (*t == ' ' || *t == '\t')
            t++;
        if (!nasm_strnicmp(t, "&endm", 5) &&
            (t[5]=='\0'||t[5]==' '||t[5]=='\t'||t[5]=='\r'||t[5]=='\n')) {
            nasm_free(line);
            return nasm_strdup("%endmacro");
        }
        am = strstr(t, "&macro");
        if (am) {
            char meat[160], *nsub;
            int nl = (int)(am - t), arity = 0;
            const char *pp = am + 6;
            while (nl > 0 && (t[nl-1]==' '||t[nl-1]=='\t'))
                nl--;
            snprintf(meat, sizeof meat, "%.*s", nl, t);
            nsub = masm_subst_params(nasm_strdup(meat),
                                     masm_ppstk->param, masm_ppstk->nparam);
            while (*pp == ' ' || *pp == '\t')
                pp++;
            if (*pp) { arity = 1; for (; *pp; pp++) if (*pp == ',') arity++; }
            if (arity)                  /* %imacro: case-insensitive, like MASM */
                snprintf(tmp, sizeof tmp, "%%imacro %s 0-%d", nsub, arity);
            else
                snprintf(tmp, sizeof tmp, "%%imacro %s 0", nsub);
            nasm_free(nsub);
            nasm_free(line);
            return nasm_strdup(tmp);
        }
    }

    /* Inside a MACRO body: substitute the parameter names with %1..%N first, so
     * every later transform sees the NASM parameter form. */
    if (masm_ppstk && masm_ppstk->is_macro)
        line = masm_subst_params(line, masm_ppstk->param, masm_ppstk->nparam);
    p = line;

    /* Inside a COMMENT block: swallow lines until the delimiter recurs. */
    if (masm_comment_delim) {
        if (strchr(line, masm_comment_delim))
            masm_comment_delim = 0;
        nasm_free(line);
        return nasm_strdup("");
    }

    /* MASM anonymous labels @@:/@F/@B -> counter-generated labels (regular
     * code only; a macro body would emit one label per expansion). */
    if (!(masm_ppstk && masm_ppstk->is_macro)) {
        char *ar = masm_rewrite_anon(line);
        if (ar) { nasm_free(line); line = ar; p = line; }
    }

    /* A `%'-line is MASM's immediate text-expansion; the only such directive we
     * need is %OUT (an assembly-time message) -- drop it.  (`%' cannot start a
     * NASM identifier, so this must be checked before the word scan.) */
    {
        const char *q = line;
        while (*q == ' ' || *q == '\t')
            q++;
        if (q[0] == '%' && !nasm_strnicmp(q + 1, "out", 3)) {
            nasm_free(line);
            return nasm_strdup("");
        }
        /*
         * A leading `% ' (percent then whitespace) is MASM's line-level
         * immediate-expansion marker (`% sBegin &type&%KRNLDS' in kernel.inc's
         * DataBegin): it forces the rest of the line's text macros to expand,
         * which NASM does anyway.  Drop the marker and process the remainder.
         * (`%if'/`%macro'/... have no space after `%'; `%[' is NASM syntax --
         * none match, so they are left alone.)
         */
        if (q[0] == '%' && (q[1] == ' ' || q[1] == '\t')) {
            char *nl;
            q++;
            while (*q == ' ' || *q == '\t')
                q++;
            nl = nasm_strdup(q);
            nasm_free(line);
            line = nl;
            p = line;
        }
    }

    /*
     * A parameter used as (or pasted into) an `=' target became `%{N}' after
     * substitution -- e.g. `%{1} = v' or `?t%{1} = v' -- which the name-first
     * `=' handler below cannot see.  Detect a top-level single `=' whose left
     * side contains `%{' and emit `%assign LHS RHS'.
     */
    if (masm_ppstk && masm_ppstk->is_macro && strstr(line, "%{")) {
        const char *s = line, *eqp = NULL;
        char qc = 0;
        for (; *s; s++) {
            if (qc) { if (*s == qc) qc = 0; continue; }
            if (*s == '\'' || *s == '"') { qc = *s; continue; }
            if (*s == '=' && s[1] != '=' &&
                (s == line || (s[-1]!='='&&s[-1]!='<'&&s[-1]!='>'&&s[-1]!='!'))) {
                eqp = s; break;
            }
        }
        if (eqp) {
            char lhs[256], ex[512], *lp = lhs;
            size_t le;
            snprintf(lhs, sizeof lhs, "%.*s", (int)(eqp - line), line);
            while (*lp == ' ' || *lp == '\t') lp++;
            le = strlen(lp);
            while (le && (lp[le-1]==' '||lp[le-1]=='\t')) lp[--le] = '\0';
            if (strstr(lp, "%{")) {
                const char *xr;
                masm_xlat_ops(ex, sizeof ex, eqp + 1);
                xr = ex;
                while (*xr == ' ' || *xr == '\t')
                    xr++;
                /*
                 * A `&'-generated symbol assigned a lone identifier
                 * (`_hft_&count = handler') is a textual alias, not a value:
                 * the RHS is often a label defined LATER (a forward handler
                 * address), so a numeric %assign would fail "not defined
                 * before use".  %define defers resolution to the use site
                 * (`dw _hft_0' -> `dw la_trap').  The LHS always carries a
                 * %{...} paste and the RHS is a bare identifier, so the two
                 * can never be equal -- no direct self-reference cycle.
                 */
                if (masm_atom_ref(xr))
                    snprintf(tmp, sizeof tmp, "%%idefine %s %s", lp, xr);
                else
                    snprintf(tmp, sizeof tmp, "%%iassign %s %s", lp, ex);
                nasm_free(line);
                masm_ppq_add(nasm_strdup(tmp));
                return masm_ppq_get();
            }
        }
    }

    l1 = masm_word(&p, w1, sizeof w1);
    if (!l1)
        return line;

    /*
     * A label prefixing a MASM string instruction (`foint: lods byte ptr
     * es:[si]'): the string-op rewrite below keys on the FIRST word, which
     * here is the label, so the `lods' would be missed.  When a `name:' label
     * is followed by a bare string op (optionally REP-prefixed), split the
     * label off, translate the remainder, and re-emit the label ahead of it.
     * (Narrow on purpose -- a plain `name: mov ..' needs no split; NASM
     * handles it directly.)
     */
    if (*p == ':' && p[1] != ':' && w1[0] != '.' && !masm_is_streg(w1)) {
        const char *rest = p + 1;
        char rw[16];
        while (*rest == ' ' || *rest == '\t')
            rest++;
        { const char *q = rest; masm_word(&q, rw, sizeof rw); }
        if (!nasm_stricmp(rw, "lods") || !nasm_stricmp(rw, "stos") ||
            !nasm_stricmp(rw, "movs") || !nasm_stricmp(rw, "scas") ||
            !nasm_stricmp(rw, "cmps") || !nasm_stricmp(rw, "ins")  ||
            !nasm_stricmp(rw, "outs") || !nasm_stricmp(rw, "rep")  ||
            !nasm_stricmp(rw, "repe") || !nasm_stricmp(rw, "repz") ||
            !nasm_stricmp(rw, "repne")|| !nasm_stricmp(rw, "repnz")) {
            char lbl[264];
            char *r, *held[8];
            int nh = 0, i;
            snprintf(lbl, sizeof lbl, "%s:", w1);
            r = masm_pp_xform(nasm_strdup(rest));
            if (r)
                held[nh++] = r;
            while (nh < 7 && (r = masm_ppq_get()) != NULL)
                held[nh++] = r;
            masm_ppq_add(nasm_strdup(lbl));
            for (i = 0; i < nh; i++)
                masm_ppq_add(held[i]);
            nasm_free(line);
            return masm_ppq_get();
        }
    }

    /*
     * `localV name, size' declares a stack BUFFER local (a struct instance).
     * Record the name so its `.member' access is rewritten (see masm_lbufs);
     * the line itself still flows on to the shim's `localV' macro unchanged.
     */
    if (!nasm_stricmp(w1, "localv")) {
        const char *q = p;
        char nm[128];
        const char *c;
        if (masm_word(&q, nm, sizeof nm))
            masm_lbuf_add(nm);
        /*
         * The size argument is often a `<...>'-wrapped expression whose space
         * would otherwise split it (`localV n, <SIZE S>' / `<SIZE S + 2>').
         * NASM separates macro args on commas, not the `<>' group, so unwrap it
         * (`localV n, SIZE S') -- the size then flows through the normal SIZE
         * operator rewrite and reaches the shim `localV' as one argument.
         */
        c = strchr(p, ',');
        if (c) {
            const char *s = c + 1;
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s == '<') {
                const char *e = strchr(s, '>');
                if (e && e > s + 1) {
                    snprintf(tmp, sizeof tmp, "%s %.*s%.*s", w1,
                             (int)(s - p), p, (int)(e - s - 1), s + 1);
                    nasm_free(line);
                    return masm_rewrite_line(nasm_strdup(tmp));
                }
            }
        }
    }

    /*
     * `cCall <far ptr NAME>, <args>' -- a cCall whose TARGET is a `<...>'-
     * wrapped far/near pointer.  The shim's cCall binds the target from its
     * first argument and does `call <target>', so the `<>' wrapper and the
     * `ptr' keyword must be removed first: rewrite to `cCall far NAME, <args>'
     * (the `far'/`near' keyword stays, so `call __cc_fn' becomes `call far
     * NAME' -- a far call to the external proc).
     */
    if (!nasm_stricmp(w1, "ccall")) {
        const char *s = p;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '<') {
            const char *e = strchr(s, '>');
            if (e && e > s + 1) {
                char tgt[256];
                size_t tn = 0;
                const char *t = s + 1;
                while (t < e && tn + 1 < sizeof tgt) {
                    if ((t == s + 1 || !nasm_isidchar(t[-1])) &&
                        !nasm_strnicmp(t, "ptr", 3) && !nasm_isidchar(t[3])) {
                        t += 3;                 /* drop a standalone `ptr' */
                        while (*t == ' ' || *t == '\t')
                            t++;
                        continue;
                    }
                    tgt[tn++] = *t++;
                }
                tgt[tn] = '\0';
                snprintf(tmp, sizeof tmp, "cCall %s%s", tgt, e + 1);
                nasm_free(line);
                return masm_rewrite_line(nasm_strdup(tmp));
            }
        } else if (nasm_isidstart(*s)) {
            /*
             * `cCall NAME <args>' -- the target and its `<...>' argument list
             * are separated by a SPACE, not a comma.  NASM splits macro args on
             * commas, so this reaches the shim as a single mangled first arg.
             * Insert the comma: `cCall NAME, <args>'.
             */
            const char *n = s;
            while (nasm_isidchar(*n))
                n++;
            {
                const char *g = n;
                while (*g == ' ' || *g == '\t')
                    g++;
                if (*g == '<') {
                    snprintf(tmp, sizeof tmp, "cCall %.*s,%s",
                             (int)(n - s), s, g);
                    nasm_free(line);
                    return masm_rewrite_line(nasm_strdup(tmp));
                }
            }
        }
    }

    /*
     * Scalar param/local declarations carry their size in the mnemonic suffix
     * (parmB/localB = 1, parmW/localW = 2, parmD/localD = 4); record name->size
     * so MOVZX/MOVSX can size a bare source that names one (see masm_lszs).
     */
    if ((!nasm_strnicmp(w1, "parm", 4) || !nasm_strnicmp(w1, "local", 5)) &&
        l1 >= 5) {
        char suf = nasm_tolower(w1[l1 - 1]);
        int bytes = suf == 'b' ? 1 : suf == 'w' ? 2 : suf == 'd' ? 4 : 0;
        if (bytes && (l1 == 5 || l1 == 6)) {   /* parmX (5) / localX (6) only */
            const char *q = p;
            char nm[128];
            if (masm_word(&q, nm, sizeof nm))
                masm_lsz_add(nm, bytes);
        }
    }

    /* MASM `EVEN' aligns the location counter to a word boundary -> `align 2'. */
    if (!nasm_stricmp(w1, "even")) {
        const char *e = p;
        while (*e == ' ' || *e == '\t')
            e++;
        if (!*e || *e == ';') {
            nasm_free(line);
            return nasm_strdup("align 2");
        }
    }

    /*
     * A label or branch target named like an FPU stack register (`st1:',
     * `jnz st1').  NASM reserves ST0..ST7 (the disassembly corpus uses them as
     * real FPU registers, so they cannot be un-reserved like the 64-bit GPR
     * names are in the tokeniser), so escape the name with `$'.  A `stN' label
     * DEFINITION (`st1:'), and a standalone `stN' OPERAND of a non-FPU
     * instruction (FPU mnemonics all begin with `f', and only FPU ops take an
     * ST register), are both unambiguously the label.
     */
    if (masm_is_streg(w1) && *p == ':') {
        snprintf(tmp, sizeof tmp, "$%s%s", w1, p);
        nasm_free(line);
        return masm_rewrite_line(nasm_strdup(tmp));
    }
    if (w1[0] != 'f' && w1[0] != 'F' && strchr(p, 's') && strchr(p, 't')) {
        char esc[1024];
        if (masm_escape_streg(esc, sizeof esc, p)) {
            snprintf(tmp, sizeof tmp, "%s %s", w1, esc);
            nasm_free(line);
            return masm_rewrite_line(nasm_strdup(tmp));
        }
    }

    if (!nasm_stricmp(w1, "purge")) {
        /* PURGE removes macro definitions; we simply leave them defined (NASM
         * permits redefinition), so accept and drop. */
        nasm_free(line);
        return nasm_strdup("");
    }

    /*
     * LAR/LSL read a selector from r/m16; the operand size follows the
     * destination register, not the selector variable's storage width.  MASM
     * writes a spurious size cast on the source (`lar eax, dword ptr handle',
     * `lsl ecx, dword ptr selector'), which NASM rejects as a dword memory
     * source.  Normalise a leading `<size> [ptr]' on the source operand to
     * `word' so it goes through the same (working) path as a hand-written
     * `word ptr' -- then let the usual operand rewrite finish the line.
     */
    if (!nasm_stricmp(w1, "lar") || !nasm_stricmp(w1, "lsl")) {
        const char *c = strchr(p, ',');
        if (c) {
            const char *s = c + 1;
            char szw[16];
            size_t n = 0;
            const char *t;
            while (*s == ' ' || *s == '\t')
                s++;
            t = s;
            while (nasm_isidchar(*t) && n + 1 < sizeof szw)
                szw[n++] = *t++;
            szw[n] = '\0';
            if (n && (!nasm_stricmp(szw, "byte")  || !nasm_stricmp(szw, "word") ||
                      !nasm_stricmp(szw, "dword") || !nasm_stricmp(szw, "fword") ||
                      !nasm_stricmp(szw, "qword") || !nasm_stricmp(szw, "tbyte"))) {
                snprintf(tmp, sizeof tmp, "%s %.*sword%s",
                         w1, (int)(s - p), p, t);
                nasm_free(line);
                return masm_rewrite_line(nasm_strdup(tmp));
            }
        }
    }

    /*
     * MOVZX/MOVSX need an explicit source size that NASM cannot infer from a
     * MASM memory operand.  MASM takes it from the source's declared TYPE (a
     * struct field's DB/DW, or a typed data label).  Look that type up and
     * inject `byte'/`word' before the source (`movzx ax, [esi].pga_pglock' ->
     * `movzx ax, byte [esi].pga_pglock'); leave the line for the operand
     * rewrite to fold `.field'.  Sources already carrying a size, register
     * sources, and params/locals of unknown size are left untouched.
     */
    if (!nasm_stricmp(w1, "movzx") || !nasm_stricmp(w1, "movsx")) {
        const char *c = strchr(p, ',');
        if (c) {
            const char *s = c + 1;
            char first[16];
            size_t fn = 0;
            const char *u;
            while (*s == ' ' || *s == '\t')
                s++;
            u = s;                              /* first word of the source */
            while (nasm_isidchar(*u) && fn + 1 < sizeof first)
                first[fn++] = *u++;
            first[fn] = '\0';
            /* proceed unless the source already carries a size keyword */
            bool sized = fn &&
                (!nasm_stricmp(first, "byte")  || !nasm_stricmp(first, "word") ||
                 !nasm_stricmp(first, "dword") || !nasm_stricmp(first, "fword") ||
                 !nasm_stricmp(first, "qword") || !nasm_stricmp(first, "tbyte"));
            if (!sized) {
                /* size-determining id: field after the last `.', else the last
                 * identifier run in the source (`es:[PDB_block_len]' -> the
                 * bracketed label; a bare `[esi]' -> the register, size 0). */
                char id[128];
                const char *dot = NULL, *q, *idstart = NULL, *idend = NULL;
                for (q = s; *q && *q != ',' && *q != ';'; q++) {
                    if (*q == '.')
                        dot = q;
                    if (nasm_isidchar(*q)) {
                        if (!idstart || (q > s && !nasm_isidchar(q[-1])))
                            idstart = q;
                        idend = q + 1;
                    }
                }
                if (dot) {
                    idstart = dot + 1;
                    idend = idstart;
                    while (nasm_isidchar(*idend))
                        idend++;
                }
                if (idstart && idend > idstart &&
                    (size_t)(idend - idstart) < sizeof id) {
                    int sz;
                    memcpy(id, idstart, idend - idstart);
                    id[idend - idstart] = '\0';
                    sz = masm_field_bytes(id);
                    if (!sz)
                        sz = masm_type_query(id);
                    if (!sz)
                        sz = masm_lsz_get(id);
                    if (sz == 1 || sz == 2) {
                        snprintf(tmp, sizeof tmp, "%s %.*s%s %s", w1,
                                 (int)(s - p), p, sz == 1 ? "byte" : "word", s);
                        nasm_free(line);
                        return masm_rewrite_line(nasm_strdup(tmp));
                    }
                }
            }
        }
    }

    /*
     * A bare MASM string instruction sized by its (documentation) operand:
     * `lods byte ptr es:[si]' -> `lodsb'.  NASM has only the b/w/d/q-suffixed
     * mnemonics, so pick the suffix from a size keyword in the operand and drop
     * the operand.  An optional REP-family prefix is carried through:
     * `rep stos dword ptr es:[edi]' -> `rep stosd'.
     */
    {
        static const char *const sops[] = {
            "lods", "stos", "movs", "scas", "cmps", "ins", "outs", NULL
        };
        const char *opw = w1;           /* the string-op mnemonic */
        const char *scan = p;           /* where to scan for the size keyword */
        const char *rep = "";           /* optional REP/REPE/REPNE prefix + ' ' */
        char opbuf[16], repbuf[16];
        int si2;
        if (!nasm_stricmp(w1, "rep")   || !nasm_stricmp(w1, "repe") ||
            !nasm_stricmp(w1, "repz")  || !nasm_stricmp(w1, "repne") ||
            !nasm_stricmp(w1, "repnz")) {
            const char *q = p;
            if (masm_word(&q, opbuf, sizeof opbuf)) {
                snprintf(repbuf, sizeof repbuf, "%s ", w1);
                rep = repbuf;
                opw = opbuf;
                scan = q;               /* rest after the op word */
            }
        }
        for (si2 = 0; sops[si2]; si2++)
            if (!nasm_stricmp(opw, sops[si2]))
                break;
        if (sops[si2]) {
            char sfx = 0;
            const char *r = scan;
            while (*r && !sfx) {
                while (*r == ' ' || *r == '\t' || *r == ',')
                    r++;
                if (nasm_isidstart(*r)) {
                    char sw[16];
                    size_t n = 0;
                    const char *s = r;
                    while (nasm_isidchar(*s)) {
                        if (n < sizeof sw - 1) sw[n] = *s;
                        n++; s++;
                    }
                    sw[n < sizeof sw ? n : sizeof sw - 1] = '\0';
                    r = s;
                    if      (!nasm_stricmp(sw, "byte"))  sfx = 'b';
                    else if (!nasm_stricmp(sw, "word"))  sfx = 'w';
                    else if (!nasm_stricmp(sw, "dword")) sfx = 'd';
                    else if (!nasm_stricmp(sw, "qword")) sfx = 'q';
                } else if (*r) {
                    r++;
                }
            }
            if (sfx) {
                snprintf(tmp, sizeof tmp, "%s%s%c", rep, opw, sfx);
                nasm_free(line);
                return nasm_strdup(tmp);
            }
        }
    }

    /* MASM COMMENT delim [text] ... delim  -- a block comment. */
    if (!nasm_stricmp(w1, "comment")) {
        const char *d = p;
        while (*d == ' ' || *d == '\t')
            d++;
        if (*d) {
            char delim = *d;
            const char *rest = strchr(d + 1, delim);
            nasm_free(line);
            if (!rest)                          /* opens a multi-line block */
                masm_comment_delim = delim;
            return nasm_strdup("");             /* delimiter found again -> one-liner */
        }
    }

    /* MASM listing / cross-reference directives: no-ops for code generation. */
    if (w1[0] == '.') {
        static const char *const noops[] = {
            ".xcref", ".cref", ".lall", ".sall", ".xall", ".list", ".nolist",
            ".xlist", ".lfcond", ".sfcond", ".tfcond", ".seq", ".alpha",
            ".listall", ".listif", ".listmacro", ".listmacroall", ".nolistif",
            ".nolistmacro", NULL
        };
        int i;
        for (i = 0; noops[i]; i++)
            if (!nasm_stricmp(w1, noops[i])) {
                nasm_free(line);
                return nasm_strdup("");
            }
    }

    /*
     * Inside a STRUCT definition: each line is `member TYPE [init]', which
     * becomes a NASM struc field `.member: resX count' (giving STRUCT.member
     * offsets and STRUCT_size for free).  `NAME ENDS' closes it.
     */
    if (masm_in_struct && !masm_is_cond(w1)) {
        char mtype[64];
        const char *mp = p;
        size_t mtl = masm_word(&mp, mtype, sizeof mtype);
        if (mtl && !nasm_stricmp(mtype, "ends")) {
            bool uni = masm_sdef_cur && masm_sdef_cur->is_union;
            if (uni) {                          /* emit the union size define */
                snprintf(tmp, sizeof tmp, "%%idefine %s_size %d",
                         masm_sdef_cur->name, masm_sdef_cur->usize);
                masm_ppq_add(nasm_strdup(tmp));
            }
            masm_in_struct = false;
            if (masm_sdef_cur) {                /* commit the definition */
                masm_sdef_cur->next = masm_sdefs;
                masm_sdefs = masm_sdef_cur;
                masm_sdef_cur = NULL;
            }
            nasm_free(line);
            if (!uni)
                masm_ppq_add(nasm_strdup("endstruc"));
            return masm_ppq_get();
        }
        {
            /*
             * An anonymous member is a bare data directive (`DB 21 DUP(?)') --
             * padding with no name.  Reserve the space, no field symbol.
             */
            const char *ares = masm_type_to_res(w1);
            if (ares) {
                int acount = 1, aesz = masm_type_bytes(w1);
                const char *ap = p;
                while (*ap == ' ' || *ap == '\t')
                    ap++;
                if (nasm_isdigit(*ap)) {
                    int n = atoi(ap);
                    const char *dp = ap;
                    while (nasm_isdigit(*dp)) dp++;
                    while (*dp == ' ' || *dp == '\t') dp++;
                    if (n > 0 && !nasm_strnicmp(dp, "dup", 3)) acount = n;
                }
                if (masm_sdef_cur && masm_sdef_cur->is_union &&
                    aesz * acount > masm_sdef_cur->usize)
                    masm_sdef_cur->usize = aesz * acount;
                if (masm_sdef_cur && masm_sdef_cur->is_union)
                    snprintf(tmp, sizeof tmp, ";");         /* union: no advance */
                else
                    snprintf(tmp, sizeof tmp, "resb %d", aesz * acount);
                nasm_free(line);
                masm_ppq_add(nasm_strdup(tmp));
                return masm_ppq_get();
            }
        }
        if (mtl) {
            const char *res = masm_type_to_res(mtype);
            int count = 1;
            int esz = masm_type_bytes(mtype);
            const char *rp = mp;
            while (*rp == ' ' || *rp == '\t')
                rp++;
            if (nasm_isdigit(*rp)) {            /* `N DUP(...)' -> count N */
                int n = atoi(rp);
                const char *dp = rp;
                while (nasm_isdigit(*dp))
                    dp++;
                while (*dp == ' ' || *dp == '\t')
                    dp++;
                if (n > 0 && !nasm_strnicmp(dp, "dup", 3))
                    count = n;
            }
            if (masm_sdef_cur) {                /* record for instance emission */
                struct masm_smember *mb;
                nasm_new(mb);
                mb->name = nasm_strdup(w1);
                mb->dir = masm_type_to_dd(mtype);
                mb->count = count;
                if (masm_sdef_cur->tail)
                    masm_sdef_cur->tail->next = mb;
                else
                    masm_sdef_cur->head = mb;
                masm_sdef_cur->tail = mb;
                if (masm_sdef_cur->is_union && esz * count > masm_sdef_cur->usize)
                    masm_sdef_cur->usize = esz * count;
            }
            if (masm_sdef_cur && masm_sdef_cur->is_union) {
                /* union member: fixed at offset 0 */
                snprintf(tmp, sizeof tmp, "%%idefine %s.%s 0",
                         masm_sdef_cur->name, w1);
            } else if (res)
                snprintf(tmp, sizeof tmp, ".%s: %s %d", w1, res, count);
            else        /* nested struct type: reserve <type>_size bytes */
                snprintf(tmp, sizeof tmp, ".%s: resb %s_size", w1, mtype);
            masm_ppq_add(nasm_strdup(tmp));
            /*
             * MASM makes each field name a bare offset symbol too (`la_handle'
             * == its offset in the struct), used in `size = la_handle' idioms.
             * Alias it to the NASM struc offset symbol STRUCT.field.
             */
            if (masm_sdef_cur && !masm_sdef_cur->is_union) {
                snprintf(tmp, sizeof tmp, "%%idefine %s %s.%s",
                         w1, masm_sdef_cur->name, w1);
                masm_ppq_add(nasm_strdup(tmp));
            }
            nasm_free(line);
            return masm_ppq_get();
        }
        return line;
    }

    if (!nasm_stricmp(w1, "local") && masm_ppstk && masm_ppstk->is_macro) {
        /*
         * MASM `LOCAL a, b' inside a MACRO body declares macro-local LABELS
         * (a fresh unique symbol per expansion) -- unlike the MASM-6 PROC
         * `LOCAL v:type' stack local (masm.mac's `local' macro, used only in
         * .MODEL/PROC code).  Inside a MASM macro definition, bind each name
         * to NASM's per-expansion `%%name' so body references and the `name:'
         * definition resolve to the same fresh label on every invocation.
         */
        const char *q = p;
        char nm[128];
        bool any = false;
        while (*q) {
            size_t n = 0;
            while (*q == ' ' || *q == '\t' || *q == ',')
                q++;
            if (!nasm_isidstart(*q))
                break;
            while (nasm_isidchar(*q) && n + 1 < sizeof nm)
                nm[n++] = *q++;
            nm[n] = '\0';
            if (*q == ':')              /* drop a `:type'/`:REQ' qualifier */
                while (*q && *q != ',')
                    q++;
            snprintf(tmp, sizeof tmp, "%%define %s %%%%%s", nm, nm);
            masm_ppq_add(nasm_strdup(tmp));
            any = true;
        }
        nasm_free(line);
        if (!any)
            return nasm_strdup("");
        return masm_ppq_get();
    }

    if (!nasm_stricmp(w1, "endm")) {
        struct masm_ppblk *b = masm_ppstk;
        if (!b)
            return line;                /* stray ENDM: let NASM diagnose */
        masm_ppstk = b->next;
        if (b->is_for) {
            /* Close the temp macro, then replay it once per list item. */
            int i;
            masm_ppq_add(nasm_strdup("%endmacro"));
            for (i = 0; i < b->nitem; i++) {
                snprintf(tmp, sizeof tmp, "%s %s", b->forname, b->item[i]);
                masm_ppq_add(nasm_strdup(tmp));
                nasm_free(b->item[i]);
            }
            snprintf(tmp, sizeof tmp, "%%unmacro %s 1", b->forname);
            masm_ppq_add(nasm_strdup(tmp));
            nasm_free(b->item);
            nasm_free(b->forname);
        } else if (b->is_macro) {
            int i;
            for (i = 0; i < b->nparam; i++)   /* params were substituted textually */
                nasm_free(b->param[i]);
            nasm_free(b->param);
            masm_ppq_add(nasm_strdup("%endmacro"));
        } else {
            masm_ppq_add(nasm_strdup("%endrep"));
        }
        nasm_free(b);
        nasm_free(line);
        return masm_ppq_get();
    }

    if (!nasm_stricmp(w1, "exitm")) {
        nasm_free(line);
        masm_ppq_add(nasm_strdup("%exitmacro"));
        return masm_ppq_get();
    }

    if (!nasm_stricmp(w1, "include")) {
        /* MASM `include file' (bare, unquoted)  ->  NASM %include "file". */
        const char *f = p;
        char fn[256];
        size_t fl;
        while (*f == ' ' || *f == '\t')
            f++;
        snprintf(fn, sizeof fn, "%s", f);
        fl = strlen(fn);
        {                               /* strip a trailing `;' comment */
            char *sc = strchr(fn, ';');
            if (sc) { *sc = '\0'; fl = (size_t)(sc - fn); }
        }
        while (fl && (fn[fl-1]==' '||fn[fl-1]=='\t'||fn[fl-1]=='\r'||fn[fl-1]=='\n'))
            fn[--fl] = '\0';
        if (fl) {
            /*
             * cmacros.inc builds its procedure/segment macros with generated
             * (nested-&-macro) machinery that NASM's macro model cannot express.
             * Transparently substitute our NASM-native shim, which provides the
             * same call interface.  (Basename match, case-insensitive.)
             */
            const char *base = fn, *s2;
            for (s2 = fn; *s2; s2++)
                if (*s2 == '/' || *s2 == '\\')
                    base = s2 + 1;
            if (!nasm_stricmp(base, "cmacros.inc"))
                snprintf(tmp, sizeof tmp, "%%include \"cmacros_shim.inc\"");
            else
                snprintf(tmp, sizeof tmp, "%%include \"%s\"", fn);
            nasm_free(line);
            masm_ppq_add(nasm_strdup(tmp));
            return masm_ppq_get();
        }
    }

    if (!nasm_stricmp(w1, "includelib")) {
        nasm_free(line);            /* link-time detail; accepted and dropped */
        return nasm_strdup("");
    }

    /*
     * `PUBLIC a, b, ...' -> `global a, b, ...'.  MASM also allows `PUBLIC
     * name:type' (proc/abs qualifiers); NASM's `global' rejects a bare `:type'
     * here, so drop any `:qualifier' from each name.
     */
    if (!nasm_stricmp(w1, "public")) {
        char names[512];
        size_t ni = 0;
        const char *r = p;
        bool skip = false;
        while (*r == ' ' || *r == '\t')
            r++;
        for (; *r && *r != ';' && ni + 1 < sizeof names; r++) {
            if (*r == ':') { skip = true; continue; }
            if (*r == ',') { skip = false; names[ni++] = ','; continue; }
            if (skip) continue;
            names[ni++] = *r;
        }
        names[ni] = '\0';
        while (ni && (names[ni-1]==' '||names[ni-1]=='\t'||names[ni-1]==','))
            names[--ni] = '\0';
        if (ni)
            snprintf(tmp, sizeof tmp, "global %s", names);
        else
            tmp[0] = '\0';
        nasm_free(line);
        masm_ppq_add(nasm_strdup(tmp));
        return masm_ppq_get();
    }

    /* `EXTRN name:type, ...' is the MASM spelling of NASM's `extern'; the
     * `:type' is documentation NASM does not need, so strip it.  (Only the
     * MASM `extrn' spelling -- native `extern sym:type' is left to NASM.) */
    if (!nasm_stricmp(w1, "extrn")) {
        char *res = masm_extern_emit(p, 0);  /* size from each name's :type */
        nasm_free(line);                     /* after: p pointed into line */
        return res;
    }

    /*
     * cmacros typed external declarations: externW/D/B name a WORD/DWORD/BYTE
     * data symbol (register the type so a bare reference reads as memory);
     * externFP/NP/P/A name code/abs symbols (no data type).  Handled here so
     * the type survives -- the shim macro cannot register it (no C access).
     */
    {
        int esz = -1;
        if      (!nasm_stricmp(w1, "externW")) esz = 2;
        else if (!nasm_stricmp(w1, "externD")) esz = 4;
        else if (!nasm_stricmp(w1, "externB")) esz = 1;
        else if (!nasm_stricmp(w1, "externA") || !nasm_stricmp(w1, "externFP") ||
                 !nasm_stricmp(w1, "externNP") || !nasm_stricmp(w1, "externP"))
            esz = 0;
        if (esz >= 0) {
            char *res = masm_extern_emit(p, esz);
            nasm_free(line);            /* after: p pointed into line */
            return res;
        }
    }

    /* Segment-register assumptions carry no encoding in flat/obj output. */
    if (!nasm_stricmp(w1, "assume") || !nasm_stricmp(w1, "assumes")) {
        nasm_free(line);
        return nasm_strdup("");
    }

    /*
     * global<W/D/B> / static<W/D/B> name, <init-expr> [, count] -- MASM groups
     * a multi-token data initialiser in <...> (so its spaces do not split it
     * into extra args): `globalW p,<dataOffset foo>'.  Strip the outer <...>
     * (leaving inner `<<' shift operators alone) so the shim macro receives the
     * bare expression; then fall through to the normal operator rewrite.
     */
    if ((!nasm_strnicmp(w1, "global", 6) || !nasm_strnicmp(w1, "static", 6)) &&
        (w1[6]=='W'||w1[6]=='w'||w1[6]=='D'||w1[6]=='d'||w1[6]=='B'||w1[6]=='b')
        && w1[7] == '\0' && strchr(p, '<')) {
        char *lt = strchr(line, '<');
        char *gt = strrchr(line, '>');
        if (lt && gt && gt > lt) {
            memmove(gt, gt + 1, strlen(gt + 1) + 1);   /* drop the `>' */
            memmove(lt, lt + 1, strlen(lt + 1) + 1);   /* drop the `<' */
        }
        return masm_rewrite_line(line);                /* size/. operators */
    }

    if (!nasm_stricmp(w1, "bits")) {
        /*
         * A raw `bits N' (rather than .386/.MODEL) must also update the struc-
         * safe __?MASM_BITS?__ shadow, or a later `segment' would take its USE
         * from the stale default and mis-size the code.
         */
        const char *b = p;
        while (*b == ' ' || *b == '\t')
            b++;
        if (nasm_isdigit(*b)) {
            int n = atoi(b);
            snprintf(tmp, sizeof tmp, "%%assign __?MASM_BITS?__ %d", n);
            masm_ppq_add(nasm_strdup(tmp));
            snprintf(tmp, sizeof tmp, "bits %d", n);
            masm_ppq_add(nasm_strdup(tmp));
            nasm_free(line);
            return masm_ppq_get();
        }
    }

    if (!nasm_stricmp(w1, "for")  || !nasm_stricmp(w1, "irp") ||
        !nasm_stricmp(w1, "forc") || !nasm_stricmp(w1, "irpc")) {
        /*
         * FOR/IRP param, <a,b,c>   (IRPC/FORC: iterate the characters of arg)
         * Lower to a fresh 1-arg macro whose body is the loop body, replayed
         * once per list item at ENDM.
         */
        bool bychar = !nasm_stricmp(w1, "forc") || !nasm_stricmp(w1, "irpc");
        char param[128], nm[64];
        const char *q = p;
        size_t pn = 0;
        struct masm_ppblk *b;
        char **item = NULL;
        int nitem = 0;

        while (*q == ' ' || *q == '\t')
            q++;
        while (nasm_isidchar(*q)) {          /* the loop parameter name */
            if (pn + 1 < sizeof param)
                param[pn++] = *q;
            q++;
        }
        param[pn] = '\0';
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q == ',')
            q++;
        while (*q == ' ' || *q == '\t')
            q++;

        if (bychar) {                        /* each character becomes an item */
            const char *s = q;
            if (*s == '<') s++;
            for (; *s && *s != '>'; s++) {
                char one[2];
                if (*s == ' ' || *s == '\t')
                    continue;
                one[0] = *s; one[1] = '\0';
                item = nasm_realloc(item, (nitem + 1) * sizeof(char *));
                item[nitem++] = nasm_strdup(one);
            }
        } else {                             /* split the <...> list on commas */
            const char *s = q;
            int depth = 0;
            char buf[192];
            size_t bn = 0;
            if (*s == '<') { s++; depth = 1; }
            for (; *s; s++) {
                if (*s == '<') { depth++; }
                else if (*s == '>') { if (depth <= 1) break; depth--; }
                if (*s == ',' && depth <= 1) {
                    buf[bn] = '\0';
                    {   char *t = buf; while (*t==' '||*t=='\t') t++;
                        size_t e = strlen(t);
                        while (e && (t[e-1]==' '||t[e-1]=='\t')) t[--e]='\0';
                        item = nasm_realloc(item, (nitem+1)*sizeof(char*));
                        item[nitem++] = nasm_strdup(t); }
                    bn = 0;
                    continue;
                }
                if (bn + 1 < sizeof buf)
                    buf[bn++] = *s;
            }
            buf[bn] = '\0';                  /* the final item */
            {   char *t = buf; while (*t==' '||*t=='\t') t++;
                size_t e = strlen(t);
                while (e && (t[e-1]==' '||t[e-1]=='\t')) t[--e]='\0';
                if (*t) {
                    item = nasm_realloc(item, (nitem+1)*sizeof(char*));
                    item[nitem++] = nasm_strdup(t);
                } }
        }

        snprintf(nm, sizeof nm, "__?masm_for%d?__", ++masm_for_seq);
        snprintf(tmp, sizeof tmp, "%%macro %s 1", nm);
        masm_ppq_add(nasm_strdup(tmp));
        snprintf(tmp, sizeof tmp, "%%define %s %%1", param);
        masm_ppq_add(nasm_strdup(tmp));
        nasm_new(b);
        b->is_for = true;
        b->forname = nasm_strdup(nm);
        b->item = item;
        b->nitem = nitem;
        b->next = masm_ppstk;
        masm_ppstk = b;
        nasm_free(line);
        return masm_ppq_get();
    }

    if (!nasm_stricmp(w1, "rept") || !nasm_stricmp(w1, "repeat")) {
        struct masm_ppblk *b;
        while (*p == ' ' || *p == '\t')
            p++;
        snprintf(tmp, sizeof tmp, "%%rep %s", p);
        nasm_new(b);
        b->next = masm_ppstk;
        masm_ppstk = b;
        nasm_free(line);
        masm_ppq_add(nasm_strdup(tmp));
        return masm_ppq_get();
    }

    {
        /*
         * Conditional assembly: MASM's IF-family -> NASM's %if-family.  NASM's
         * preprocessor tracks %if/%endif nesting itself, so no block stack is
         * needed here.  (The runtime .IF/.ENDIF of Track B are dot-prefixed and
         * handled in the package; these bare forms are assembly-time.)
         */
        const char *rest = p;
        const char *dir = NULL;
        while (*rest == ' ' || *rest == '\t')
            rest++;
        if      (!nasm_stricmp(w1, "if"))     dir = "%if";
        else if (!nasm_stricmp(w1, "ifdef"))  dir = "%ifdef";
        else if (!nasm_stricmp(w1, "ifndef")) dir = "%ifndef";
        else if (!nasm_stricmp(w1, "elseif")) dir = "%elif";
        /* Pass-specific IF1/IF2: this preprocessor runs once, so IF1 (pass-1
         * one-time setup) is always taken and IF2 is never taken. */
        else if (!nasm_stricmp(w1, "if1"))    { dir = "%if"; rest = "1"; }
        else if (!nasm_stricmp(w1, "if2"))    { dir = "%if"; rest = "0"; }
        if (dir) {
            if (!nasm_stricmp(dir, "%if") || !nasm_stricmp(dir, "%elif")) {
                char ex[512];
                masm_xlat_ops(ex, sizeof ex, rest);  /* IF/ELSEIF: word ops */
                /* %elif cannot be preceded by guard blocks (it would break the
                 * %if chain); only default option switches on a fresh %if. */
                if (!nasm_stricmp(dir, "%if"))
                    masm_emit_opt_guards(ex);
                snprintf(tmp, sizeof tmp, "%s %s", dir, ex);
            } else {
                snprintf(tmp, sizeof tmp, "%s %s", dir, rest);
            }
            nasm_free(line);
            masm_ppq_add(nasm_strdup(tmp));
            return masm_ppq_get();
        }
        if (!nasm_stricmp(w1, "ife")) {         /* IFE: assemble if expr == 0 */
            char ex[512], *c;
            char qc = 0;
            size_t el;
            masm_xlat_ops(ex, sizeof ex, rest);
            /* Strip a trailing `;' comment: wrapping in `(...) == 0' would
             * otherwise let the comment swallow the closing `) == 0'. */
            for (c = ex; *c; c++) {
                if (qc) { if (*c == qc) qc = 0; }
                else if (*c == '\'' || *c == '"') qc = *c;
                else if (*c == ';') { *c = '\0'; break; }
            }
            el = strlen(ex);
            while (el && (ex[el-1]==' '||ex[el-1]=='\t')) ex[--el] = '\0';
            masm_emit_opt_guards(ex);
            snprintf(tmp, sizeof tmp, "%%if (%s) == 0", ex);
            nasm_free(line);
            masm_ppq_add(nasm_strdup(tmp));
            return masm_ppq_get();
        }
        if (!nasm_stricmp(w1, "else")) {
            nasm_free(line);
            masm_ppq_add(nasm_strdup("%else"));
            return masm_ppq_get();
        }
        if (!nasm_stricmp(w1, "endif")) {
            nasm_free(line);
            masm_ppq_add(nasm_strdup("%endif"));
            return masm_ppq_get();
        }
        /*
         * Blank-argument and text-comparison conditionals.  MASM wraps text
         * arguments in <...>; NASM's %ifempty/%ifidn do not, so strip the
         * angle brackets from `rest' as we forward it.
         */
        {
            const char *cdir = NULL;
            if      (!nasm_stricmp(w1, "ifb")     || !nasm_stricmp(w1, "ifidn0"))
                cdir = "%ifempty";
            else if (!nasm_stricmp(w1, "ifnb"))   cdir = "%ifnempty";
            else if (!nasm_stricmp(w1, "ifidn"))  cdir = "%ifidn";
            else if (!nasm_stricmp(w1, "ifidni")) cdir = "%ifidni";
            else if (!nasm_stricmp(w1, "ifdif"))  cdir = "%ifnidn";
            else if (!nasm_stricmp(w1, "ifdifi")) cdir = "%ifnidni";
            if (cdir) {
                char stripped[256];
                size_t si = 0;
                const char *r = rest;
                for (; *r && si + 1 < sizeof stripped; r++)
                    if (*r != '<' && *r != '>')
                        stripped[si++] = *r;
                stripped[si] = '\0';
                snprintf(tmp, sizeof tmp, "%s %s", cdir, stripped);
                nasm_free(line);
                masm_ppq_add(nasm_strdup(tmp));
                return masm_ppq_get();
            }
        }
        /* Diagnostics: the .ERR family -> %error, guarded for the conditional
         * variants.  ECHO/%OUT (informational console output) are dropped. */
        if (!nasm_stricmp(w1, ".err")) {
            snprintf(tmp, sizeof tmp, "%%error %s", rest);
            nasm_free(line);
            masm_ppq_add(nasm_strdup(tmp));
            return masm_ppq_get();
        }
        if (!nasm_stricmp(w1, ".erre") || !nasm_stricmp(w1, ".errnz")) {
            /* .ERRE expr: error if expr == 0;  .ERRNZ expr: error if expr != 0.
             * The expression gets the same operator rewriting an IFE does
             * (word ops + SIZE/SIZEOF/`.member') so `.ERRNZ 32-size Foo' works;
             * a trailing `;' comment is stripped before the `(...) cmp 0' wrap. */
            const char *cmp = !nasm_stricmp(w1, ".erre") ? "==" : "!=";
            char ex[512], *rr, *c;
            char qc = 0;
            size_t el;
            masm_xlat_ops(ex, sizeof ex, rest);
            for (c = ex; *c; c++) {
                if (qc) { if (*c == qc) qc = 0; }
                else if (*c == '\'' || *c == '"') qc = *c;
                else if (*c == ';') { *c = '\0'; break; }
            }
            el = strlen(ex);
            while (el && (ex[el-1]==' '||ex[el-1]=='\t')) ex[--el] = '\0';
            rr = masm_rewrite_line(nasm_strdup(ex));
            snprintf(tmp, sizeof tmp, "%%if (%s) %s 0", rr, cmp);
            nasm_free(rr);
            masm_ppq_add(nasm_strdup(tmp));
            masm_ppq_add(nasm_strdup("%error assertion failed"));
            masm_ppq_add(nasm_strdup("%endif"));
            nasm_free(line);
            return masm_ppq_get();
        }
        if (!nasm_stricmp(w1, "echo") || !nasm_stricmp(w1, "%out") ||
            !nasm_stricmp(w1, ".radix")) {
            nasm_free(line);            /* informational / accepted-and-ignored */
            return nasm_strdup("");
        }
    }

    {
        /* NAME = value  ->  %assign NAME value  (redefinable numeric equate) */
        const char *pe = p;
        while (*pe == ' ' || *pe == '\t')
            pe++;
        if (*pe == '=' && pe[1] != '=') {
            char ex[512];
            char *ex2;
            masm_xlat_ops(ex, sizeof ex, masm_skip_typeptr(pe + 1));
            ex2 = masm_rewrite_line(nasm_strdup(ex));  /* SIZE/SIZEOF/... */
            /*
             * A MASM `=' is a redefinable numeric equate -> %assign (evaluated
             * now).  We do NOT bind it lazily (%define) to tolerate a forward
             * reference in the RHS: `=' also spells counters (`n = 0' then
             * `n = n + 1'), and those are pasted into identifiers (`foo&n'),
             * where a textual/parenthesised value would break.  A forward
             * reference in a `=' expression is the single-pass preprocessor's
             * boundary against MASM's two passes.  %iassign, not %assign:
             * MASM symbols are case-insensitive (matching the label folding).
             */
            snprintf(tmp, sizeof tmp, "%%iassign %s %s", w1, ex2);
            nasm_free(ex2);
            nasm_free(line);
            masm_ppq_add(nasm_strdup(tmp));
            return masm_ppq_get();
        }
    }

    l2 = masm_word(&p, w2, sizeof w2);

    if (l2 && (!nasm_stricmp(w2, "struct") || !nasm_stricmp(w2, "struc") ||
               !nasm_stricmp(w2, "union"))) {
        /*
         * NAME STRUCT  ->  struc NAME (members collected until NAME ENDS).
         * NAME UNION   ->  no struc; every member sits at offset 0 and the size
         *                  is the largest member (emitted as NAME.m/NAME_size
         *                  defines at ENDS).
         */
        struct masm_sdef *sd;
        masm_in_struct = true;
        nasm_new(sd);
        sd->name = nasm_strdup(w1);
        sd->is_union = !nasm_stricmp(w2, "union");
        masm_sdef_cur = sd;
        nasm_free(line);
        if (sd->is_union)
            return nasm_strdup("");             /* union: nothing emitted yet */
        snprintf(tmp, sizeof tmp, "struc %s", w1);
        masm_ppq_add(nasm_strdup(tmp));
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "label")) {
        /*
         * NAME LABEL type  -- define NAME at the current location with the given
         * type.  Emit the plain label `NAME:'; for a data type (BYTE/WORD/...)
         * also register it so a bare reference reads as its contents, matching
         * a DB/DW/... definition.  NEAR/FAR/PROC are code labels (no data type).
         */
        char ty[32];
        const char *r = p;
        size_t tn = 0;
        int sz;
        while (*r == ' ' || *r == '\t')
            r++;
        while ((nasm_isidchar(*r)) && tn + 1 < sizeof ty)
            ty[tn++] = *r++;
        ty[tn] = '\0';
        sz = masm_type_bytes(ty);       /* 0 for near/far/proc/unknown */
        if (sz > 0)
            masm_type_note(w1, sz);
        snprintf(tmp, sizeof tmp, "%s:", w1);
        nasm_free(line);
        masm_ppq_add(nasm_strdup(tmp));
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "record")) {
        /*
         * NAME RECORD f1:w1, f2:w2, ...   -- a bit-packed record.  Fields pack
         * most-significant first; each field name is its shift count, MASK f is
         * the field's bit mask, WIDTH f its width.  The whole record is a byte /
         * word / dword by total width.
         */
        struct { char nm[64]; int w; } fld[32];
        int nf = 0, total = 0, i, pos;
        struct masm_sdef *sd;
        const char *s = p;
        while (*s && nf < 32) {
            char nm[64]; size_t ni = 0; int w = 0;
            while (*s == ' ' || *s == '\t' || *s == ',')
                s++;
            if (!nasm_isidstart(*s))
                break;
            while (nasm_isidchar(*s)) {
                if (ni + 1 < sizeof nm) nm[ni++] = *s;
                s++;
            }
            nm[ni] = '\0';
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s == ':') {
                s++;
                while (*s == ' ' || *s == '\t')
                    s++;
                w = atoi(s);
                while (nasm_isdigit(*s))
                    s++;
            }
            if (w <= 0)
                break;
            snprintf(fld[nf].nm, sizeof fld[nf].nm, "%s", nm);
            fld[nf].w = w;
            total += w;
            nf++;
        }
        nasm_new(sd);
        sd->name = nasm_strdup(w1);
        sd->is_record = true;
        sd->rdir = total <= 8 ? "db" : total <= 16 ? "dw" : "dd";
        pos = total;
        for (i = 0; i < nf; i++) {
            struct masm_smember *mb;
            int width = fld[i].w;
            unsigned mask;
            pos -= width;
            mask = (width >= 32 ? 0xffffffffu : ((1u << width) - 1)) << pos;
            snprintf(tmp, sizeof tmp, "%%idefine %s %d", fld[i].nm, pos);
            masm_ppq_add(nasm_strdup(tmp));
            snprintf(tmp, sizeof tmp, "%%define MASK_%s 0%08Xh", fld[i].nm, mask);
            masm_ppq_add(nasm_strdup(tmp));
            snprintf(tmp, sizeof tmp, "%%define WIDTH_%s %d", fld[i].nm, width);
            masm_ppq_add(nasm_strdup(tmp));
            nasm_new(mb);
            mb->name = nasm_strdup(fld[i].nm);
            mb->shift = pos;
            mb->count = 1;
            if (sd->tail) sd->tail->next = mb; else sd->head = mb;
            sd->tail = mb;
        }
        snprintf(tmp, sizeof tmp, "%%idefine %s_size %d", w1,
                 total <= 8 ? 1 : total <= 16 ? 2 : 4);
        masm_ppq_add(nasm_strdup(tmp));
        sd->next = masm_sdefs;
        masm_sdefs = sd;
        nasm_free(line);
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "catstr")) {
        /* NAME CATSTR a,b,...  -> concatenate the text of the args (angle
         * brackets stripped, joined with %+ so macro args expand and paste). */
        char rhs[512];
        size_t ri = 0;
        const char *s = p;
        int depth = 0, first = 1;
        while (*s == ' ' || *s == '\t')
            s++;
        while (*s) {
            if (*s == '<') { depth++; s++; continue; }
            if (*s == '>') { if (depth) depth--; s++; continue; }
            if (*s == ',' && depth == 0) {          /* arg separator */
                if (ri + 4 < sizeof rhs) {
                    rhs[ri++] = ' '; rhs[ri++] = '%'; rhs[ri++] = '+'; rhs[ri++] = ' ';
                }
                first = 0;
                s++;
                while (*s == ' ' || *s == '\t')
                    s++;
                continue;
            }
            if (ri + 1 < sizeof rhs)
                rhs[ri++] = *s;
            s++;
        }
        (void)first;
        rhs[ri] = '\0';
        snprintf(tmp, sizeof tmp, "%%ixdefine %s %s", w1, rhs);
        nasm_free(line);
        masm_ppq_add(nasm_strdup(tmp));
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "instr")) {
        /*
         * NAME INSTR [start,] <str1>, <str2>  --  1-based position of str2 in
         * str1 (from `start', default 1), or 0 if absent.  Literal <...> args
         * are searched here; the result is a numeric equate.
         */
        char part[3][256];
        int np = 0;
        size_t pi = 0;
        int depth = 0;
        const char *s = p;
        while (*s == ' ' || *s == '\t')
            s++;
        for (; *s && np < 3; s++) {             /* split top-level commas */
            if (*s == '<') { depth++; continue; }
            if (*s == '>') { if (depth) depth--; continue; }
            if (*s == ',' && depth == 0) {
                part[np][pi] = '\0'; np++; pi = 0; continue;
            }
            if (pi + 1 < sizeof part[0])
                part[np][pi++] = *s;
        }
        if (np < 3) { part[np][pi] = '\0'; np++; }
        {
            int start = 1, base = 0, pos = 0;
            const char *s1, *s2, *hit;
            if (np >= 3) {                      /* leading start index */
                start = atoi(part[0]);
                if (start < 1) start = 1;
                base = 1;
            }
            s1 = part[base];
            s2 = part[base + 1];
            while (*s1 == ' ' || *s1 == '\t') s1++;
            while (*s2 == ' ' || *s2 == '\t') s2++;
            if ((int)strlen(s1) >= start - 1) {
                hit = strstr(s1 + (start - 1), s2);
                if (hit)
                    pos = (int)(hit - s1) + 1;
            }
            snprintf(tmp, sizeof tmp, "%%iassign %s %d", w1, pos);
        }
        nasm_free(line);
        masm_ppq_add(nasm_strdup(tmp));
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "sizestr")) {
        /* NAME SIZESTR <text>  -> length of the text (a numeric equate). */
        const char *s = p;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '<') {                            /* literal: count in place */
            int n = 0, depth = 1;
            const char *t = s + 1;
            for (; *t; t++) {
                if (*t == '<') depth++;
                else if (*t == '>') { if (--depth == 0) break; }
                n++;
            }
            snprintf(tmp, sizeof tmp, "%%iassign %s %d", w1, n);
            masm_ppq_add(nasm_strdup(tmp));
        } else {          /* a text macro: stringify it, then measure */
            snprintf(tmp, sizeof tmp, "%%defstr __masm_sstmp %s", s);
            masm_ppq_add(nasm_strdup(tmp));
            snprintf(tmp, sizeof tmp, "%%strlen %s __masm_sstmp", w1);
            masm_ppq_add(nasm_strdup(tmp));
        }
        nasm_free(line);
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "substr")) {
        /*
         * NAME SUBSTR text, start[, len].  NASM's %substr needs a string source,
         * so stringify the first (text) argument via %defstr, then %substr.  A
         * literal <text> is quoted directly.
         */
        const char *s = p;
        char src[256], rest[256];
        size_t i = 0;
        int depth = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        for (; *s && i + 1 < sizeof src; s++) {     /* the text arg, up to comma */
            if (*s == '<') { depth++; continue; }
            if (*s == '>') { if (depth) depth--; continue; }
            if (*s == ',' && depth == 0)
                break;
            src[i++] = *s;
        }
        src[i] = '\0';
        while (i && (src[i-1]==' '||src[i-1]=='\t')) src[--i] = '\0';
        snprintf(rest, sizeof rest, "%s", (*s == ',') ? s + 1 : "");
        /* stringify source -> %substr into a temp string -> retokenise, so the
         * result is a token (like other MASM text macros) and chains cleanly. */
        snprintf(tmp, sizeof tmp, "%%defstr __masm_sstmp %s", src);
        masm_ppq_add(nasm_strdup(tmp));
        snprintf(tmp, sizeof tmp, "%%substr __masm_sstmp2 __masm_sstmp,%s", rest);
        masm_ppq_add(nasm_strdup(tmp));
        snprintf(tmp, sizeof tmp, "%%deftok %s __masm_sstmp2", w1);
        masm_ppq_add(nasm_strdup(tmp));
        nasm_free(line);
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "typedef")) {
        /*
         * NAME TYPEDEF spec  --  a type alias.  Make NAME usable as a data
         * directive and give it a size:
         *   TYPEDEF <primitive>       -> that directive / size
         *   TYPEDEF PTR x / PROTO ... -> a pointer: dd / 4 (32-bit flat)
         * (A pointer defined before .MODEL would be 16-bit/2 under ML; we use
         * the 32-bit size, which is what post-.MODEL headers expect.)
         */
        char sw[64];
        const char *sp = p;
        size_t swl;
        const char *dir;
        int sz;
        swl = masm_word(&sp, sw, sizeof sw);
        sz = swl ? masm_type_bytes(sw) : 0;
        if (sz) {
            dir = masm_type_to_dd(sw);
        } else {                                /* pointer / proto / alias */
            dir = "dd";
            sz = 4;
        }
        snprintf(tmp, sizeof tmp, "%%idefine %s %s", w1, dir ? dir : "dd");
        masm_ppq_add(nasm_strdup(tmp));
        snprintf(tmp, sizeof tmp, "%%idefine %s_size %d", w1, sz);
        masm_ppq_add(nasm_strdup(tmp));
        nasm_free(line);
        return masm_ppq_get();
    }

    {
        /*
         * A BARE struct instance `STRUCTTYPE <i0, i1, ...>' -- no name on this
         * line; the label came from a preceding `labelB <..>' / `name:' (kdata:
         * `labelB <PUBLIC,bootExecBlock>' then `EXECBLOCK <0,0,0,0>').  Emit
         * just the field data at the current location; `.member' access on the
         * preceding label resolves through the struct field offsets (var.field).
         */
        struct masm_sdef *sd0 = masm_sdef_find(w1);
        const char *ip0 = p;
        while (*ip0 == ' ' || *ip0 == '\t')
            ip0++;
        if (sd0 && !sd0->is_record && *ip0 == '<') {
            struct masm_smember *mb;
            char inits[32][192];
            int ninit = 0, mi, depth = 0;
            char *d = inits[0];
            size_t n = 0;
            const char *s;
            ip0++;                              /* past '<' */
            for (s = ip0; *s; s++) {
                if (*s == '<') depth++;
                else if (*s == '>') { if (depth == 0) break; depth--; }
                if (*s == ',' && depth == 0) {
                    d[n] = '\0';
                    if (ninit < 31) { ninit++; d = inits[ninit]; n = 0; }
                    continue;
                }
                if (n + 1 < sizeof inits[0])
                    d[n++] = *s;
            }
            d[n] = '\0';
            ninit++;
            for (mb = sd0->head, mi = 0; mb; mb = mb->next, mi++) {
                char val[192];
                const char *v = "0";
                const char *dir = mb->dir ? mb->dir : "dd";
                if (mi < ninit) {
                    char *b = inits[mi];
                    while (*b == ' ' || *b == '\t') b++;
                    snprintf(val, sizeof val, "%s", b);
                    { size_t e = strlen(val);
                      while (e && (val[e-1]==' '||val[e-1]=='\t'||val[e-1]=='\r'))
                          val[--e] = '\0'; }
                    if (val[0]) v = val;
                }
                if (mb->count > 1)
                    snprintf(tmp, sizeof tmp, "times %d %s %s", mb->count, dir, v);
                else
                    snprintf(tmp, sizeof tmp, "%s %s", dir, v);
                masm_ppq_add(nasm_strdup(tmp));
            }
            nasm_free(line);
            return masm_ppq_get();
        }
    }

    if (l2 && masm_sdef_find(w2)) {
        /*
         * `label STRUCTTYPE <i0, i1, ...>' -- a static struct instance.  Emit a
         * labelled data block so each field gets a real address; the MASM data-
         * label typing (keyed on the literal label string) then makes a bare
         * `label.member' reference mean its contents.  The field labels use the
         * full `label.member' name so the set-time and get-time keys match:
         *     label:
         *     label.m0  <dir0>  i0-or-0
         *     label.m1  <dir1>  i1-or-0
         */
        struct masm_sdef *sd = masm_sdef_find(w2);
        struct masm_smember *mb;
        const char *ip = p;
        char inits[32][192];
        int ninit = 0, mi;

        while (*ip == ' ' || *ip == '\t')
            ip++;
        if (*ip == '<') {                       /* split top-level <...> inits */
            int depth = 0;
            const char *s;
            char *d = inits[0];
            size_t n = 0;
            ip++;                               /* past '<' */
            for (s = ip; *s; s++) {
                if (*s == '<') depth++;
                else if (*s == '>') {
                    if (depth == 0) break;
                    depth--;
                }
                if (*s == ',' && depth == 0) {
                    d[n] = '\0';
                    if (ninit < 31) { ninit++; d = inits[ninit]; n = 0; }
                    continue;
                }
                if (n + 1 < sizeof inits[0])
                    d[n++] = *s;
            }
            d[n] = '\0';
            ninit++;                            /* count the last field */
        }

        if (sd->is_record) {
            /* pack the fields into one integer: label <dir> (i<<sh)|... */
            char rhs[512];
            size_t ri = 0;
            int first = 1;
            for (mb = sd->head, mi = 0; mb; mb = mb->next, mi++) {
                char val[192];
                const char *v = "0";
                if (mi < ninit) {
                    char *b = inits[mi];
                    while (*b == ' ' || *b == '\t') b++;
                    snprintf(val, sizeof val, "%s", b);
                    { size_t e = strlen(val);
                      while (e && (val[e-1]==' '||val[e-1]=='\t'||val[e-1]=='\r'))
                          val[--e] = '\0'; }
                    if (val[0]) v = val;
                }
                ri += snprintf(rhs + ri, ri < sizeof rhs ? sizeof rhs - ri : 0,
                               "%s((%s) << %d)", first ? "" : " | ", v, mb->shift);
                first = 0;
            }
            if (first)                          /* no fields: emit 0 */
                snprintf(rhs, sizeof rhs, "0");
            snprintf(tmp, sizeof tmp, "%s\t%s %s", w1, sd->rdir, rhs);
            masm_ppq_add(nasm_strdup(tmp));
            nasm_free(line);
            return masm_ppq_get();
        }

        snprintf(tmp, sizeof tmp, "%s:", w1);
        masm_ppq_add(nasm_strdup(tmp));
        for (mb = sd->head, mi = 0; mb; mb = mb->next, mi++) {
            char val[192];
            const char *v = "0";
            if (mi < ninit) {                   /* trim the field's init text */
                char *b = inits[mi];
                while (*b == ' ' || *b == '\t') b++;
                snprintf(val, sizeof val, "%s", b);
                { size_t e = strlen(val);
                  while (e && (val[e-1]==' '||val[e-1]=='\t'||val[e-1]=='\r'))
                      val[--e] = '\0'; }
                if (val[0])
                    v = val;
            }
            const char *dir = mb->dir ? mb->dir : "dd";  /* nested: best effort */
            if (mb->count > 1)
                snprintf(tmp, sizeof tmp, "%s.%s\ttimes %d %s %s",
                         w1, mb->name, mb->count, dir, v);
            else
                snprintf(tmp, sizeof tmp, "%s.%s\t%s %s", w1, mb->name, dir, v);
            masm_ppq_add(nasm_strdup(tmp));
        }
        nasm_free(line);
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "equ")) {
        /*
         * NAME EQU <text>  is a MASM text equate (redefinable text alias) ->
         * %xdefine.  A numeric/label EQU (no angle brackets) is left for NASM's
         * own `equ'.
         */
        const char *v = p;
        while (*v == ' ' || *v == '\t')
            v++;
        if (*v == '<') {
            char buf[512];
            size_t vn;
            snprintf(buf, sizeof buf, "%s", v);
            vn = strlen(buf);
            while (vn && (buf[vn-1]==' '||buf[vn-1]=='\t'||buf[vn-1]=='\r'))
                buf[--vn] = '\0';
            if (buf[0] == '<' && vn >= 2 && buf[vn-1] == '>') {
                buf[vn-1] = '\0';
                memmove(buf, buf + 1, vn - 1);
            }
            snprintf(tmp, sizeof tmp, "%%ixdefine %s %s", w1, buf);
            nasm_free(line);
            masm_ppq_add(nasm_strdup(tmp));
            return masm_ppq_get();
        }
        {
            /*
             * NAME EQU expr.  Drop a leading <size> PTR cast, translate MASM
             * word operators (or/and/...), then hand to NASM's `equ'.  The one
             * exception is a bare size keyword (`RSHORT EQU short'): NASM's equ
             * rejects it, so alias it with %define.  A general lone-identifier
             * equate is left as `equ' -- %define there risks a definition cycle
             * (a redefinable/forward equate referencing itself), which hangs the
             * preprocessor.
             */
            char ex[512], *xr;
            masm_xlat_ops(ex, sizeof ex, masm_skip_typeptr(v));
            xr = ex;
            while (*xr == ' ' || *xr == '\t')
                xr++;
            if (*xr == '[') {
                /*
                 * NAME EQU [mem].field...  -- an alias for a MEMORY operand
                 * (cmacros frame accessors: `wParam equ [pFrame].wp_wParam').
                 * NASM's `equ' rejects a memory operand, so bind it textually
                 * (%define) after rewriting `].field' member access to
                 * `+ field]'.
                 */
                char *mr = masm_rewrite_line(nasm_strdup(xr));
                snprintf(tmp, sizeof tmp, "%%idefine %s %s", w1, mr);
                nasm_free(mr);
            } else if (masm_ident_only(xr) &&
                (!nasm_stricmp(xr, "short") || !nasm_stricmp(xr, "near") ||
                 !nasm_stricmp(xr, "far")))
                snprintf(tmp, sizeof tmp, "%%idefine %s %s", w1, xr);
            else {
                /* Rewrite operators the same way an instruction operand is
                 * (SIZE/SIZEOF/TYPE/`.member'): `DSC_LEN equ (size DscPtr)'
                 * -> `... equ (DscPtr_size)'. */
                char *rr = masm_rewrite_line(nasm_strdup(xr));
                snprintf(tmp, sizeof tmp, "%s equ %s", w1, rr);
                nasm_free(rr);
            }
            nasm_free(line);
            masm_ppq_add(nasm_strdup(tmp));
            return masm_ppq_get();
        }
    }

    if (l2 && !nasm_stricmp(w2, "textequ")) {
        /* NAME TEXTEQU value  ->  %xdefine NAME value  (strip outer <>) */
        const char *v = p;
        char buf[512];
        size_t vn;
        while (*v == ' ' || *v == '\t')
            v++;
        snprintf(buf, sizeof buf, "%s", v);
        vn = strlen(buf);
        while (vn && (buf[vn-1] == ' ' || buf[vn-1] == '\t' || buf[vn-1] == '\r'))
            buf[--vn] = '\0';
        if (buf[0] == '<' && vn >= 2 && buf[vn-1] == '>') {
            buf[vn-1] = '\0';
            memmove(buf, buf + 1, vn - 1);
        }
        snprintf(tmp, sizeof tmp, "%%ixdefine %s %s", w1, buf);
        nasm_free(line);
        masm_ppq_add(nasm_strdup(tmp));
        return masm_ppq_get();
    }

    if (l2 && !nasm_stricmp(w2, "macro")) {
        struct masm_ppblk *b;
        int nparam = 0, i;
        char **param = NULL;
        const char *q = p;

        while (*q == ' ' || *q == '\t')
            q++;
        while (*q) {                    /* comma-separated parameter names */
            char nm[128];
            size_t n = 0;
            while (*q == ' ' || *q == '\t')
                q++;
            while (nasm_isidchar(*q)) {
                if (n + 1 < sizeof nm)
                    nm[n] = *q;
                n++;
                q++;
            }
            nm[n < sizeof nm ? n : sizeof nm - 1] = '\0';
            if (n) {
                param = nasm_realloc(param, (nparam + 1) * sizeof(char *));
                param[nparam++] = nasm_strdup(nm);
            }
            while (*q && *q != ',')     /* skip :REQ / :=default etc. */
                q++;
            if (*q == ',')
                q++;
            else
                break;
        }

        /* MASM macro parameters are all optional (an omitted one is blank, as
         * IFB tests) AND a MASM caller may pass extra arguments -- notably a
         * `<...>' list argument, which NASM splits on its inner commas into
         * several arguments (`WOWTrace msg,<<ax,f>,<bx,n>>').  So declare the
         * macro VARIADIC (`0-*', unbounded, like the shim's cCall) rather than
         * an exact upper bound: the named params still bind to %1..%N and any
         * overflow is ignored.  `*' (unbounded args), NOT `+' (a greedy last
         * param that re-joins with commas -- that form is pathologically slow).
         * %imacro (not %macro): MASM macro names are case-insensitive too. */
        (void)nparam;
        snprintf(tmp, sizeof tmp, "%%imacro %s 0-*", w1);
        masm_ppq_add(nasm_strdup(tmp));
        /* Parameters are substituted textually in the body (masm_subst_params),
         * so no `%define param %N' smacros are emitted here. */
        (void)i;
        nasm_new(b);
        b->is_macro = true;
        b->nparam = nparam;
        b->param = param;
        b->next = masm_ppstk;
        masm_ppstk = b;
        nasm_free(line);
        return masm_ppq_get();
    }

    /* Plain line: apply struct-member / SIZEOF operand rewrites. */
    return masm_rewrite_line(line);
}

static char *read_line(void)
{
    char *line;
    FILE *f = istk->fp;

    if (masm_mode) {
        line = masm_ppq_get();          /* drain queued translated lines first */
        if (line)
            return line;
    }

    if (f)
        line = line_from_file(f);
    else
        line = line_from_stdmac();

    if (!line)
        return NULL;

    if (masm_mode)
        line = masm_pp_xform(line);     /* MASM MACRO/ENDM/REPT -> NASM */

    if (!istk->nolist)
        lfmt->line(LIST_READ, istk->where.lineno, line);

    return line;
}

/*
 * Tokenize a line of text. This is a very simple process since we
 * don't need to parse the value out of e.g. numeric tokens: we
 * simply split one string into many.
 */
static Token *tokenize(const char *line)
{
    enum token_type type;
    Token *list = NULL;
    Token *t, **tail = &list;

    while (*line) {
        const char *p = line;
        const char *ep = NULL;  /* End of token, for trimming the end */
        size_t toklen;
        char firstchar = *p;    /* Can be used to override the first char */

        if (*p == '%') {
            /*
             * Preprocessor construct; find the end of the token.
             * Classification is handled later, because %{...} can be
             * used to create any preprocessor token.
             */
            p++;
            if (*p == '+' && !nasm_isdigit(p[1])) {
                /* Paste token */
                p++;
            } else if (nasm_isdigit(*p) ||
                       ((*p == '-' || *p == '+') && nasm_isdigit(p[1]))) {
                do {
                    p++;
                }
                while (nasm_isdigit(*p));
            } else if (*p == '{' || *p == '[') {
                /* %{...} or %[...] */
                char firstchar = *p;
                char endchar = *p + 2; /* } or ] */
                int lvl = 1;
                line += (*p++ == '{'); /* Skip { but not [ (yet) */
                while (lvl) {
                    if (*p == firstchar) {
                        lvl++;
                    } else if (*p == endchar) {
                        lvl--;
                    } else if (nasm_isquote(*p)) {
                        p = nasm_skip_string(p);
                    }

                    /*
                     * *p can have been advanced to a null character by
                     * nasm_skip_string()
                     */
                    if (!*p) {
                        nasm_warn(firstchar == '}' ?
                                  WARN_PP_OPEN_BRACES : WARN_PP_OPEN_BRACKETS,
                                  "unterminated %%%c...%c construct (missing `%c')",
                                  firstchar, endchar, endchar);
                        break;
                    }
                    p++;
                }
                ep = lvl ? p : p-1; /* Terminal character not part of token */
            } else if (*p == '?') {
                /* %? or %?? */
                p++;
                if (*p == '?')
                    p++;
            } else if (*p == '*' && p[1] == '?') {
                /* %*? and %*?? */
                p += 2;
                if (*p == '?')
                    p++;
            } else if (*p == '!') {
                /* Environment variable reference */
                p++;
                if (nasm_isidchar(*p)) {
                    do {
                        p++;
                    }
                    while (nasm_isidchar(*p));
                } else if (nasm_isquote(*p)) {
                    p = nasm_skip_string(p);
                    if (*p)
                        p++;
                    else
                        nasm_nonfatal("unterminated %%! string");
                } else {
                    /* %! without anything else... */
                }
            } else if (*p == ',') {
                /* Conditional comma */
                p++;
            } else if (nasm_isidchar(*p) ||
                       (*p == '%' && nasm_isidchar(p[1]))) {
                /* Identifier or some sort */
                do {
                    p++;
                }
                while (nasm_isidchar(*p));
            } else if (*p == '%') {
                /* %% operator */
                p++;
            }

            if (!ep)
                ep = p;
            toklen = ep - line;

            /* Classify here, to handle %{...} correctly */
            if (toklen < 2) {
                type = '%';     /* % operator */
                if (unlikely(*line == '{')) {
                    nasm_warn(WARN_PP_EMPTY_BRACES,
                              "empty %%{} construct expands to the %% operator");
                }
            } else {
                char c0 = line[1];

                switch (c0) {
                case '+':
                    type = (toklen == 2) ? TOKEN_PASTE : TOKEN_MMACRO_PARAM;
                    break;

                case '-':
                    type = TOKEN_MMACRO_PARAM;
                    break;

                case '?':
                    if (toklen == 2)
                        type = TOKEN_PREPROC_Q;
                    else if (toklen == 3 && line[2] == '?')
                        type = TOKEN_PREPROC_QQ;
                    else
                        type = TOKEN_PREPROC_ID;
                    break;

                case '*':
                    type = TOKEN_OTHER;
                    if (line[2] == '?') {
                        if (toklen == 3)
                            type = TOKEN_PREPROC_SQ;
                        else if (toklen == 4 && line[3] == '?')
                            type = TOKEN_PREPROC_SQQ;
                    }
                    break;

                case '!':
                    type = (toklen == 2) ? TOKEN_OTHER : TOKEN_ENVIRON;
                    break;

                case '%':
                    type = (toklen == 2) ? TOKEN_SMOD : TOKEN_LOCAL_SYMBOL;
                    break;

                case '$':
                    type = (toklen == 2) ? TOKEN_OTHER : TOKEN_LOCAL_MACRO;
                    break;

                case '[':
                    line += 2;  /* Skip %[ */
                    firstchar = *line; /* Don't clobber */
                    toklen -= 2;
                    type = TOKEN_INDIRECT;
                    break;

                case ',':
                    type = (toklen == 2) ? TOKEN_COND_COMMA : TOKEN_PREPROC_ID;
                    break;

                case '\'':
                case '\"':
                case '`':
                    /* %{'string'} */
                    type = TOKEN_PREPROC_ID;
                    break;

                case ':':
                    type = TOKEN_MMACRO_PARAM; /* %{:..} */
                    break;

                default:
                    if (nasm_isdigit(c0))
                        type = TOKEN_MMACRO_PARAM;
                    else if (nasm_isidchar(c0) || toklen > 2)
                        type = TOKEN_PREPROC_ID;
                    else
                        type = TOKEN_OTHER;
                    break;
                }
            }
        } else if (*p == '?' && !nasm_isidchar(p[1])) {
            /* ? operator */
            type = TOKEN_QMARK;
            p++;
        } else if (nasm_isidstart(*p) ||
                   (*p == '$' && nasm_isidchar(p[1]) &&
                    (p[1] != '$' || nasm_isidchar(p[2])) &&
                    (!globl.dollarhex || !nasm_isdigit(p[1])))) {
            /*
             * A regular identifier. This includes keywords which are not
             * special to the preprocessor.
             */
            type = TOKEN_ID;
            while (nasm_isidchar(*++p))
                ;
         } else if (nasm_isquote(*p)) {
            /*
             * A string token.
             */
            char quote = *p;

            type = TOKEN_STR;
            p = nasm_skip_string(p);

            if (*p) {
                p++;
            } else {
                nasm_warn(WARN_PP_OPEN_STRING,
                          "unterminated string (missing `%c')", quote);
                type = TOKEN_ERRSTR;
            }
        } else if (p[0] == '$' && p[1] == '$') {
            type = TOKEN_BASE;
            p += 2;
        } else if (nasm_isnumstart(*p)) {
            bool is_hex = false;
            bool is_float = false;
            bool has_e = false;
            char c;

            /*
             * A numeric token.
             */

            if (*p == '$') {
                p++;
                is_hex = true;
            }

            for (;;) {
                c = *p++;

                if (!is_hex && (c == 'e' || c == 'E')) {
                    has_e = true;
                    if (*p == '+' || *p == '-') {
                        /*
                         * e can only be followed by +/- if it is either a
                         * prefixed hex number or a floating-point number
                         */
                        p++;
                        is_float = true;
                    }
                } else if (c == 'H' || c == 'h' || c == 'X' || c == 'x') {
                    is_hex = true;
                } else if (c == 'P' || c == 'p') {
                    is_float = true;
                    if (*p == '+' || *p == '-')
                        p++;
                } else if (nasm_isnumchar(c))
                    ; /* just advance */
                else if (c == '.') {
                    /*
                     * we need to deal with consequences of the legacy
                     * parser, like "1.nolist" being two tokens
                     * (TOKEN_NUM, TOKEN_ID) here; at least give it
                     * a shot for now.  In the future, we probably need
                     * a flex-based scanner with proper pattern matching
                     * to do it as well as it can be done.  Nothing in
                     * the world is going to help the person who wants
                     * 0x123.p16 interpreted as two tokens, though.
                     */
                    const char *r = p;
                    while (*r == '_')
                        r++;

                    if (nasm_isdigit(*r) || (is_hex && nasm_isxdigit(*r)) ||
                        (!is_hex && (*r == 'e' || *r == 'E')) ||
                        (*r == 'p' || *r == 'P')) {
                        p = r;
                        is_float = true;
                    } else
                        break;  /* Terminate the token */
                } else
                    break;
            }
            p--;        /* Point to first character beyond number */

            if (p == line+1 && *line == '$') {
                type = TOKEN_HERE;
            } else {
                if (has_e && !is_hex) {
                    /* 1e13 is floating-point, but 1e13h is not */
                    is_float = true;
                }

                type = is_float ? TOKEN_FLOAT : TOKEN_NUM;
            }
        } else if (nasm_isspace(*p)) {
            firstchar = ' ';    /* Always a single space */
            type = TOKEN_WHITESPACE;
            p = nasm_skip_spaces(p);
            /*
             * Whitespace just before end-of-line is discarded by
             * pretending it's a comment; whitespace just before a
             * comment gets lumped into the comment.
             */
            if (!*p || *p == ';')
                type = TOKEN_EOS;
        } else if (*p == ';') {
            type = TOKEN_EOS;
        } else {
            /*
             * Anything else is an operator of some kind. We check
             * for all the double-character operators (>>, <<, //,
             * %%, <=, >=, ==, !=, <>, &&, ||, ^^) and the triple-
	     * character operators (<<<, >>>, <=>) but anything
             * else is a single-character operator.
             */
            type = (unsigned char)*p;
	    switch (*p++) {
	    case '>':
		if (*p == '>') {
		    p++;
                    type = TOKEN_SHR;
		    if (*p == '>') {
                        type = TOKEN_SAR;
			p++;
                    }
		} else if (*p == '=') {
                    type = TOKEN_GE;
                    p++;
                }
		break;

	    case '<':
		if (*p == '<') {
		    p++;
                    type = TOKEN_SHL;
		    if (*p == '<')
			p++;
		} else if (*p == '=') {
		    p++;
                    type = TOKEN_LE;
		    if (*p == '>') {
			p++;
                        type = TOKEN_LEG;
                    }
		} else if (*p == '>') {
		    p++;
                    type = TOKEN_NE;
		}
		break;

	    case '!':
		if (*p == '=') {
		    p++;
                    type = TOKEN_NE;
                }
		break;

	    case '/':
                if (*p == '/') {
                    p++;
                    type = TOKEN_SDIV;
                }
                break;
	    case '=':
                if (*p == '=')
                    p++;        /* Still TOKEN_EQ == '=' though */
                break;
	    case '&':
                if (*p == '&') {
                    p++;
                    type = TOKEN_DBL_AND;
                }
                break;

	    case '|':
                if (*p == '|') {
                    p++;
                    type = TOKEN_DBL_OR;
                }
                break;

	    case '^':
                if (*p == '^') {
                    p++;
                    type = TOKEN_DBL_XOR;
                }
		break;

	    default:
		break;
	    }
        }

        if (type == TOKEN_EOS)
            break;              /* done with the string... */

        if (!ep)
            ep = p;
        toklen = ep - line;

        if (toklen) {
            *tail = t = new_Token(NULL, type, line, toklen);
            *tok_text_buf(t) = firstchar; /* E.g. %{foo} -> {foo -> %foo */
            tail = &t->next;
        }

        line = p;
    }
    return list;
}

/*
 * Tokens are allocated in blocks to improve speed. Set the blocksize
 * to 0 to use regular nasm_malloc(); this is useful for debugging.
 *
 * alloc_Token() returns a zero-initialized token structure.
 */
#define TOKEN_BLOCKSIZE 0 /* 4096 */ /* Number of tokens, not bytes */

#if TOKEN_BLOCKSIZE

static Token *freeTokens  = NULL;
static Token *tokenblocks = NULL;

static Token *alloc_Token(void)
{
    Token *t = freeTokens;

    if (unlikely(!t)) {
        Token *block;
        size_t i;

        nasm_newn(block, TOKEN_BLOCKSIZE);

        /*
         * The first entry in each array are a linked list of
         * block allocations and is not used for data.
         */
        block[0].next = tokenblocks;
	block[0].type = TOKEN_BLOCK;
        tokenblocks = block;

        /*
         * Add the rest to the free list
         */
        for (i = 2; i < TOKEN_BLOCKSIZE - 1; i++)
            block[i].next = &block[i+1];

        freeTokens = &block[2];

        /*
         * Return the topmost usable token
         */
        return &block[1];
    }

    freeTokens = t->next;
    t->next = NULL;
    return t;
}

static Token *free_Token(Token *t)
{
    Token *next;

    nasm_assert(t);
    nasm_assert(t->type != TOKEN_FREE);

    next = t->next;
    if (t->len > INLINE_TEXT)
        nasm_free(t->text.p.ptr);

    nasm_zero(*t);
    t->type = TOKEN_FREE;
    t->next = freeTokens;
    freeTokens = t;

    return next;
}

static void free_Blocks(void)
{
    Token *block, *blocktmp;

    list_for_each_safe(block, blocktmp, tokenblocks)
        nasm_free(block);

    freeTokens = tokenblocks = NULL;
}

#else

static inline Token *alloc_Token(void)
{
    Token *t;
    nasm_new(t);
    return t;
}

static Token *free_Token(Token *t)
{
    Token *next = t->next;
    if (t->len > INLINE_TEXT)
        nasm_free(t->text.p.ptr);
    nasm_free(t);
    return next;
}

static inline void free_Blocks(void)
{
    /* Nothing to do */
}

#endif

static Token *do_delete_Token(Token **tp)
{
    if (tp && *tp)
        return *tp = free_Token(*tp);
    else
        return NULL;
}

/*
 *  this function creates a new Token and passes a pointer to it
 *  back to the caller.  It sets the type, text, and next pointer elements.
 */
static Token *new_Token(Token * next, enum token_type type,
                        const char *text, size_t txtlen)
{
    Token *t = alloc_Token();
    char *textp;

    t->next = next;
    t->type = type;
    if (type == TOKEN_WHITESPACE) {
        t->len = 1;
        t->text.a[0] = ' ';
    } else {
        if (text && text[0] && !txtlen)
            txtlen = tok_strlen(text);

        t->len = tok_check_len(txtlen);

        if (text) {
            textp = (txtlen > INLINE_TEXT)
                ? (t->text.p.ptr = nasm_malloc(txtlen+1)) : t->text.a;
            memcpy(textp, text, txtlen);
            textp[txtlen] = '\0';   /* In case we needed malloc() */
        } else {
            /*
             * Allocate a buffer but do not fill it. The caller
             * can fill in text, but must not change the length.
             * The filled in text must be exactly txtlen once
             * the buffer is filled and before the token is added
             * to any line lists.
             */
            if (txtlen > INLINE_TEXT)
                t->text.p.ptr = nasm_zalloc(txtlen+1);
        }
    }
    return t;
}

/*
 * Same as new_Token(), but text belongs to the new token and is
 * either taken over or freed.  This function MUST be called
 * with valid txt and txtlen, unlike new_Token().
 */
static Token *new_Token_free(Token * next, enum token_type type,
                             char *text, size_t txtlen)
{
    Token *t = alloc_Token();

    t->next = next;
    t->type = type;
    t->len = tok_check_len(txtlen);

    if (txtlen <= INLINE_TEXT) {
        memcpy(t->text.a, text, txtlen);
        nasm_free(text);
    } else {
        t->text.p.ptr = text;
    }

    return t;
}

static Token *dup_Token(Token *next, const Token *src)
{
    Token *t;

    if (unlikely(!src))
        return NULL;

    t = alloc_Token();

    memcpy(t, src, sizeof *src);
    t->next = next;

    if (t->len > INLINE_TEXT) {
        t->text.p.ptr = nasm_malloc(t->len + 1);
        memcpy(t->text.p.ptr, src->text.p.ptr, t->len+1);
    }

    return t;
}

static Token *new_White(Token *next)
{
    Token *t = alloc_Token();

    t->next = next;
    t->type = TOKEN_WHITESPACE;
    t->len  = 1;
    t->text.a[0] = ' ';

    return t;
}

/*
 * This *transfers* the content from one token to another, leaving the
 * next pointer of the latter intact. Unlike dup_Token(), the old
 * token is destroyed, except for its next pointer, and the text
 * pointer allocation, if any, is simply transferred.
 */
static Token *steal_Token(Token *dst, Token *src)
{
    /* Delete any previous text string allocation */
    if (unlikely(dst->len > INLINE_TEXT))
        nasm_free(dst->text.p.ptr);

    /* Overwrite everything except the next pointers */
    memcpy((char *)dst + sizeof(Token *), (char *)src + sizeof(Token *),
	   sizeof(Token) - sizeof(Token *));

    /* Clear the donor token */
    memset((char *)src + sizeof(Token *), 0, sizeof(Token) - sizeof(Token *));

    return dst;
}

/*
 * Convert a line of tokens back into text. This modifies the list
 * by expanding environment variables.
 *
 * If expand_locals is not zero, identifiers of the form "%$*xxx"
 * are also transformed into ..@ctxnum.xxx
 */
static char *detoken(Token * tlist, bool expand_locals)
{
    Token *t;
    char *line, *p;
    int len = 0;

    list_for_each(t, tlist) {
	switch (t->type) {
	case TOKEN_ENVIRON:
	{
	    const char *v = pp_getenv(t, true);
	    set_text(t, v, tok_strlen(v));
	    t->type = TOKEN_NAKED_STR;
	    break;
        }

	case TOKEN_LOCAL_MACRO:
        case TOKEN_LOCAL_SYMBOL:
	    if (expand_locals) {
		const char *q;
		char *p;
		Context *ctx = get_ctx(tok_text(t), &q);
		if (ctx) {
		    p = nasm_asprintf("..@%"PRIu64".%s", ctx->number, q);
		    set_text_free(t, p, nasm_last_string_len());
		    t->type = TOKEN_ID;
		}
	    }
	    break;

        case TOKEN_INDIRECT:
            /*
             * This won't happen in when emitting to the assembler,
             * but can happen when emitting output for some of the
             * list options. The token string doesn't actually include
             * the brackets in this case.
             */
            len += 3;           /* %[] */
            break;

        case TOKEN_FREE:
            panic();

	default:
	    break;		/* No modifications */
        }

        if (debug_level(2)) {
            unsigned int t_len  = t->len;
            unsigned int s_len = tok_strlen(tok_text(t));
            if (t_len != s_len) {
                nasm_panic("assertion failed: token \"%s\" type %u len %u has t->len %u\n",
                           tok_text(t), t->type, s_len, t_len);
                t->len = s_len;
            }
        }

	len += t->len;
    }

    p = line = nasm_malloc(len + 1);

    list_for_each(t, tlist) {
        switch (t->type) {
        case TOKEN_INDIRECT:
            *p++ = '%';
            *p++ = '[';
            p = mempcpy(p, tok_text(t), t->len);
            *p++ = ']';
            break;

        default:
            p = mempcpy(p, tok_text(t), t->len);
        }
    }
    *p = '\0';

    return line;
}

/*
 * A scanner, suitable for use by the expression evaluator, which
 * operates on a line of Tokens. Expects a pointer to a pointer to
 * the first token in the line to be passed in as its private_data
 * field.
 *
 * FIX: This really needs to be unified with stdscan.
 */
struct ppscan {
    Token *tptr;
    int ntokens;
};

static int ppscan(void *private_data, struct tokenval *tokval)
{
    struct ppscan *pps = private_data;
    Token *tline;
    const char *txt;

    do {
	if (pps->ntokens && (tline = pps->tptr)) {
	    pps->ntokens--;
	    pps->tptr = tline->next;
	} else {
	    pps->tptr = NULL;
	    pps->ntokens = 0;
	    return tokval->t_type = TOKEN_EOS;
	}
    } while (tline->type == TOKEN_WHITESPACE);

    txt = tok_text(tline);
    tokval->t_start = txt;
    tokval->t_len = tline->len;
    tokval->t_charptr = (char *)txt; /* Fix needing const removal here */

    switch (tline->type) {
    default:
        break;

    case TOKEN_ID:
        if (txt[0] == '$') {
            /* Escaped symbol */
            tokval->t_charptr++;
        } else {
            /* This could be an assembler keyword */
            return nasm_token_hash(txt, tokval);
        }
        break;

    case TOKEN_NUM:
    {
        bool rn_error;
        if (*txt == '$')
            warn_dollar_hex();
        tokval->t_integer = readnum(txt, &rn_error);
        if (rn_error)
            return tokval->t_type = TOKEN_ERRNUM;
        else
            return tokval->t_type = TOKEN_NUM;
    }

    case TOKEN_STR:
	tokval->t_charptr = (char *)unquote_token(tline);
        /* fall through */
    case TOKEN_INTERNAL_STR:
    case TOKEN_NAKED_STR:
        tokval->t_inttwo = tline->len;
	return tokval->t_type = TOKEN_STR;
    }

    return tokval->t_type = tline->type;
}

/*
 * 1. An expression (true if nonzero 0)
 * 2. The keywords true, on, yes for true
 * 3. The keywords false, off, no for false
 * 4. An empty line, for true
 *
 * On error, return defval (usually the previous value)
 */
static bool pp_get_boolean_option(Token *tline, bool defval)
{
    static const char * const noyes[] = {
        "no", "yes",
        "false", "true",
        "off", "on"
    };
    struct ppscan pps;
    struct tokenval tokval;
    expr *evalresult;

    tline = skip_white(tline);
    if (!tline)
        return true;

    if (tline->type == TOKEN_ID) {
        size_t i;
	const char *txt = tok_text(tline);

        for (i = 0; i < ARRAY_SIZE(noyes); i++)
            if (!nasm_stricmp(txt, noyes[i]))
                return i & 1;
    }

    pps.tptr = NULL;
    pps.tptr = tline;
    pps.ntokens = -1;
    tokval.t_type = TOKEN_INVALID;
    evalresult = evaluate(ppscan, &pps, &tokval, NULL, true, NULL);

    if (!evalresult)
        return true;

    if (tokval.t_type) {
        nasm_warn(WARN_PP_TRAILING,
                  "trailing garbage after expression ignored");
    }
    if (!is_really_simple(evalresult)) {
        nasm_nonfatal("boolean flag expression must be a constant");
        return defval;
    }

    return reloc_value(evalresult) != 0;
}

/*
 * Compare a string to the name of an existing macro; this is a
 * simple wrapper which calls either strcmp or nasm_stricmp
 * depending on the value of the `casesense' parameter.
 */
static int mstrcmp(const char *p, const char *q, bool casesense)
{
    return casesense ? strcmp(p, q) : nasm_stricmp(p, q);
}

/*
 * Compare a string to the name of an existing macro; this is a
 * simple wrapper which calls either strcmp or nasm_stricmp
 * depending on the value of the `casesense' parameter.
 */
static int mmemcmp(const char *p, const char *q, size_t l, bool casesense)
{
    return casesense ? memcmp(p, q, l) : nasm_memicmp(p, q, l);
}

/*
 * Return the Context structure associated with a %$ token. Return
 * NULL, having _already_ reported an error condition, if the
 * context stack isn't deep enough for the supplied number of $
 * signs.
 *
 * If "namep" is non-NULL, set it to the pointer to the macro name
 * tail, i.e. the part beyond %$...
 */
static Context *get_ctx(const char *name, const char **namep)
{
    Context *ctx;
    int i;

    if (namep)
        *namep = name;

    if (!name || name[0] != '%' || name[1] != '$')
        return NULL;

    if (!cstk) {
        nasm_nonfatal("`%s': context stack is empty", name);
        return NULL;
    }

    name += 2;
    ctx = cstk;
    i = 0;
    while (ctx && *name == '$') {
        name++;
        i++;
        ctx = ctx->next;
    }
    if (!ctx) {
        nasm_nonfatal("`%s': context stack is only"
                      " %d level%s deep", name, i, (i == 1 ? "" : "s"));
        return NULL;
    }

    if (namep)
        *namep = name;

    return ctx;
}

/*
 * Open an include file. This routine must always return a valid
 * file pointer if it returns - it's responsible for throwing an
 * ERR_FATAL and bombing out completely if not. It should also try
 * the include path one by one until it finds the file or reaches
 * the end of the path.
 *
 * Note: for INC_PROBE the function returns NULL at all times;
 * instead look for a filename in *slpath.
 */
enum incopen_mode {
    INC_OPTIONAL      = 0,
    INC_NEEDED        = 1,      /* File must exist */
    INC_REQUIRED      = 2,      /* File must exist, but only open once/pass */
    INC_PROBE         = 4,      /* Existence probe (don't open the file) */
    INC_EXACT         = 8       /* Exact filename match only (no path search) */
};

/* This is conducts a full pathname search */
static FILE *inc_fopen_search(const char *file,
                              char **slpath,
                              enum incopen_mode *omp,
                              enum file_flags fmode)
{
    const struct strlist_entry *ip;
    FILE *fp;
    const char *prefix = "";
    char *sp;
    bool found;
    enum incopen_mode omode = *omp;

    ip = omode & INC_EXACT ? NULL : strlist_head(ipath_list);

    while (1) {
        sp = nasm_catfile(prefix, file);
        if (omode & INC_PROBE) {
            fp = NULL;
            found = nasm_file_exists(sp);
        } else {
            fp = nasm_open_read(sp, fmode);
            found = (fp != NULL);
        }
        if (found) {
            *slpath = sp;
            if (!prefix[0])
                *omp |= INC_EXACT;
            return fp;
        }

        nasm_free(sp);

        if (!ip) {
            *slpath = NULL;
            return NULL;
        }

        prefix = ip->str;
        ip = ip->next;
    }
}

/*
 * Open a file, or test for the presence of one (depending on omode),
 * considering the include path.
 */
struct file_hash_entry {
    const char *path;
    struct file_hash_entry *full; /* Hash entry for the full path */
    int64_t include_pass;	  /* Pass in which last included (for %require) */
    enum incopen_mode omode;      /* Flags */
};

static FILE *inc_fopen(const char *file,
                       struct strlist *dhead,
                       const struct file_hash_entry **found_fhe,
                       enum incopen_mode omode,
                       enum file_flags fmode)
{
    struct file_hash_entry **fhep;
    struct file_hash_entry *fhe = NULL;
    struct hash_insert hi;
    const char *path = NULL;
    FILE *fp = NULL;
    const int64_t pass = pass_count();
    bool skip_open = !!(omode & INC_PROBE);

    fhep = (struct file_hash_entry **)hash_find(&FileHash, file, &hi);
    if (fhep) {
        fhe = *fhep;
        path = fhe->path;
        if ((omode ^ fhe->omode) & INC_EXACT) {
            if (omode & INC_EXACT)
                path = NULL;    /* Entry found, but it is non-exact */
            else if (!path)
                fhe = NULL;     /* No exact entry found, but maybe searchable */
        }
    }

    if (!fhe) {
        /* Need to do the actual path search */
        char *pptr;
        fp = inc_fopen_search(file, &pptr, &omode, fmode);
        path = pptr;

        /* Positive or negative result */
        nasm_new(fhe);
        fhe->path  = path;
        fhe->full  = fhe;    /* It is *possible*... */
        fhe->omode = omode & INC_EXACT;

        /*
         * Don't cache a negative result if INC_EXACT is specified
         * (used by %iffile).  In the future consider making it
         * possible to distinguish, but for now don't worry about
         * it...
         */
        if (fhep) {
            nasm_free(*fhep);
            *fhep = fhe;
        } else {
            hash_add(&hi, nasm_strdup(file), fhe);
        }

        /*
         * Add a hash entry for the canonical path if there isn't one
         * already. Try to get the unique name from the OS best we can.
         * Note that ->path and ->full->path can be different, and that
         * is okay (we don't want to print out a full canonical path
         * in messages, for example.)
         */
        if (path) {
            char *fullpath = nasm_realpath(path);

            if (!strcmp(file, fullpath)) {
                nasm_free(fullpath);
            } else {
                struct file_hash_entry **fullp, *full;
                fullp = (struct file_hash_entry **)
                    hash_find(&FileHash, fullpath, &hi);

                if (fullp) {
                    full = *fullp;
                    nasm_free(fullpath);
                } else {
                    nasm_new(full);
                    full->path  = fullpath;
                    full->full  = full;
                    full->omode = INC_EXACT;
                    hash_add(&hi, full->path, full);
                }
                fhe->full = full;
            }
        }
    }

    if (dhead) {
        /*
         * This file could have previously probed for but never added;
         * in that case it may be necessary to try to re-add it here.
         *
         * This could be fixed by merging the file hash and dependency
         * array at some point...
         */
        strlist_add(dhead, path ? path : file);
    }

    if (path) {
        skip_open |=
            ((omode | fhe->full->omode) & INC_REQUIRED) &&
            (fhe->full->include_pass >= pass);

        if (!skip_open) {
            fp = nasm_open_read(path, fmode);

            if (fp)
                fhe->full->include_pass = pass;
        }
    }

    if (!fp && !skip_open && (omode & INC_NEEDED)) {
        if (!path)
            errno = ENOENT;

        nasm_nonfatal("unable to open include file `%s': %s",
                      file, strerror(errno));
    }

    if (found_fhe)
        *found_fhe = path ? fhe : NULL;

    return fp;
}

/*
 * Opens an include or input file. Public version, for use by modules
 * that get a file:lineno pair and need to look at the file again
 * (e.g. the CodeView debug backend). Returns NULL on failure.
 */
FILE *pp_input_fopen(const char *filename, enum file_flags mode)
{
    return inc_fopen(filename, NULL, NULL, INC_OPTIONAL, mode);
}

/*
 * Expand a token list that is expected to contain a filename string.
 * Returns a new token containing a TOK_INTERNAL_STR with the given
 * filename, or NULL on error.  If the argument "*otp" is set, set
 * that to point to the actual quoted string token.
 */
static Token *tlist_filename(Token **tp, Token **otp, const char *dname)
{
    Token *t;

    *tp = t = expand_smacro_noreset(*tp);

    t = skip_white(t);
    if (!tok_string(t)) {
        if (otp)
            *otp = NULL;
        nasm_nonfatal("`%s' expects a file name", dname);
        return NULL;
    }

    if (skip_white(t->next)) {
        nasm_warn(WARN_PP_TRAILING,
                  "trailing garbage after `%s' ignored", dname);
    }

    if (otp)
        *otp = t;

    t = dup_Token(NULL, t);
    unquote_token_cstr(t);
    return t;
}

/*
 * This implements the %pathsearch directive and %pathsearch() function.
 * Returns a new token.
 */
static Token *pp_do_pathsearch(Token **tp, const char *dname)
{
    const struct file_hash_entry *fhe;
    Token *t, *ot;

    t = tlist_filename(tp, &ot, dname);
    if (!t)
        return NULL;

    inc_fopen(tok_text(t), NULL, &fhe, INC_PROBE, NF_BINARY);
    if (fhe) {
        delete_Token(t);
        return make_tok_qstr(NULL, fhe->path);
    } else {
        return steal_Token(t, ot);
    }
}

/*
 * This implements the %depend directive and the %depend() function.
 * It returns a stolen copy of the original string token after skipping
 * leading spaces, or NULL on error.
 * Returns a new token.
 */
static Token *pp_do_depend(Token **tp, const char *dname)
{
    Token *t, *ot;

    t = tlist_filename(tp, &ot, dname);
    if (!t)
        return NULL;

    strlist_add(deplist, tok_text(t));
    return steal_Token(t, ot);
}

/*
 * Determine if we should warn on defining a single-line macro of
 * name `name', with `nparam' parameters. If nparam is 0 or -1, will
 * return true if _any_ single-line macro of that name is defined.
 * Otherwise, will return true if a single-line macro with either
 * `nparam' or no parameters is defined.
 *
 * If a macro with precisely the right number of parameters is
 * defined, or nparam is -1, the address of the definition structure
 * will be returned in `defn'; otherwise NULL will be returned. If `defn'
 * is NULL, no action will be taken regarding its contents, and no
 * error will occur.
 *
 * Note that this is also called with nparam zero to resolve
 * `ifdef'.
 */
static bool
smacro_defined(Context *ctx, const char *name, int nparam, SMacro **defn,
               bool nocase, bool find_alias)
{
    struct hash_table *smtbl;
    SMacro *m;

    smtbl = ctx ? &ctx->localmac : &smacros;

restart:
    m = (SMacro *) hash_findix(smtbl, name);

    while (m) {
        if (!mstrcmp(m->name, name, m->casesense && nocase) &&
            (nparam <= 0 || m->nparam == 0 ||
             (nparam >= m->nparam_min &&
              (m->varadic || nparam <= m->nparam)))) {
            if (m->alias && !find_alias) {
                if (!ppconf.noaliases) {
                    name = tok_text(m->expansion);
                    goto restart;
                } else {
                    continue;
                }
            }
            if (defn)
                *defn = m;
            return true;
        }
        m = m->next;
    }

    return false;
}

static int read_param_count(const char *str)
{
    int64_t result;
    bool err;

    result = readnum(str, &err);
    if (err || result < 0) {
        nasm_nonfatal("unable to parse parameter count `%s'", str);
        return 0;
    } else if (result > nasm_limit[LIMIT_PARAMS]) {
        nasm_nonfatal("parameter count `%s' is too large (max %"PRId64")",
                      str, nasm_limit[LIMIT_PARAMS]);
        return 0;
    }
    return result;
}

/*
 * Count and mark off the parameters in a multi-line macro call.
 * This is called both from within the multi-line macro expansion
 * code, and also to mark off the default parameters when provided
 * in a %macro definition line.
 *
 * Note that we need space in the params array for parameter 0 being
 * a possible captured label as well as the final NULL.
 *
 * Returns a pointer to the pointer to a terminal comma if present;
 * used to drop an empty terminal argument for legacy reasons.
 */
static Token **count_mmac_params(Token *tline, int *nparamp, Token ***paramsp)
{
    int paramsize;
    int nparam = 0;
    Token *t;
    Token **comma = NULL, **maybe_comma = NULL;
    Token **params;

    paramsize = PARAM_DELTA;
    nasm_newn(params, paramsize);

    t = skip_white(tline);
    if (t) {
        while (true) {
            /* Need two slots for captured label and NULL */
            if (unlikely(nparam+2 >= paramsize)) {
                paramsize += PARAM_DELTA;
                params = nasm_realloc(params, sizeof(*params) * paramsize);
            }
            params[++nparam] = t;
            if (tok_is(t, '{')) {
                int brace = 1;

                comma = NULL;   /* Non-empty parameter */

                while (brace && (t = t->next)) {
                    brace += tok_is(t, '{');
                    brace -= tok_is(t, '}');
                }

                if (t) {
                    /*
                     * Now we've found the closing brace, look further
                     * for the comma.
                     */
                    t = skip_white(t->next);
                    if (tok_isnt(t, ','))
                        nasm_nonfatal("braces do not enclose all of macro parameter");
                } else {
                    nasm_nonfatal("expecting closing brace in macro parameter");
                }
            }

            if (!t)
                break;              /* End of string, no comma */

            /* Advance to the next comma */
            maybe_comma = &t->next;
            while (tok_isnt(t, ',')) {
                if (!tok_white(t))
                    comma = NULL; /* Non-empty parameter */
                maybe_comma = &t->next;
                t = t->next;
            }

            if (!t)
                break;              /* End of string, no comma */

            comma = maybe_comma;     /* Point to comma pointer */
            t = skip_white(t->next); /* Eat the comma and whitespace */
        }
    }

    params[nparam+1] = NULL;
    *paramsp = params;
    *nparamp = nparam;

    return comma;
}

/* Check to see if a string is a valid preprocessor directive */

/* This requires that the caller has checked dname[0] == '%' */
static inline enum preproc_token pp_get_nasm_directive(const char *dname)
{
    /*
     * For it to be a directive, the second character has to be an
     * ASCII letter; this is a very quick and dirty test for that;
     * all other cases will get rejected by the token hash.
     */
    if (likely((uint8_t)((dname[1] & ~0x20) - 'A') <= 'Z'))
        return pp_token_hash(dname);

    return PP_invalid;
}

static inline enum preproc_token pp_get_tasm_directive(const char *dname)
{
    if (likely(!(ppopt & PP_TASM)))
        return PP_invalid;

    /*
     * Directive in TASM mode. Again, must begin with a letter.
     */
    if ((uint8_t)((dname[0] & ~0x20) - 'A') <= 'Z')
        return pp_tasm_token_hash(dname);

    return PP_invalid;
}

static enum preproc_token pp_get_directive(const char *dname)
{
    if (dname[0] == '%')
        return pp_get_nasm_directive(dname);
    else
        return pp_get_tasm_directive(dname);
}

static bool is_directive(const char *dname)
{
    char *p;
    const char *q;
    bool j;

    dname = nasm_skip_spaces(dname);

    if (*dname == '[') {
        dname = nasm_skip_spaces(dname+1);
    } else {
        if (*dname == '%')
            return pp_get_nasm_directive(dname) != PP_invalid;

        if (pp_get_tasm_directive(dname) != PP_invalid)
            return true;
    }

    q = nasm_skip_word(dname);
    p = nasm_strndup(dname, q-dname);
    j = directive_valid(p);
    nasm_free(p);
    return j;
}

/*
 * Determine whether one of the various `if' conditions is true or
 * not.
 *
 * We must free the tline we get passed.
 */
static enum cond_state
if_condition(Token * tline, enum preproc_token ct, const char *dname)
{
    bool j;
    Token *t, *tt, *origline;
    struct ppscan pps;
    struct tokenval tokval;
    expr *evalresult;
    enum token_type needtype;
    bool casesense = true;
    enum preproc_token cond = PP_COND(ct);

    origline = tline;

    switch (cond) {
    case PP_IFCTX:
        j = false;              /* have we matched yet? */
        while (true) {
            tline = skip_white(tline);
            if (!tline)
                break;
            if (tline->type != TOKEN_ID) {
                nasm_nonfatal("`%s' expects context identifiers",
                              dname);
                goto fail;
            }
            if (cstk && cstk->name && !nasm_stricmp(tok_text(tline), cstk->name))
                j = true;
            tline = tline->next;
        }
        break;

    case PP_IFDEF:
    case PP_IFDEFALIAS:
    {
        bool alias = cond == PP_IFDEFALIAS;
        SMacro *smac;
        Context *ctx;
        const char *mname;

        j = false;              /* have we matched yet? */
        while (tline) {
            tline = skip_white(tline);
            if (!tok_macro_or_func_id(tline)) {
                nasm_nonfatal("`%s' expects macro identifiers",
                              dname);
                goto fail;
            }

            mname = tok_text(tline);
            ctx = get_ctx(mname, &mname);
            if (smacro_defined(ctx, mname, -1, &smac, true, alias) && smac
                && smac->alias == alias) {
                j = true;
                break;
            }
            tline = tline->next;
        }
        break;
    }

    case PP_IFDIFI:
        /*
         * %ifdifi doesn't actually exist; it ignores its argument and is
         * always false. This exists solely to stub out the corresponding
         * TASM directive.
         */
        j = false;
        goto fail;

    case PP_IFDIRECTIVE:
        tline = skip_white(expand_smacro(tline));
        j = false;
        if (tline)
            j = is_directive(unquote_token(tline));
        break;

    case PP_IFENV:
        tline = expand_smacro(tline);
        j = false;              /* have we matched yet? */
        while (tline) {
            tline = skip_white(tline);
            if (!tline || (tline->type != TOKEN_ID &&
                           tline->type != TOKEN_STR &&
			   tline->type != TOKEN_INTERNAL_STR &&
                           tline->type != TOKEN_ENVIRON)) {
                nasm_nonfatal("`%s' expects environment variable names",
                              dname);
                goto fail;
            }

	    j |= !!pp_getenv(tline, false);
            tline = tline->next;
	}
	break;

    case PP_IFFILE:
    {
        const struct file_hash_entry *fhe;

        t = tlist_filename(&origline, NULL, dname);
        if (!t)
            goto fail;

        inc_fopen(tok_text(t), NULL, &fhe, INC_PROBE|INC_EXACT, NF_BINARY);
        j = fhe && (fhe->omode & INC_EXACT);
        delete_Token(t);
        break;
    }

    case PP_IFIDNI:
        casesense = false;
        /* fall through */
    case PP_IFIDN:
        tline = expand_smacro(tline);
        t = tt = tline;
        while (tok_isnt(tt, ','))
            tt = tt->next;
        if (!tt) {
            nasm_nonfatal("`%s' expects two comma-separated arguments",
                          dname);
            goto fail;
        }
        tt = tt->next;
        j = true;               /* assume equality unless proved not */
        while (tok_isnt(t, ',') && tt) {
	    unsigned int l1, l2;
	    const char *t1, *t2;

            if (tok_is(tt, ',')) {
                nasm_nonfatal("`%s': more than one comma on line",
                              dname);
                goto fail;
            }
            if (t->type == TOKEN_WHITESPACE) {
                t = t->next;
                continue;
            }
            if (tt->type == TOKEN_WHITESPACE) {
                tt = tt->next;
                continue;
            }
            if (tt->type != t->type) {
                j = false;      /* found mismatching tokens */
                break;
            }

	    t1 = unquote_token(t);
	    t2 = unquote_token(tt);
	    l1 = t->len;
	    l2 = tt->len;

	    if (l1 != l2 || mmemcmp(t1, t2, l1, casesense)) {
		j = false;
		break;
	    }

            t = t->next;
            tt = tt->next;
        }
        if (!tok_is(t, ',') || tt)
            j = false;          /* trailing gunk on one end or other */
        break;

    case PP_IFMACRO:
    {
        bool found = false;
        MMacro searching, *mmac;

        tline = skip_white(tline);
        tline = expand_id(tline);
        if (!tok_is(tline, TOKEN_ID)) {
            nasm_nonfatal("`%s' expects a macro name", dname);
            goto fail;
        }
        nasm_zero(searching);
        searching.name = dup_text(tline);
        searching.casesense = true;
        searching.nparam_min = 0;
        searching.nparam_max = INT_MAX;
        tline = expand_smacro(tline->next);
        tline = skip_white(tline);
        if (!tline) {
        } else if (!tok_is(tline, TOKEN_NUM)) {
            nasm_nonfatal("`%s' expects a parameter count or nothing",
                          dname);
        } else {
            searching.nparam_min = searching.nparam_max =
                read_param_count(tok_text(tline));
        }
        if (tline && tok_is(tline->next, '-')) {
            tline = tline->next->next;
            if (tok_is(tline, '*'))
                searching.nparam_max = INT_MAX;
            else if (!tok_is(tline, TOKEN_NUM))
                nasm_nonfatal("`%s' expects a parameter count after `-'",
                              dname);
            else {
                searching.nparam_max = read_param_count(tok_text(tline));
                if (searching.nparam_min > searching.nparam_max) {
                    nasm_nonfatal("minimum parameter count exceeds maximum");
                    searching.nparam_max = searching.nparam_min;
                }
            }
        }
        if (tline && tok_is(tline->next, '+')) {
            tline = tline->next;
            searching.plus = true;
        }
        mmac = (MMacro *) hash_findix(&mmacros, searching.name);
        while (mmac) {
            if (!strcmp(mmac->name, searching.name) &&
                (mmac->nparam_min <= searching.nparam_max
                 || searching.plus)
                && (searching.nparam_min <= mmac->nparam_max
                    || mmac->plus)) {
                found = true;
                break;
            }
            mmac = mmac->next;
        }
        if (tline && tline->next) {
            nasm_warn(WARN_PP_TRAILING,
                      "trailing garbage after `%s' ignored", dname);
        }
        nasm_free(searching.name);
        j = found;
        break;
    }

    case PP_IFID:
        needtype = TOKEN_ID;
        goto iftype;
    case PP_IFNUM:
        needtype = TOKEN_NUM;
        goto iftype;
    case PP_IFSTR:
        needtype = TOKEN_STR;
        goto iftype;

iftype:
        t = tline = expand_smacro(tline);

        while (tok_white(t) ||
               (needtype == TOKEN_NUM && (tok_is(t, '-') || tok_is(t, '+'))))
            t = t->next;

        j = tok_is(t, needtype);
        break;

    case PP_IFTOKEN:
        tline = expand_smacro(tline);
        t = skip_white(tline);

        j = false;
        if (t) {
            t = skip_white(t->next); /* Skip the actual token + whitespace */
            j = !t;
        }
        break;

    case PP_IFEMPTY:
        tline = expand_smacro(tline);
        t = skip_white(tline);
        j = !t;                 /* Should be empty */
        break;

    case PP_IF:
        pps.tptr = tline = expand_smacro(tline);
	pps.ntokens = -1;
        tokval.t_type = TOKEN_INVALID;
        evalresult = evaluate(ppscan, &pps, &tokval, NULL, true, NULL);
        if (!evalresult)
            return -1;
        if (tokval.t_type) {
            nasm_warn(WARN_PP_TRAILING, "trailing garbage after expression ignored");
        }
        if (!is_simple(evalresult)) {
            nasm_nonfatal("non-constant value given to `%s'",
                          dname);
            goto fail;
        }
        j = reloc_value(evalresult) != 0;
        break;

    case PP_IFUSING:
    case PP_IFUSABLE:
    {
        const struct use_package *pkg;
        const char *name;

        pkg = get_use_pkg(tline, dname, &name);
        if (!name)
            goto fail;

        j = pkg && ((cond == PP_IFUSABLE) | use_loaded[pkg->index]);
        break;
    }

    default:
        nasm_nonfatal("unknown preprocessor directive `%s'", dname);
        goto fail;
    }

    delete_tlist(origline);
    return (j ^ PP_COND_NEGATIVE(ct)) ? COND_IF_TRUE : COND_IF_FALSE;

fail:
    delete_tlist(origline);
    return COND_NEVER;
}

/*
 * Default smacro expansion routine: just returns a copy of the
 * expansion list.
 */
static Token *
smacro_expand_default(const SMacro *s, Token **params, int nparams)
{
    (void)params;
    (void)nparams;

    return dup_tlist(s->expansion, NULL);
}

/*
 * Emit a macro definition or undef to the listing file or debug format
 * if desired. This is similar to detoken(), but it handles the
 * reverse expansion list, does not expand %! or local variable
 * tokens, and does some special handling for macro parameters.
 */
static void
list_smacro_def(enum preproc_token op, const Context *ctx, const SMacro *m)
{
    Token *t;
    size_t namelen, size;
    char *def, *p, *end_spec;
    char *context_prefix = NULL;
    size_t context_len;

    namelen = strlen(m->name);
    size = namelen + 2;  /* Include room for space after name + NUL */

    if (ctx) {
        int context_depth = cstk->depth - ctx->depth + 1;
        context_prefix =
            nasm_asprintf("[%s::%"PRIu64"] %%%-*s",
                          ctx->name ? ctx->name : "",
                          ctx->number, context_depth, "");

        context_len = nasm_last_string_len();
        memset(context_prefix + context_len - context_depth,
               '$', context_depth);
        size += context_len;
    }

    list_for_each(t, m->expansion)
        size += t->len;

    if (m->nparam) {
        /*
         * Space for "(" at the beginning, then up to 5 flags "=&&!+"
         * + "/ux" + terminal "," or ")" per parameter, plus the parameter
         * name, if any.
         */
        int i;

        size += 1 + (5+3+1) * m->nparam;
        for (i = 0; i < m->nparam; i++)
            size += m->params[i].name.len;
    }

    def = nasm_malloc(size);
    p = def+size;
    *--p = '\0';

    list_for_each(t, m->expansion) {
	p -= t->len;
	memcpy(p, tok_text(t), t->len);
    }

    *--p = ' ';
    end_spec = p;               /* Truncate here for macro def only */

    if (m->nparam) {
        int i;

        *--p = ')';

        for (i = m->nparam-1; i >= 0; i--) {
            enum sparmflags flags = m->params[i].flags;
            bool slash = false;

            if (m->params[i].radix) {
                *--p = m->params[i].radix;
                slash = true;
            }
            if (flags & SPARM_UNSIGNED) {
                *--p = 'u';
                slash = true;
            }
            if (slash)
                *--p = '/';

            if (flags & (SPARM_GREEDY|SPARM_VARADIC))
                *--p = '+';
	    p -= m->params[i].name.len;
	    memcpy(p, tok_text(&m->params[i].name), m->params[i].name.len);

            if (flags & SPARM_NOSTRIP)
                *--p = '!';
            if (flags & SPARM_STR) {
                *--p = '&';
                if (flags & SPARM_CONDQUOTE)
                    *--p = '&';
            }
            if (flags & SPARM_EVAL)
                *--p = '=';
            *--p = ',';
        }
        *p = '(';               /* First parameter starts with ( not , */
    }

    p -= namelen;
    memcpy(p, m->name, namelen);

    if (context_prefix) {
        p -= context_len;
        memcpy(p, context_prefix, context_len);
        nasm_free(context_prefix);
    }

    if (ppdbg & PDBG_LIST_SMACROS)
        nasm_listmsg("%s %s", pp_directives[op], p);
    if (ppdbg & PDBG_SMACROS) {
        bool define = !(op == PP_UNDEF || op == PP_UNDEFALIAS);
        if (!define)
            *end_spec = '\0';   /* Remove the expansion (for list file only) */
        dfmt->debug_smacros(define, p);
    }
    nasm_free(def);
}

/*
 * Parse smacro arguments, return argument count. If the tmpl argument
 * is set, set the nparam, varadic and params field in the template.
 * The varadic field is not used by define_smacro(), but is provided
 * in case the caller wants it for other purposes.
 *
 * *tpp is updated to point to the pointer to the first token after the
 * prototype.
 *
 * The text values from any argument tokens are "stolen" and the
 * corresponding text fields set to NULL.
 *
 * Note that the user can't define a true varadic macro; doing so
 * would be meaningless. The true varadic macros are only used for
 * internal "magic macro" functions.
 */
static int parse_smacro_template(Token ***tpp, SMacro *tmpl)
{
    int nparam = 0;
    enum sparmflags flags;
    struct smac_param *params = NULL;
    bool err, done;
    bool greedy = false;
    bool parsing_radix;
    char radix;
    Token **tn = *tpp;
    Token *t = *tn;
    Token *name;

    /*
     * DO NOT skip whitespace here, or we won't be able to distinguish:
     *
     * %define foo (a,b)		; no arguments, (a,b) is the expansion
     * %define bar(a,b)			; two arguments, empty expansion
     *
     * This ambiguity was inherited from C.
     */

    if (!tok_is(t, '('))
        goto finish;

    if (tmpl) {
        Token *tx = t;
        Token **txpp = &tx;
        int sparam;

        /* Count parameters first */
        sparam = parse_smacro_template(&txpp, NULL);
        if (!sparam)
            goto finish;        /* No parameters, we're done */
        nasm_newn(params, sparam);
    }

    /* Skip leading paren */
    tn = &t->next;
    t = *tn;

    name = NULL;
    flags = 0;
    radix = 0;
    parsing_radix = false;
    err = done = false;

    while (!done) {
        if (!t) {
            if (name || flags)
                nasm_nonfatal("`)' expected to terminate macro template");
            else
                nasm_nonfatal("parameter identifier expected");
            break;
        }

        switch (t->type) {
        case TOKEN_ID:
            if (parsing_radix) {
                const char *cp;
                for (cp = tok_text(t); cp && *cp; cp++) {
                    switch (*cp | 0x20) {
                    case 'b': case 'y':
                    case 'd': case 't':
                    case 'o': case 'q':
                    case 'h': case 'x':
                        radix = *cp;
                        break;
                    case 's':
                        flags &= ~SPARM_UNSIGNED;
                        break;
                    case 'u':
                        flags |= SPARM_UNSIGNED;
                        break;
                    default:
                        nasm_nonfatal("invalid radix specifier `/%s'",
                                      tok_text(t));
                        cp = NULL;
                        break;
                    }
                    if (!cp) /* stop on invalid radix specifier */
                        break;
                }
            } else {
                if (name)
                    goto bad;
                name = t;
            }
            break;
        case '=':
            flags |= SPARM_EVAL;
            break;
        case '&':
            flags |= SPARM_STR;
            break;
        case TOKEN_DBL_AND:
            flags |= SPARM_STR|SPARM_CONDQUOTE;
            break;
        case '!':
            flags |= SPARM_NOSTRIP;
            break;
        case '+':
            flags |= SPARM_GREEDY|SPARM_OPTIONAL;
            greedy = true;
            break;
        case '/':
            if (!(flags & SPARM_EVAL))
                nasm_nonfatal("radix specifier for parameter without `='");
            parsing_radix = true;
            break;
        case ',':
            if (greedy)
                nasm_nonfatal("greedy parameter must be last");
            goto end_param;
        case ')':
            done = true;
            goto end_param;
        end_param:
            if (params) {
                if (name)
                    steal_Token(&params[nparam].name, name);
                params[nparam].flags = flags;
                params[nparam].radix = radix;
            }
            nparam++;
            name = NULL;
            flags = 0;
            parsing_radix = false;
            radix = 0;
            break;
        case TOKEN_WHITESPACE:
            break;
        default:
        bad:
            if (!err) {
                nasm_nonfatal("garbage `%s' in macro parameter list",
                              tok_text(t));
                err = true;
            }
            break;
        }

        tn = &t->next;
        t = *tn;
    }

finish:
    while (t && t->type == TOKEN_WHITESPACE) {
        tn = &t->next;
        t = t->next;
    }
    *tpp = tn;
    if (tmpl) {
        tmpl->nparam     = nparam;
        tmpl->varadic    = greedy;
        tmpl->params     = params;
    }
    return nparam;
}

/*
 * Common code for defining an smacro. The tmpl argument, if not NULL,
 * contains any macro parameters that aren't explicit arguments;
 * those are the more uncommon macro variants.
 */
static SMacro *define_smacro(const char *mname, bool casesense,
                             Token *expansion, SMacro *tmpl)
{
    SMacro *smac, **smhead;
    struct hash_table *smtbl;
    Context *ctx;
    bool defining_alias = false;
    int nparam = 0;
    bool defined;

    if (tmpl) {
        defining_alias = tmpl->alias;
        nparam = tmpl->nparam;
        if (nparam && !defining_alias)
            mark_smac_params(expansion, tmpl, 0);
    }

    ctx = get_ctx(mname, &mname);

    defined = smacro_defined(ctx, mname, nparam, &smac, casesense, true);

    if (defined) {
        if (smac->alias) {
            if (smac->in_progress) {
                nasm_nonfatal("macro alias loop");
                goto fail;
            }

            if (!defining_alias && !ppconf.noaliases) {
                /* It is an alias macro; follow the alias link */
                SMacro *s;

                smac->in_progress++;
                s = define_smacro(tok_text(smac->expansion), casesense,
                                  expansion, tmpl);
                smac->in_progress--;
                return s;
            }
        }

        if (casesense ^ smac->casesense) {
            /*
             */
            nasm_warn(WARN_PP_MACRO_DEF_CASE_SINGLE, "case %ssensitive definition of macro `%s' will shadow %ssensitive macro `%s'",
                      casesense ? "" : "in",
                      mname,
                      smac->casesense ? "" : "in",
                      smac->name);
            defined = false;
        } else if ((!!nparam) ^ (!!smac->nparam)) {
            /*
             * The immediately previous versions of NASM considered
             * this an error, so promote this warning is promoted to
             * to error by default.
             */
            nasm_warn(WARN_PP_MACRO_DEF_PARAM_SINGLE,
                      "macro `%s' defined both with and without parameters",
                      mname);
            defined = false;
        } else if (smac->nparam < nparam) {
            nasm_warn(WARN_PP_MACRO_DEF_GREEDY_SINGLE,
                      "defining macro `%s' shadows previous greedy definition",
                      mname);
            defined = false;
        }
    }

    if (defined) {
        /*
         * We're redefinining, so we have to take over an
         * existing SMacro structure. This means freeing
         * what was already in it, but not the structure itself.
         */
        clear_smacro(smac);
    } else {
        /* Create a new macro */
        smtbl  = ctx ? &ctx->localmac : &smacros;
        smhead = (SMacro **) hash_findi_add(smtbl, mname);
        nasm_new(smac);
        smac->next = *smhead;
        *smhead = smac;
    }

    smac->name       = nasm_strdup(mname);
    smac->casesense  = casesense;
    smac->expansion  = reverse_tokens(expansion);
    smac->expand     = smacro_expand_default;
    smac->nparam     = nparam;
    smac->nparam_min = nparam;
    if (tmpl) {
        smac->params     = tmpl->params;
        smac->alias      = tmpl->alias;
        smac->recursive  = tmpl->recursive;
        if (tmpl->expand) {
            smac->expand    = tmpl->expand;
            smac->expandpvt = tmpl->expandpvt;
        }
        if (nparam) {
            int nparam_min = nparam;

            smac->varadic =
                !!(tmpl->params[nparam-1].flags &
                   (SPARM_GREEDY|SPARM_VARADIC));

            while (nparam_min > 1) {
                if (!(tmpl->params[nparam_min-1].flags & SPARM_OPTIONAL))
                    break;
                nparam_min--;
            }

            smac->nparam_min = nparam_min;
        }
    }
    if (ppdbg & (PDBG_SMACROS|PDBG_LIST_SMACROS)) {
        list_smacro_def((smac->alias ? PP_DEFALIAS : PP_DEFINE)
                        + !casesense, ctx, smac);
    }
    return smac;

fail:
    delete_tlist(expansion);
    if (tmpl)
        free_smacro_members(tmpl);
    return NULL;
}

/*
 * Undefine an smacro
 */
static void undef_smacro(const char *mname, bool undefalias)
{
    SMacro **smhead, *s, **sp;
    struct hash_table *smtbl;
    Context *ctx;

    ctx = get_ctx(mname, &mname);
    smtbl = ctx ? &ctx->localmac : &smacros;
    smhead = (SMacro **)hash_findi(smtbl, mname, NULL);

    if (smhead) {
        /*
         * We now have a macro name... go hunt for it.
         */
        sp = smhead;
        while ((s = *sp) != NULL) {
            if (!mstrcmp(s->name, mname, s->casesense)) {
                if (s->alias && !undefalias) {
                    if (!ppconf.noaliases) {
                        if (s->in_progress) {
                            nasm_nonfatal("macro alias loop");
                        } else {
                            s->in_progress = true;
                            undef_smacro(tok_text(s->expansion), false);
                            s->in_progress = false;
                        }
                    }
                } else {
                    if (list_option('d'))
                        list_smacro_def(s->alias ? PP_UNDEFALIAS : PP_UNDEF,
                                        ctx, s);
                    *sp = s->next;
                    free_smacro(s);
                    continue;
                }
            }
            sp = &s->next;
        }
    }
}

/*
 * Parse a mmacro specification.
 */
static bool parse_mmacro_spec(Token *tline, MMacro *def, const char *directive)
{
    tline = tline->next;
    tline = skip_white(tline);
    tline = expand_id(tline);
    if (!tok_is(tline, TOKEN_ID)) {
        nasm_nonfatal("`%s' expects a macro name", directive);
        return false;
    }

#if 0
    def->prev = NULL;
#endif
    def->name = dup_text(tline);
    def->plus = false;
    def->nolist = 0;
    def->nparam_min = 0;
    def->nparam_max = 0;

    tline = expand_smacro(tline->next);
    tline = skip_white(tline);
    if (!tok_is(tline, TOKEN_NUM))
        nasm_nonfatal("`%s' expects a parameter count", directive);
    else
        def->nparam_min = def->nparam_max = read_param_count(tok_text(tline));
    if (tline && tok_is(tline->next, '-')) {
        tline = tline->next->next;
        if (tok_is(tline, '*')) {
            def->nparam_max = INT_MAX;
        } else if (!tok_is(tline, TOKEN_NUM)) {
            nasm_nonfatal("`%s' expects a parameter count after `-'", directive);
        } else {
            def->nparam_max = read_param_count(tok_text(tline));
            if (def->nparam_min > def->nparam_max) {
                nasm_nonfatal("minimum parameter count exceeds maximum");
                def->nparam_max = def->nparam_min;
            }
        }
    }
    if (tline && tok_is(tline->next, '+')) {
        tline = tline->next;
        def->plus = true;
    }
    if (tline && tok_is(tline->next, TOKEN_ID) &&
	tline->next->len == 7 &&
        !nasm_stricmp(tline->next->text.a, ".nolist")) {
        tline = tline->next;
        if (!list_option('f'))
            def->nolist |= NL_LIST|NL_LINE;
    }

    /*
     * Handle default parameters.
     */
    def->ndefs = 0;
    if (tline && tline->next) {
        Token **comma;
        def->dlist = tline->next;
        tline->next = NULL;
        comma = count_mmac_params(def->dlist, &def->ndefs, &def->defaults);
        if (!ppconf.sane_empty_expansion && comma) {
            *comma = NULL;
            def->ndefs--;
            nasm_warn(WARN_PP_MACRO_PARAMS_LEGACY,
                      "dropping trailing empty default parameter in definition of multi-line macro `%s'",
                      def->name);
        }
    } else {
        def->dlist = NULL;
        def->defaults = NULL;
    }
    def->expansion = NULL;

    if (def->defaults && def->ndefs > def->nparam_max - def->nparam_min &&
        !def->plus) {
        nasm_warn(WARN_PP_MACRO_DEFAULTS,
                   "too many default macro parameters in macro `%s'", def->name);
    }

    return true;
}


/*
 * Decode a size directive
 */
static int parse_size(const char *str) {
    struct tokenval tv;
    if (nasm_token_hash(str, &tv) != TOKEN_SIZE)
        return 0;

    return tv.t_inttwo;
}

/*
 * Process a preprocessor %pragma directive.  Currently there are none.
 * Gets passed the token list starting with the "preproc" token from
 * "%pragma preproc".
 */
static void do_pragma_preproc(Token *tline)
{
    const char *txt;

    /* Skip to the real stuff */
    tline = tline->next;
    tline = skip_white(tline);

    if (!tok_is(tline, TOKEN_ID))
        return;

    txt = tok_text(tline);
    if (!nasm_stricmp(txt, "sane_empty_expansion")) {
        tline = skip_white(tline->next);
        ppconf.sane_empty_expansion =
            pp_get_boolean_option(tline, ppconf.sane_empty_expansion);
    } else {
        /* Unknown pragma, ignore for now */
    }
}

static const char *get_id_noskip(Token **tp, const char *dname);

static const char *get_id(Token **tp, const char *dname)
{
    *tp = (*tp)->next;          /* Skip directive */
    return get_id_noskip(tp, dname);
}

static const char *get_id_noskip(Token **tp, const char *dname)
{
    const char *id;
    Token *t = *tp;

    t = skip_white(t);
    t = expand_id(t);

    if (!tok_macro_id(t)) {
        nasm_nonfatal("`%s' expects a macro identifier", dname);
        return NULL;
    }

    id = tok_text(t);
    nasm_assert(!tok_white(t)); /* Had skip_white() here?? */
    *tp = t;
    return id;
}

/* Parse a %use package name and find the package. Set *err on syntax error. */
static const struct use_package *
get_use_pkg(Token *t, const char *dname, const char **name)
{
    const char *id;

    t = skip_white(t);
    t = expand_smacro(t);

    *name = NULL;

    if (!t) {
        nasm_nonfatal("`%s' expects a package name, got end of line", dname);
        return NULL;
    } else if (t->type != TOKEN_ID && t->type != TOKEN_STR) {
        nasm_nonfatal("`%s' expects a package name, got `%s'",
                      dname, tok_text(t));
        return NULL;
    }

    *name = id = unquote_token(t);

    t = t->next;
    t = skip_white(t);
    if (t) {
        nasm_warn(WARN_PP_TRAILING,
                  "trailing garbage after `%s' ignored", dname);
    }

    return nasm_find_use_package(id);
}

/*
 * Mark parameter tokens in an smacro definition. If the type argument
 * is 0, create smac param tokens, otherwise use the type specified;
 * normally this is used for TOKEN_XDEF_PARAM, which is used to protect
 * parameter tokens during expansion during %xdefine.
 *
 * tmpl may not be NULL here.
 */
static void mark_smac_params(Token *tline, const SMacro *tmpl,
                             enum token_type type)
{
    const struct smac_param *params = tmpl->params;
    int nparam = tmpl->nparam;
    Token *t;
    int i;

    list_for_each(t, tline) {
        if (t->type != TOKEN_ID && t->type != TOKEN_XDEF_PARAM)
            continue;

        for (i = 0; i < nparam; i++) {
            if (tok_text_match(t, &params[i].name))
                t->type = type ? type : tok_smac_param(i);
        }
    }
}

/**
 * %clear selected macro sets either globally or in contexts
 */
static void do_clear(enum clear_what what, bool context)
{
    if (context) {
        if (what & CLEAR_ALLDEFINE) {
            Context *ctx;
            list_for_each(ctx, cstk)
                clear_smacro_table(&ctx->localmac, what);
        }
        /* Nothing else can be context-local */
    } else {
        if (what & CLEAR_ALLDEFINE)
            clear_smacro_table(&smacros, what);
        if (what & CLEAR_MMACRO)
            free_mmacro_table(&mmacros);
    }
}

/*
 * Process a %line directive, including the gcc/cpp compatibility
 * form with a # at the front.
 */
static int line_directive(Token *origline, Token *tline)
{
    int k, m;
    bool err;
    const char *dname;

    /*
     * Valid syntaxes:
     * %line nnn[+mmm] [filename]
     * %line nnn[+mmm] "filename" flags...
     *
     * "flags" are for gcc compatibility and are currently ignored.
     *
     * "#" at the beginning of the line is also treated as a %line
     * directive, again for compatibility with gcc.
     */
    if ((ppopt & PP_NOLINE) || istk->mstk.mstk)
        goto done;

    dname = tok_text(tline);
    tline = tline->next;
    tline = skip_white(tline);
    if (!tok_is(tline, TOKEN_NUM)) {
        nasm_nonfatal("`%s' expects a line number", dname);
        goto done;
    }
    k = readnum(tok_text(tline), &err);
    m = 1;
    tline = tline->next;
    if (tok_is(tline, '+') || tok_is(tline, '-')) {
        bool minus = tok_is(tline, '-');
        tline = tline->next;
        if (!tok_is(tline, TOKEN_NUM)) {
            nasm_nonfatal("`%s' expects a line increment", dname);
            goto done;
        }
        m = readnum(tok_text(tline), &err);
        if (minus)
            m = -m;
        tline = tline->next;
    }
    tline = skip_white(tline);
    if (tline) {
        if (tline->type == TOKEN_STR) {
            const char *fname;
            /*
             * If this is a quoted string, ignore anything after
             * it; this allows for compatibility with gcc's
             * additional flags options.
             */

            fname = unquote_token_anystr(tline, BADCTL,
                                          dname[0] == '#' ? STR_C : STR_NASM);
            src_set_fname(fname);
        } else {
            char *fname;
            fname = detoken(tline, false);
            src_set_fname(fname);
            nasm_free(fname);
        }
    }
    src_set_linnum(k);

    istk->where = src_where();
    istk->lineinc = m;
    goto done;

done:
    delete_tlist(origline);
    return DIRECTIVE_FOUND;
}

/*
 * Used for the %arg and %local directives
 */
static void define_stack_smacro(const char *name, int offset)
{
    Token *tt;

    tt = make_tok_char(NULL, ')');
    tt = make_tok_num(tt, offset);
    if (!tok_is(tt, '-'))
        tt = make_tok_char(tt, '+');
    tt = new_Token(tt, TOKEN_ID, StackPointer, 0);
    tt = make_tok_char(tt, '(');

    define_smacro(name, true, tt, NULL);
}


/*
 * This implements the %assign directive: expand an smacro expression,
 * then evaluate it, and assign the corresponding number to an smacro.
 */
static void assign_smacro(const char *mname, bool casesense,
                          Token *tline, const char *dname)
{
    struct ppscan pps;
    expr *evalresult;
    struct tokenval tokval;

    tline = expand_smacro(tline);

    pps.tptr = tline;
    pps.ntokens = -1;
    tokval.t_type = TOKEN_INVALID;
    evalresult = evaluate(ppscan, &pps, &tokval, NULL, true, NULL);
    delete_tlist(tline);
    if (!evalresult)
        return;

    if (tokval.t_type) {
        nasm_warn(WARN_PP_TRAILING,
                  "trailing garbage after expression ignored");
    }
    if (!is_simple(evalresult)) {
        nasm_nonfatal("non-constant value given to `%s'", dname);
    } else {
	tline = make_tok_num(NULL, reloc_value(evalresult));

        /*
         * We now have a macro name, an implicit parameter count of
         * zero, and a numeric token to use as an expansion. Create
         * and store an SMacro.
         */
        define_smacro(mname, casesense, tline, NULL);
    }
}

/*
 * Implement string concatenation as used by the %strcat directive
 * and function.
 */
static Token *pp_strcat(Token *tline, const char *dname)
{

    size_t len;
    Token *t;
    Token *res = NULL;
    char *q, *qbuf;

    len = 0;
    list_for_each(t, tline) {
        switch (t->type) {
        case TOKEN_WHITESPACE:
        case TOKEN_COMMA:
            break;
        case TOKEN_STR:
            unquote_token(t);
            /* fall through */
        case TOKEN_INTERNAL_STR:
            len += t->len;
            break;
        default:
            nasm_nonfatal("non-string passed to `%s': %s", dname,
                          tok_text(t));
            goto err;
        }
    }

    q = qbuf = nasm_malloc(len+1);
    list_for_each(t, tline) {
        if (t->type == TOKEN_INTERNAL_STR)
            q = mempcpy(q, tok_text(t), t->len);
    }
    *q = '\0';

    res = make_tok_qstr_len(NULL, qbuf, len);
    nasm_free(qbuf);
err:
    delete_tlist(tline);
    return res;
}


/*
 * Implement substring extraction as used by the %substr directive
 * and function.
 */
static Token *pp_substr_common(Token *t, int64_t start, int64_t count);
static const char *pp_get_substr(Token *t, int64_t start, int64_t *countp);

static Token *pp_substr(Token *tline, const char *dname)
{
    int64_t start, count;
    struct ppscan pps;
    Token *t;
    Token *res = NULL;
    expr *evalresult;
    struct tokenval tokval;

    t = skip_white(tline);

    if (!tok_is(t, TOKEN_STR)) {
        nasm_nonfatal("`%s' requires a string as parameter", dname);
        goto err;
    }

    pps.tptr = skip_white(t->next);
    if (tok_is(pps.tptr, TOKEN_COMMA))
        pps.tptr = skip_white(pps.tptr->next);
    if (!pps.tptr) {
        nasm_nonfatal("`%s' requires a starting index", dname);
        goto err;
    }

    pps.ntokens = -1;
    tokval.t_type = TOKEN_INVALID;
    evalresult = evaluate(ppscan, &pps, &tokval, NULL, true, NULL);
    if (!evalresult) {
        goto err;
    } else if (!is_simple(evalresult)) {
        nasm_nonfatal("non-constant value given to `%s'", dname);
        goto err;
    }
    start = evalresult->value;

    pps.tptr = skip_white(pps.tptr);
    if (!pps.tptr) {
        count = 1;  /* Backwards compatibility: one character */
    } else {
        tokval.t_type = TOKEN_INVALID;
        evalresult = evaluate(ppscan, &pps, &tokval, NULL, true, NULL);
        if (!evalresult) {
            goto err;
        } else if (!is_simple(evalresult)) {
            nasm_nonfatal("non-constant value given to `%s'", dname);
            goto err;
        }
        count = evalresult->value;
    }

    res = pp_substr_common(t, start, count);

err:
    delete_tlist(tline);
    return res;
}

static const char *pp_get_substr(Token *t, int64_t start, int64_t *countp)
{
    size_t len;
    int64_t count = *countp;

    unquote_token(t);
    len = t->len;

    /* make start and count being in range */
    start -= 1;                 /* First character is 1 */

    if (start < 0)
        start = 0;
    if (count < 0)
        count = len + count + 1 - start;
    if (start + count > (int64_t)len)
        count = len - start;
    if (!len || count < 0 || start >= (int64_t)len)
        start = -1; /* empty string */

    if (start < 0) {
        *countp = 0;
        return "";
    } else {
        *countp = count;
        return tok_text(t) + start;
    }
}

static Token *pp_substr_common(Token *t, int64_t start, int64_t count)
{
    const char *txt = pp_get_substr(t, start, &count);
    return make_tok_qstr_len(NULL, txt, count);
}

/*
 * %exitmacro (named = true) or %exitrep (named = false);
 * returns the MMacro structure being exited.
 */
static MMacro *do_exit_macro(const char *dname, bool named)
{
    Line *l;

    /*
     * We must search along istk->expansion until we hit a
     * macro-end marker for a macro with a name. Then we
     * suppress all lines between exitmacro and endmacro.
     */
    list_for_each(l, istk->expansion) {
        if (l->finishes && (!!l->finishes->name == named))
            break;
    }

    if (l) {
        Line *ll;
        for (ll = istk->expansion; ll != l; ll = ll->next)
            ll->suppressed = true;
        return l->finishes;
    } else {
        nasm_nonfatal("`%s' not within `%%%s' block", dname, dname+5);
        return NULL;
    }
}

/*
 * Issue a user-originated error message
 */
static void user_error(enum preproc_token op, Token **tlinep)
{
    Token *tline, *t;
    char *p;
    const char *q;
    errflags severity;

    switch (op) {
    case PP_NOTE:
        severity = ERR_NOTE;
        break;
    case PP_WARNING:
        severity = ERR_WARNING|WARN_USER|ERR_PASS2;
        break;
    case PP_ERROR:
        /* Only error out if this is the final pass */
        severity = ERR_NONFATAL|ERR_PASS2;
        break;
    case PP_FATAL:
        severity = ERR_FATAL;
        break;
    default:
        panic();
    }

    *tlinep = tline = expand_smacro(skip_white(*tlinep));
    t = skip_tok_white(tline);

    if (tok_is(tline, TOKEN_STR) && !t) {
        /* The line contains only a quoted string */
        p = NULL;                 /* Don't try to free */
        q = unquote_token(tline); /* Ignore NUL character truncation */
    } else {
        /* Not a quoted string, or more than one quoted string */
        q = p = detoken(tline, false);
    }

    q = nasm_skip_spaces(q);
    if (!*q)
        q = pp_directives[op];   /* Less confusing than an empty message */

    nasm_error(severity, "%s", q);
    nasm_free(p);               /* p == NULL if nothing to free */
}

/**
 * find and process preprocessor directive in passed line
 * Find out if a line contains a preprocessor directive, and deal
 * with it if so.
 *
 * If a directive _is_ found, it is the responsibility of this routine
 * (and not the caller) to delete_tlist() the line.
 *
 * @param tline a pointer to the current tokeninzed line linked list
 * @param output if this directive generated output
 * @param suppressed if this input line was suppressed (look for condition end)
 * @return DIRECTIVE_FOUND or NO_DIRECTIVE_FOUND
 *
 */
static int do_directive(Token *tline, Token **output, bool suppressed)
{
    enum preproc_token op;
    int j;
    enum nolist_flags nolist;
    bool casesense;
    int offset;
    const char *p;
    char *q;
    const char *mname;
    struct ppscan pps;
    Include *inc;
    Context *ctx;
    Cond *cond;
    MMacro *mmac, **mmhead;
    Token *t = NULL, *tt, *macro_start, *origline;
    struct tokenval tokval;
    expr *evalresult;
    int64_t count;
    const char *dname;          /* Name of directive, for messages */

    *output = NULL;             /* No output generated */
    origline = tline;

    /* cpp-like line directive, must not be preceded by whitespace */
    if (tok_is(tline, '#'))
        return line_directive(origline, tline);

    tline = skip_white(tline);
    if (!tline)
        return NO_DIRECTIVE_FOUND;

    dname = tok_text(tline);

    switch (tline->type) {
    case TOKEN_PREPROC_ID:
        op = pp_get_nasm_directive(dname);
        if (op == PP_invalid) {
            /* Look to see if it is a misspelled or future conditional */
            if (!nasm_strnicmp(dname, "%if", 3))
                op = PP_IF_BOGUS;
            else if (!nasm_strnicmp(dname, "%elif", 5))
                op = PP_ELIF_BOGUS;
        }
        break;

    case TOKEN_ID:
        op = pp_get_tasm_directive(tok_text(tline));
        break;

    default:
        op = PP_invalid;
        break;
    }

    switch (op) {
    case PP_invalid:
        return NO_DIRECTIVE_FOUND;

    case PP_LINE:
        /*
         * %line directives are always processed immediately and
         * unconditionally, as they are intended to reflect position
         * in externally preprocessed sources.
         */
        return line_directive(origline, tline);

    default:
        break;
    }

    if (op == PP_invalid)
        return NO_DIRECTIVE_FOUND;

    if (unlikely(ppopt & PP_TRIVIAL))
        goto done;

    casesense = true;
    if (PP_HAS_CASE(op) & PP_INSENSITIVE(op)) {
        casesense = false;
        op--;
    }

    /*
     * If we're in a non-emitting branch of a condition construct,
     * or walking to the end of an already terminated %rep block,
     * we should ignore all directives except for condition
     * directives.
     */
    if (suppressed && !is_condition(op)) {
        return NO_DIRECTIVE_FOUND;
    }

    /*
     * If we're defining a macro or reading a %rep block, we should
     * ignore all directives except for %macro/%imacro (which nest),
     * %endm/%endmacro, %line and (only if we're in a %rep block) %endrep.
     * If we're in a %rep block, another %rep nests, so should be let through.
     */
    if (defining && op != PP_MACRO && op != PP_RMACRO &&
        op != PP_ENDMACRO && op != PP_ENDM &&
        (defining->name || (op != PP_ENDREP && op != PP_REP))) {
        return NO_DIRECTIVE_FOUND;
    }

    if (defining) {
        if (op == PP_MACRO || op == PP_RMACRO) {
            nested_mac_count++;
            return NO_DIRECTIVE_FOUND;
        } else if (nested_mac_count > 0) {
            if (op == PP_ENDMACRO) {
                nested_mac_count--;
                return NO_DIRECTIVE_FOUND;
            }
        }
        if (!defining->name) {
            if (op == PP_REP) {
                nested_rep_count++;
                return NO_DIRECTIVE_FOUND;
            } else if (nested_rep_count > 0) {
                if (op == PP_ENDREP) {
                    nested_rep_count--;
                    return NO_DIRECTIVE_FOUND;
                }
            }
        }
    }

    if (pp_op_may_be_function[op]) {
        if (tok_is(skip_white(tline->next), '('))
            return NO_DIRECTIVE_FOUND; /* Expand as a preprocessor function */
    }

    switch (op) {
    default:
        nasm_nonfatal("unknown preprocessor directive `%s'", dname);
        return NO_DIRECTIVE_FOUND;      /* didn't get it */

    case PP_PRAGMA:
        /*
         * %pragma namespace options...
         *
         * The namespace "preproc" is reserved for the preprocessor;
         * all other namespaces generate a [pragma] assembly directive.
         *
         * Invalid %pragmas are ignored and may have different
         * meaning in future versions of NASM.
         */
        t = tline;
        tline = tline->next;
        t->next = NULL;
        tline = zap_white(expand_smacro(tline));
        if (tok_is(tline, TOKEN_ID)) {
            if (!nasm_stricmp(tok_text(tline), "preproc")) {
                /* Preprocessor pragma */
                do_pragma_preproc(tline);
                delete_tlist(tline);
            } else {
                /* Build the assembler directive */

                /* Append bracket to the end of the output */
                for (t = tline; t->next; t = t->next)
                    ;
                t->next = make_tok_char(NULL, ']');

                /* Prepend "[pragma " */
                t = new_White(tline);
                t = new_Token(t, TOKEN_ID, "pragma", 6);
                t = make_tok_char(t, '[');
                tline = t;
                *output = tline;
            }
        }
        break;

    case PP_STACKSIZE:
    {
        const char *arg;

        /* Directive to tell NASM what the default stack size is. The
         * default is for a 16-bit stack, and this can be overridden with
         * %stacksize large.
         */
        tline = skip_white(tline->next);
        if (!tline || tline->type != TOKEN_ID) {
            nasm_nonfatal("`%s' missing size parameter", dname);
            break;
        }

        arg = tok_text(tline);

        if (nasm_stricmp(arg, "flat") == 0) {
            /* All subsequent ARG directives are for a 32-bit stack */
            StackSize = 4;
            StackPointer = "ebp";
            ArgOffset = 8;
            LocalOffset = 0;
        } else if (nasm_stricmp(arg, "flat64") == 0) {
            /* All subsequent ARG directives are for a 64-bit stack */
            StackSize = 8;
            StackPointer = "rbp";
            ArgOffset = 16;
            LocalOffset = 0;
        } else if (nasm_stricmp(arg, "large") == 0) {
            /* All subsequent ARG directives are for a 16-bit stack,
             * far function call.
             */
            StackSize = 2;
            StackPointer = "bp";
            ArgOffset = 4;
            LocalOffset = 0;
        } else if (nasm_stricmp(arg, "small") == 0) {
            /* All subsequent ARG directives are for a 16-bit stack,
             * far function call. We don't support near functions.
             */
            StackSize = 2;
            StackPointer = "bp";
            ArgOffset = 6;
            LocalOffset = 0;
        } else {
            nasm_nonfatal("`%s' invalid size type", dname);
        }
        break;
    }

    case PP_ARG:
        /* TASM like ARG directive to define arguments to functions, in
         * the following form:
         *
         *      ARG arg1:WORD, arg2:DWORD, arg4:QWORD
         */
        offset = ArgOffset;
        do {
            const char *arg;
            int size = StackSize;

            /* Find the argument name */
            tline = skip_white(tline->next);
            if (!tline || tline->type != TOKEN_ID) {
                nasm_nonfatal("`%s' missing argument parameter", dname);
                goto done;
            }
            arg = tok_text(tline);

            /* Find the argument size type */
            tline = tline->next;
            if (!tok_is(tline, ':')) {
                nasm_nonfatal("syntax error processing `%s' directive", dname);
                goto done;
            }
            tline = tline->next;
            if (!tok_is(tline, TOKEN_ID)) {
                nasm_nonfatal("`%s' missing size type parameter", dname);
                goto done;
            }

            /* Allow macro expansion of type parameter */
            tt = tokenize(tok_text(tline));
            tt = expand_smacro(tt);
            size = parse_size(tok_text(tt));
            delete_tlist(tt);
            if (!size) {
                nasm_nonfatal("invalid size type for `%s' missing directive", dname);
                goto done;
            }

            /* Round up to even stack slots */
            size = ALIGN(size, StackSize);

            /* Now define the macro for the argument */
            define_stack_smacro(arg, offset);
            offset += size;

            /* Move to the next argument in the list */
            tline = skip_white(tline->next);
        } while (tok_is(tline, ','));
        ArgOffset = offset;
        break;

    case PP_LOCAL:
    {
        int total_size = 0;

        /* TASM like LOCAL directive to define local variables for a
         * function, in the following form:
         *
         *      LOCAL local1:WORD, local2:DWORD, local4:QWORD = LocalSize
         *
         * The '= LocalSize' at the end is ignored by NASM, but is
         * required by TASM to define the local parameter size (and used
         * by the TASM macro package).
         */
        offset = LocalOffset;
        do {
            const char *local;
            int size = StackSize;

            /* Find the argument name */
            tline = skip_white(tline->next);
            if (!tline || tline->type != TOKEN_ID) {
                nasm_nonfatal("`%s' missing argument parameter", dname);
                goto done;
            }
            local = tok_text(tline);

            /* Find the argument size type */
            tline = tline->next;
            if (!tok_is(tline, ':')) {
                nasm_nonfatal("syntax error processing `%s' directive", dname);
                goto done;
            }
            tline = tline->next;
            if (!tok_is(tline, TOKEN_ID)) {
                nasm_nonfatal("`%s' missing size type parameter", dname);
                goto done;
            }

            /* Allow macro expansion of type parameter */
            tt = tokenize(tok_text(tline));
            tt = expand_smacro(tt);
            size = parse_size(tok_text(tt));
            delete_tlist(tt);
            if (!size) {
                nasm_nonfatal("invalid size type for `%s' missing directive", dname);
                goto done;
            }

            /* Round up to even stack slots */
            size = ALIGN(size, StackSize);

            offset += size;     /* Negative offset, increment before */

            /* Now define the macro for the argument */
            define_stack_smacro(local, -offset);

            /* How is this different from offset? */
            total_size += size;

            /* Move to the next argument in the list */
            tline = skip_white(tline->next);
        } while (tok_is(tline, ','));

        /* Now define the assign to setup the enter_c macro correctly */
        tt = make_tok_num(NULL, total_size);
        tt = make_tok_char(tt, '+');
        tt = new_Token(tt, TOKEN_LOCAL_MACRO, "%$localsize", 11);
        assign_smacro("%$localsize", true, tt, dname);

        LocalOffset = offset;
        break;
    }
    case PP_CLEAR:
    {
        bool context = false;

        t = tline->next = expand_smacro(tline->next);
        t = skip_white(t);
        if (!t) {
            /* Emulate legacy behavior */
            do_clear(CLEAR_DEFINE|CLEAR_MMACRO, false);
        } else {
            while (tok_is(t, TOKEN_ID)) {
                const char *txt = tok_text(t);

                /*
                 * Advance to the next token, skipping whitespace
                 * and optional comma separators.
                 */
                while ((t = skip_white(t->next)) && tok_is(t, ','))
                    ;

                if (!nasm_stricmp(txt, "all")) {
                    do_clear(CLEAR_ALL, context);
                } else if (!nasm_stricmp(txt, "define") ||
                           !nasm_stricmp(txt, "def") ||
                           !nasm_stricmp(txt, "smacro")) {
                    do_clear(CLEAR_DEFINE, context);
                } else if (!nasm_stricmp(txt, "defalias") ||
                           !nasm_stricmp(txt, "alias") ||
                           !nasm_stricmp(txt, "salias")) {
                    do_clear(CLEAR_DEFALIAS, context);
                } else if (!nasm_stricmp(txt, "alldef") ||
                           !nasm_stricmp(txt, "alldefine")) {
                    do_clear(CLEAR_ALLDEFINE, context);
                } else if (!nasm_stricmp(txt, "macro") ||
                           !nasm_stricmp(txt, "mmacro")) {
                    do_clear(CLEAR_MMACRO, context);
                } else if (!nasm_stricmp(txt, "context") ||
                           !nasm_stricmp(txt, "ctx")) {
                    context = true;
                } else if (!nasm_stricmp(txt, "global")) {
                    context = false;
                } else if (!nasm_stricmp(txt, "nothing") ||
                         !nasm_stricmp(txt, "none") ||
                         !nasm_stricmp(txt, "ignore") ||
                         !nasm_stricmp(txt, "-") ||
                         !nasm_stricmp(txt, "--")) {
                    /* Do nothing */
                } else {
                    nasm_nonfatal("invalid option to %s: %s", dname, txt);
                    t = NULL;
                    break;
                }
            }
        }

        t = skip_white(t);
        if (t) {
            nasm_warn(WARN_PP_TRAILING,
                      "trailing garbage after `%s' ignored", dname);
        }
        break;
    }

    case PP_DEPEND:
        t = pp_do_depend(&tline->next, dname);
        delete_Token(t);
        goto done;

    case PP_INCLUDE:
    case PP_REQUIRE:
    {
        const struct file_hash_entry *fhe;

        t = tlist_filename(&tline->next, NULL, dname);
        if (!t)
            goto done;

        nasm_new(inc);
        inc->next = istk;
        p = tok_text(t);
        inc->fp = inc_fopen(p, deplist, &fhe,
                            (pp_mode == PP_DEPS) ? INC_OPTIONAL :
                            (op == PP_REQUIRE) ? INC_REQUIRED :
                            INC_NEEDED, NF_TEXT);
        if (!inc->fp) {
            /* -MG given but file not found, or repeated %require */
            nasm_free(inc);
        } else {
            inc->nolist  = istk->nolist;
            inc->noline  = istk->noline;
            inc->where   = istk->where;
            inc->lineinc = 0;
            istk = inc;
            if (!istk->noline) {
                src_set(0, fhe ? fhe->path : p);
                istk->where = src_where();
                istk->lineinc = 1;
                if (ppdbg & PDBG_INCLUDE)
                    dfmt->debug_include(true, istk->next->where, istk->where);
            }
            if (!istk->nolist)
                lfmt->uplevel(LIST_INCLUDE, 0);
        }
        delete_Token(t);
        break;
    }

    case PP_USE:
    {
        const struct use_package *pkg;
        const char *name;

        pkg = get_use_pkg(tline->next, dname, &name);
        if (!name)
            goto done;
        if (!pkg) {
            nasm_nonfatal("unknown `%s' package: `%s'", dname, name);
        } else if (!use_loaded[pkg->index]) {
            /*
             * Not already included, go ahead and include it.
             * Treat it as an include file for the purpose of
             * producing a listing.
             */
            use_loaded[pkg->index] = true;
            pp_start_stdmac();
            pp_add_stdmac(pkg->macros);
        }
        break;
    }
    case PP_PUSH:
    case PP_REPL:
    case PP_POP:
        tline = tline->next;
        tline = skip_white(tline);
        tline = expand_id(tline);
        if (tline) {
            if (!tok_is(tline, TOKEN_ID)) {
                nasm_nonfatal("`%s' expects a context identifier", dname);
                goto done;
            }
            if (skip_white(tline->next)) {
                nasm_warn(WARN_PP_TRAILING, "trailing garbage after `%s' ignored",
                           dname);
            }
            p = tok_text(tline);
        } else {
            p = NULL; /* Anonymous */
        }

        if (op == PP_PUSH) {
            nasm_new(ctx);
            ctx->depth = cstk ? cstk->depth + 1 : 1;
            ctx->next = cstk;
            ctx->name = p ? nasm_strdup(p) : NULL;
            ctx->number = unique++;
            cstk = ctx;
        } else {
            /* %pop or %repl */
            if (!cstk) {
                nasm_nonfatal("`%s': context stack is empty", dname);
            } else if (op == PP_POP) {
                if (p && (!cstk->name || nasm_stricmp(p, cstk->name)))
                    nasm_nonfatal("`%s' in wrong context: %s, "
                               "expected %s",
                               dname, cstk->name ? cstk->name : "anonymous", p);
                else
                    ctx_pop();
            } else {
                /* op == PP_REPL */
                nasm_free((char *)cstk->name);
                cstk->name = p ? nasm_strdup(p) : NULL;
                p = NULL;
            }
        }
        break;

    case PP_FATAL:
    case PP_ERROR:
    case PP_WARNING:
    case PP_NOTE:
        user_error(op, &tline->next);
        break;

    CASE_PP_IF:
        if (suppressed || (istk->conds && !emitting(istk->conds->state))) {
            /*
             * Don't evaluate the actual condition inside of a dead
             * expansion, because it is entirely valid for it to be
             * syntactically broken at this point. There is no point,
             * anyway: the output from all branches of this conditionals
             * will be suppressed, and the only reason this is being
             * processed at all is to look for nested conditionals.
             */
            j = COND_NEVER;
        } else {
            j = if_condition(tline->next, op, dname);
            tline->next = NULL; /* it got freed */
        }
        cond = nasm_malloc(sizeof(Cond));
        cond->next = istk->conds;
        cond->state = j;
        istk->conds = cond;
        if(istk->mstk.mstk)
            istk->mstk.mstk->condcnt++;
        break;

    CASE_PP_ELIF:
        if (!istk->conds) {
            nasm_nonfatal("`%s': no matching `%%if'", dname);
            break;
        }
        switch(istk->conds->state) {
        case COND_IF_TRUE:
            istk->conds->state = COND_DONE;
            break;

        case COND_DONE:
        case COND_NEVER:
            break;

        case COND_ELSE_TRUE:
        case COND_ELSE_FALSE:
            nasm_warn(WARN_PP_ELSE_ELIF|ERR_PP_PRECOND,
                       "`%s' after `%%else', ignoring content", dname);
            istk->conds->state = COND_NEVER;
            break;

        case COND_IF_FALSE:
            /*
             * IMPORTANT: In the case of %if, we will already have
             * called expand_mmac_params(); however, if we're
             * processing an %elif we must have been in a
             * non-emitting mode, which would have inhibited
             * the normal invocation of expand_mmac_params().
             * Therefore, we have to do it explicitly here.
             */
            j = if_condition(expand_mmac_params(tline->next), op, dname);
            tline->next = NULL; /* it got freed */
            istk->conds->state = j;
            break;
        }
        break;

    case PP_ELSE:
        if (tline->next)
            nasm_warn(WARN_PP_TRAILING|ERR_PP_PRECOND,
                      "trailing garbage after `%s' ignored", dname);
        if (!istk->conds) {
	    nasm_nonfatal("`%s': no matching `%%if'", dname);
            break;
        }
        switch(istk->conds->state) {
        case COND_IF_TRUE:
        case COND_DONE:
            istk->conds->state = COND_ELSE_FALSE;
            break;

        case COND_NEVER:
            break;

        case COND_IF_FALSE:
            istk->conds->state = COND_ELSE_TRUE;
            break;

        case COND_ELSE_TRUE:
        case COND_ELSE_FALSE:
            nasm_warn(WARN_PP_ELSE_ELSE|ERR_PP_PRECOND,
                      "`%s' after `%%else', ignoring content", dname);
            istk->conds->state = COND_NEVER;
            break;
        }
        break;

    case PP_ENDIF:
        if (tline->next) {
            nasm_warn(WARN_PP_TRAILING|ERR_PP_PRECOND,
                      "trailing garbage after `%s' ignored", dname);
        }
        if (!istk->conds) {
            nasm_nonfatal("`%s': no matching `%%if'", dname);
            break;
        }
        cond = istk->conds;
        istk->conds = cond->next;
        nasm_free(cond);
        if(istk->mstk.mstk)
            istk->mstk.mstk->condcnt--;
        break;

    case PP_RMACRO:
    {
        op = PP_MACRO;
        nasm_warn(WARN_PP_RESERVED,
                  "reserved directive `%s', treating as '%s'",
                  dname, pp_directives[op + !casesense]);
    }
    /* fall through */
    case PP_MACRO:
    {
        MMacro *def;

        nasm_assert(!defining);
        def = new_mmacro();
        def->casesense = casesense;

#if 0
        if (op == PP_RMACRO)
            def->max_depth = nasm_limit[LIMIT_MACRO_LEVELS];
#endif
        if (!parse_mmacro_spec(tline, def, dname)) {
            free_mmacro(def);
            goto done;
        }

        /*
         * dstk.mstk points to the previous definition bracket,
         * whereas dstk.mmac points to the topmost mmacro, which
         * in this case is the one we are just starting to create.
         */

        /* def->dstk.mstk = defining  == NULL */
        def->dstk.mmac = get_mmacro(def);
        defining = get_mmacro(def);
        defining->where = istk->where;

        mmac = (MMacro *) hash_findix(&mmacros, defining->name);
        while (mmac) {
            if (!strcmp(mmac->name, defining->name) &&
                (mmac->nparam_min <= defining->nparam_max
                 || defining->plus)
                && (defining->nparam_min <= mmac->nparam_max
                    || mmac->plus)) {
                nasm_warn(WARN_PP_MACRO_REDEF_MULTI,
                          "redefining multi-line macro `%s'",
                           defining->name);
                break;
            }
            mmac = mmac->next;
        }
        break;
    }

    case PP_ENDM:
    case PP_ENDMACRO:
        if (!defining) {
            nasm_nonfatal("`%s': not defining a macro", dname);
            goto done;
        } else if (!defining->name) {
            nasm_nonfatal("expected `%%endrep' before `%s'", dname);
            goto done;
        }

        pop_mstk(&defining->dstk, defining->dstk.mstk);

        if (defining->refcnt != 1)
            nasm_panic("defining->refcnt == %"PRIzu, defining->refcnt);

        mmhead = (MMacro **) hash_findi_add(&mmacros, defining->name);
        defining->next = *mmhead;
        *mmhead = defining;     /* Linked list inherits defining's refcnt */
        defining = NULL;
        break;

    case PP_EXITMACRO:
        do_exit_macro(dname, true);
        break;

    case PP_UNIMACRO:
        casesense = false;
        /* fall through */
    case PP_UNMACRO:
    {
        MMacro **mmac_p;
        MMacro spec;

        nasm_zero(spec);
        spec.casesense = casesense;
        if (!parse_mmacro_spec(tline, &spec, dname)) {
            goto done;
        }
        mmac_p = (MMacro **) hash_findi(&mmacros, spec.name, NULL);
        if (!mmac_p) {
            /* No such macro */
            delete_tlist(spec.dlist);
            break;
        }

        while (mmac_p && *mmac_p) {
            mmac = *mmac_p;
            if (mmac->casesense == spec.casesense &&
                !mstrcmp(mmac->name, spec.name, spec.casesense) &&
                mmac->nparam_min == spec.nparam_min &&
                mmac->nparam_max == spec.nparam_max &&
                mmac->plus == spec.plus) {
                *mmac_p = mmac->next;
            } else {
                mmac_p = &mmac->next;
            }
        }
        delete_tlist(spec.dlist);
        break;
    }

    case PP_ROTATE:
        while (tok_white(tline->next))
            tline = tline->next;
        if (!tline->next) {
            delete_tlist(origline);
            nasm_nonfatal("`%s' missing rotate count", dname);
            return DIRECTIVE_FOUND;
        }
        t = expand_smacro(tline->next);
        tline->next = NULL;
        pps.tptr = tline = t;
	pps.ntokens = -1;
        tokval.t_type = TOKEN_INVALID;
        evalresult =
            evaluate(ppscan, &pps, &tokval, NULL, true, NULL);
        delete_tlist(tline);
        if (!evalresult)
            return DIRECTIVE_FOUND;
        if (tokval.t_type) {
            nasm_warn(WARN_PP_TRAILING,
                      "trailing garbage after expression ignored");
        }
        if (!is_simple(evalresult)) {
            nasm_nonfatal("non-constant value given to `%s'", dname);
            return DIRECTIVE_FOUND;
        }
        mmac = istk->mstk.mmac;
        if (!mmac) {
            nasm_nonfatal("`%s' invoked outside a macro call", dname);
        } else if (mmac->nparam == 0) {
            nasm_nonfatal("`%s' invoked within macro without parameters", dname);
        } else {
            int rotate = mmac->rotate + reloc_value(evalresult);

            rotate %= (int)mmac->nparam;
            if (rotate < 0)
                rotate += mmac->nparam;

            mmac->rotate = rotate;
        }
        break;

    case PP_REP:
    {
        MMacro *def;

        nolist = 0;
        tline = skip_white(tline->next);
        if (tok_is(tline, TOKEN_ID) && tline->len == 7 &&
	    !nasm_memicmp(tline->text.a, ".nolist", 7)) {
            if (!list_option('f'))
                nolist |= NL_LIST; /* ... but update line numbers */
            tline = skip_white(tline->next);
        }

        if (tline) {
            pps.tptr = expand_smacro(tline);
	    pps.ntokens = -1;
            tokval.t_type = TOKEN_INVALID;
            /* XXX: really critical?! */
            evalresult =
                evaluate(ppscan, &pps, &tokval, NULL, true, NULL);
            if (!evalresult)
                goto done;
            if (tokval.t_type)
                nasm_warn(WARN_PP_TRAILING, "trailing garbage after expression ignored");
            if (!is_simple(evalresult)) {
                nasm_nonfatal("non-constant value given to `%s'", dname);
                goto done;
            }
            count = reloc_value(evalresult);
            if (count > nasm_limit[LIMIT_REP]) {
                nasm_nonfatal("`%s' count %"PRId64" exceeds limit (currently %"PRId64")",
                              dname, count, nasm_limit[LIMIT_REP]);
                count = 0;
            } else if (count < 0) {
                nasm_warn(ERR_PASS2|WARN_PP_REP_NEGATIVE,
                          "negative `%s' count: %"PRId64, dname, count);
                count = 0;
            } else {
                count++;
            }
        } else {
            nasm_nonfatal("`%s' expects a repeat count", dname);
            count = 0;
        }
        def = new_mmacro();
        def->nolist = nolist;
        def->in_progress = count;
        def->mstk.mstk = get_mmacro(istk->mstk.mstk);
        def->mstk.mmac = get_mmacro(istk->mstk.mmac);
        def->dstk.mstk = defining; /* Inherits defining's refcount */
        def->dstk.mmac = defining ? get_mmacro(defining->dstk.mmac) : NULL;
        def->where = istk->where;
        defining = get_mmacro(def);
        break;
    }

    case PP_ENDREP:
    {
        Line *l;

        if (!defining || defining->name) {
            nasm_nonfatal("`%%endrep': no matching `%%rep'");
            goto done;
        }

        /*
         * Now we have a "macro" defined - although it has no name
         * and we won't be entering it in the hash tables - we must
         * push a macro-end marker for it on to istk->expansion.
         * After that, it will take care of propagating itself (a
         * macro-end marker line for a macro which is really a %rep
         * block will cause the macro to be re-expanded, complete
         * with another macro-end marker to ensure the process
         * continues) until the whole expansion is forcibly removed
         * from istk->expansion by a %exitrep.
         */
        nasm_new(l);
        l->next = istk->expansion;
        l->finishes = get_mmacro(defining);
        l->first = NULL;
        l->where = src_where();
        istk->expansion = l;

        /* A loop does not change istk->noline */
        istk->nolist += !!(defining->nolist & NL_LIST);
        if (!istk->nolist)
            lfmt->uplevel(LIST_MACRO, 0);

        put_mmacro(&defining->dstk.mmac);

        /* These inherit the respective refcounts */
        pop_mmacro(&istk->mstk.mstk, defining);
        defining = defining->dstk.mstk;
        break;
    }

    case PP_EXITREP:
    {
        MMacro *m = do_exit_macro(dname, false);
        if (m)
            m->in_progress = 1;     /* No more repeats */
        break;
    }

    case PP_DEFINE:
    case PP_XDEFINE:
    case PP_DEFALIAS:
    {
        SMacro tmpl;
        Token **lastp;
        int nparam;

        if (!(mname = get_id(&tline, dname)))
            goto done;

        nasm_zero(tmpl);
        lastp = &tline->next;
        nparam = parse_smacro_template(&lastp, &tmpl);
        tline = *lastp;
        *lastp = NULL;

        if (unlikely(op == PP_DEFALIAS)) {
            macro_start = tline;
            if (!tok_macro_id(macro_start)) {
                nasm_nonfatal("`%s' expects a macro identifier to alias",
                              dname);
                goto done;
            }
            tt = macro_start->next;
            macro_start->next = NULL;
            tline = tline->next;
            tline = skip_white(tline);
            if (tline && tline->type) {
                nasm_warn(WARN_PP_TRAILING,
                          "trailing garbage after aliasing identifier ignored");
            }
            delete_tlist(tt);
            tmpl.alias = true;
        } else {
            if (op == PP_XDEFINE) {
                /* Protect macro parameter tokens */
                if (nparam)
                    mark_smac_params(tline, &tmpl, TOKEN_XDEF_PARAM);
                tline = expand_smacro(tline);
            }
            macro_start = tline;
        }

        /*
         * Good. We now have a macro name, a parameter count, and a
         * token list (in reverse order) for an expansion. We ought
         * to be OK just to create an SMacro, store it, and let
         * delete_tlist have the rest of the line (which we have
         * carefully re-terminated after chopping off the expansion
         * from the end).
         */
        define_smacro(mname, casesense, macro_start, &tmpl);
        break;
    }

    case PP_UNDEF:
    case PP_UNDEFALIAS:
        if (!(mname = get_id(&tline, dname)))
            goto done;
        if (tline->next)
            nasm_warn(WARN_PP_TRAILING,
                      "trailing garbage after macro name ignored");

        undef_smacro(mname, op == PP_UNDEFALIAS);
        break;

    case PP_DEFSTR:
        if (!(mname = get_id(&tline, dname)))
            goto done;

        tline = expand_smacro(cut_tlist(tline));

        tline = zap_white(tline);
        q = detoken(tline, false);
        macro_start = make_tok_qstr(NULL, q);
        nasm_free(q);

        /*
         * We now have a macro name, an implicit parameter count of
         * zero, and a string token to use as an expansion. Create
         * and store an SMacro.
         */
        define_smacro(mname, casesense, macro_start, NULL);
        break;

    case PP_DEFTOK:
        if (!(mname = get_id(&tline, dname)))
            goto done;

        tline = expand_smacro(cut_tlist(tline));

        t = skip_white(tline);
        /* t should now point to the string */
        if (!tok_is(t, TOKEN_STR)) {
            nasm_nonfatal("`%s' requires string as second parameter", dname);
            delete_tlist(tline);
            goto done;
        }

        /*
         * Convert the string to a token stream.
         */
        macro_start = tokenize(unquote_token_cstr(t));

        /*
         * We now have a macro name, an implicit parameter count of
         * zero, and a numeric token to use as an expansion. Create
         * and store an SMacro.
         */
        define_smacro(mname, casesense, macro_start, NULL);
        delete_tlist(tline);
        break;

    case PP_PATHSEARCH:
    {
        if (!(mname = get_id(&tline, dname)))
            goto done;

        macro_start = pp_do_pathsearch(&tline->next, dname);

        /*
         * We now have a macro name, an implicit parameter count of
         * zero, and a string token to use as an expansion. Create
         * and store an SMacro.
         */
        if (macro_start)
            define_smacro(mname, casesense, macro_start, NULL);

        break;
    }

    case PP_STRLEN:
        if (!(mname = get_id(&tline, dname)))
            goto done;

        tline = expand_smacro(cut_tlist(tline));

        t = skip_white(tline);
        /* t should now point to the string */
        if (!tok_is(t, TOKEN_STR)) {
            nasm_nonfatal("`%s' requires string as second parameter", dname);
            delete_tlist(tline);
            delete_tlist(origline);
            return DIRECTIVE_FOUND;
        }

	unquote_token(t);
        macro_start = make_tok_num(NULL, t->len);

        /*
         * We now have a macro name, an implicit parameter count of
         * zero, and a numeric token to use as an expansion. Create
         * and store an SMacro.
         */
        define_smacro(mname, casesense, macro_start, NULL);
        delete_tlist(tline);
        delete_tlist(origline);
        return DIRECTIVE_FOUND;

    case PP_STRCAT:
        if (!(mname = get_id(&tline, dname)))
            goto done;

        tline = expand_smacro(cut_tlist(tline));

        macro_start = pp_strcat(tline, dname);
        /*
         * We now have a macro name, an implicit parameter count of
         * zero, and a string token to use as an expansion. Create
         * and store an SMacro.
         */
        if (macro_start)
            define_smacro(mname, casesense, macro_start, NULL);
        break;

    case PP_SUBSTR:
        if (!(mname = get_id(&tline, dname)))
            goto done;

        tline = expand_smacro(cut_tlist(tline));

        macro_start = pp_substr(tline, dname);
        /*
         * We now have a macro name, an implicit parameter count of
         * zero, and a string token to use as an expansion. Create
         * and store an SMacro.
         */
        if (macro_start)
            define_smacro(mname, casesense, macro_start, NULL);
        break;

    case PP_ASSIGN:
        if (!(mname = get_id(&tline, dname)))
            goto done;

        tline = cut_tlist(tline);
        assign_smacro(mname, casesense, tline, dname);
        goto done;

    case PP_ALIASES:
        tline = tline->next;
        tline = expand_smacro(tline);
        ppconf.noaliases = !pp_get_boolean_option(tline, !ppconf.noaliases);
        break;

    case PP_LINE:
        nasm_panic("`%s' directive not preprocessed early", dname);
        break;

    case PP_NULL:
        /* Goes nowhere, does nothing */
        break;
    }

done:
    delete_tlist(origline);
    return DIRECTIVE_FOUND;
}

/*
 * Ensure that a macro parameter contains a condition code and
 * nothing else. Return the condition code index if so, or -1
 * otherwise.
 */
static int find_cc(Token * t)
{
    Token *tt;

    if (!t)
        return -1;              /* Probably a %+ without a space */

    t = skip_white(t);
    if (!tok_is(t, TOKEN_ID))
        return -1;
    tt = t->next;
    tt = skip_white(tt);
    if (tok_isnt(tt, ','))
        return -1;

    return bsii(tok_text(t), (const char **)conditions,
		ARRAY_SIZE(conditions));
}

enum concat_flags {
    CONCAT_ID             = 0x01,
    CONCAT_LOCAL_MACRO    = 0x02,
    CONCAT_ENVIRON        = 0x04,
    CONCAT_PREPROC_ID     = 0x08,
    CONCAT_NUM            = 0x10,
    CONCAT_FLOAT          = 0x20,
    CONCAT_OP             = 0x40  /* Operators */
};

struct concat_mask {
    enum concat_flags mask_head;
    enum concat_flags mask_tail;
};


static inline bool pp_concat_match(const Token *t, enum concat_flags mask)
{
    enum concat_flags ctype = 0;

    if (!t)
        return false;

    switch (t->type) {
    case TOKEN_ID:
    case TOKEN_QMARK:           /* Keyword, treated as ID for pasting */
        ctype = CONCAT_ID;
        break;
    case TOKEN_LOCAL_MACRO:
        ctype = CONCAT_LOCAL_MACRO;
        break;
    case TOKEN_ENVIRON:
        ctype = CONCAT_ENVIRON;
        break;
    case TOKEN_PREPROC_ID:
        ctype = CONCAT_PREPROC_ID;
        break;
    case TOKEN_NUM:
    case TOKEN_FLOAT:
        ctype = CONCAT_NUM;
        break;
    case TOKEN_HERE:
    case TOKEN_BASE:
        /* NASM 2.15 treats these as operators, but is that sane? */
        ctype = CONCAT_OP;
        break;
    case TOKEN_OTHER:
        ctype = CONCAT_OP;      /* For historical reasons */
        break;
    default:
        if (t->type > TOKEN_WHITESPACE && t->type < TOKEN_MAX_OPERATOR)
            ctype = CONCAT_OP;
        else
            ctype = 0;
    }

    return !!(ctype & mask);
}

/*
 * This routines walks over tokens stream and handles tokens
 * pasting, if @handle_explicit passed then explicit pasting
 * term is handled, otherwise -- implicit pastings only.
 * The @m array can contain a series of token types which are
 * executed as separate passes.
 */
static bool paste_tokens(Token **head, const struct concat_mask *m,
                         size_t mnum, bool handle_explicit)
{
    Token *tok, *t, *next, **prev_next, **prev_nonspace, **nextp;
    bool pasted = false;
    char *buf, *p;
    size_t len, i;

    /*
     * The last token before pasting. We need it
     * to be able to connect new handled tokens.
     * In other words if there were a tokens stream
     *
     * A -> B -> C -> D
     *
     * and we've joined tokens B and C, the resulting
     * stream should be
     *
     * A -> BC -> D
     */
    tok = *head;
    prev_next = prev_nonspace = head;

    if (tok_white(tok) || tok_is(tok, TOKEN_PASTE))
        prev_nonspace = NULL;

    while (tok && (next = tok->next)) {
        bool did_paste = false;

        switch (tok->type) {
        case TOKEN_WHITESPACE:
            /* Zap redundant whitespaces */
            tok->next = next = zap_white(next);
            break;

        case TOKEN_PASTE:
            /* Explicit pasting */
            if (!handle_explicit)
                break;

            did_paste = true;

            /* Left pasting token is start of line, just drop %+ */
            if (!prev_nonspace) {
                prev_next = nextp = head;
                t = NULL;
            } else {
                prev_next = prev_nonspace;
                t = *prev_next;
                nextp = &t->next;
            }

            /*
             * Delete the %+ token itself plus any whitespace.
             * In a sequence of %+ ... %+ ... %+ pasting sequences where
             * some expansions in the middle have ended up empty,
             * we can end up having multiple %+ tokens in a row;
             * just drop whem in that case.
             */
            next = *nextp;
            while (next) {
                if (next->type == TOKEN_PASTE || next->type == TOKEN_WHITESPACE)
                    next = delete_Token(next);
                else
                    break;
            }
            *nextp = next;

            /*
             * Nothing after? Just leave the existing token.
             */
            if (!next)
                break;

            if (!t) {
                /* Nothing to actually paste, just zapping the paste */
                *prev_next = tok = next;
                break;
            }

            /* An actual paste */
            p = buf = nasm_malloc(t->len + next->len + 1);
            p = mempcpy(p, tok_text(t), t->len);
            p = mempcpy(p, tok_text(next), next->len);
            *p = '\0';
            delete_Token(t);
            t = tokenize(buf);
            nasm_free(buf);

            if (unlikely(!t)) {
                /*
                 * No output at all? Replace with a single whitespace.
                 * This should never happen.
                 */
                tok = t = new_White(NULL);
            } else {
                *prev_nonspace = tok = t;
            }
            while (t->next)
                t = t->next;    /* Find the last token produced */

            /* Delete the second token and attach to the end of the list */
            t->next = delete_Token(next);

            /* We want to restart from the head of the pasted token */
            *prev_next = next = tok;
            break;

        default:
            /* implicit pasting */
            for (i = 0; i < mnum; i++) {
                if (pp_concat_match(tok, m[i].mask_head))
                    break;
            }

            if (i >= mnum)
                break;

            len =  tok->len;
            while (pp_concat_match(next, m[i].mask_tail)) {
                len += next->len;
                next = next->next;
            }

            /* No match or no text to process */
            if (len == tok->len)
                break;

            p = buf = nasm_malloc(len + 1);
            while (tok != next) {
                p = mempcpy(p, tok_text(tok), tok->len);
                tok = delete_Token(tok);
            }
            *p = '\0';
            *prev_next = tok = t = tokenize(buf);
            nasm_free(buf);

            /*
             * Connect pasted into original stream,
             * ie A -> new-tokens -> B
             */
            while ((tok = t->next)) {
                if (tok->type != TOKEN_WHITESPACE && tok->type != TOKEN_PASTE)
                    prev_nonspace = &t->next;
                t = tok;
            }

            t->next = next;
            prev_next = &t->next;
            did_paste = true;
            break;
        }

        if (did_paste) {
            pasted = true;
        } else {
            prev_next = &tok->next;
            if (next && next->type != TOKEN_WHITESPACE &&
                next->type != TOKEN_PASTE)
                prev_nonspace = prev_next;
        }
        tok = next;
    }

    return pasted;
}

/*
 * Computes the proper rotation of mmacro parameters
 */
static int mmac_rotate(const MMacro *mac, unsigned int n)
{
    if (--n < mac->nparam)
        n = (n + mac->rotate) % mac->nparam;

    return n+1;
}

/*
 * expands to a list of tokens from %{x:y}
 */
static void expand_mmac_params_range(MMacro *mac, Token *tline, Token ***tail)
{
    Token *t;
    const char *arg = tok_text(tline) + 1;
    int fst, lst, incr, n;
    int parsed;

    parsed = sscanf(arg, "%d:%d", &fst, &lst);
    nasm_assert(parsed == 2);

    /*
     * only macros params are accounted so
     * if someone passes %0 -- we reject such
     * value(s)
     */
    if (lst == 0 || fst == 0)
        goto err;

    /* the values should be sane */
    if ((fst > (int)mac->nparam || fst < (-(int)mac->nparam)) ||
        (lst > (int)mac->nparam || lst < (-(int)mac->nparam)))
        goto err;

    fst = fst < 0 ? fst + (int)mac->nparam + 1: fst;
    lst = lst < 0 ? lst + (int)mac->nparam + 1: lst;

    /*
     * It will be at least one parameter, as we can loop
     * in either direction.
     */
    incr = (fst < lst) ? 1 : -1;

    while (true) {
        n = mmac_rotate(mac, fst);
        dup_tlistn(mac->params[n], mac->paramlen[n], tail);
        if (fst == lst)
            break;
        t = make_tok_char(NULL, ',');
        **tail = t;
        *tail = &t->next;
        fst += incr;
    }

    return;

err:
    nasm_nonfatal("`%%{%s}': macro parameters out of range", arg);
    return;
}

/*
 * Expand MMacro-local things: parameter references (%0, %n, %+n,
 * %-n) and MMacro-local identifiers (%%foo) as well as
 * macro indirection (%[...]) and range (%{..:..}).
 */
static Token *expand_mmac_params(Token * tline)
{
    Token **tail, *thead;
    bool changed = false;
    MMacro * const mac = istk->mstk.mmac;

    tail = &thead;
    thead = NULL;

    while (tline) {
        bool change;
        bool err_not_mac = false;
        Token *t = tline;
        const char *text = tok_text(t);
        char *newtext = NULL;
        int type = t->type;

        tline = tline->next;
        t->next = NULL;

        switch (type) {
        case TOKEN_LOCAL_SYMBOL:
            change = true;

            if (!mac) {
                err_not_mac = true;
                break;
            }

            type = TOKEN_ID;
            text = newtext =
                nasm_asprintf("..@%"PRIu64".%s", mac->unique, text+2);
            break;
        case TOKEN_MMACRO_PARAM:
        {
            Token *tt = NULL;

            change = true;

            if (!mac) {
                err_not_mac = true;
                break;
            }

            if (strchr(text, ':')) {
                /* It is a range */
                expand_mmac_params_range(mac, t, &tail);
                text = NULL;
                break;
            }

            switch (text[1]) {
                /*
                 * We have to make a substitution of one of the
                 * forms %1, %-1, %+1, %%foo, %0, %00.
                 */
            case '0':
                if (!text[2]) {
                    type = TOKEN_NUM;
                    text = newtext = nasm_asprintf("%d", mac->nparam);
                    break;
                }
                if (text[2] != '0' || text[3])
                    goto invalid;
                /* a possible captured label == mac->params[0] */
                /* fall through */
            default:
            {
                unsigned long n;
                char *ep;

                n = strtoul(text + 1, &ep, 10);
                if (unlikely(*ep))
                    goto invalid;

                if (n <= mac->nparam) {
                    n = mmac_rotate(mac, n);
                    dup_tlistn(mac->params[n], mac->paramlen[n], &tail);
                }
                text = NULL;
                break;
            }
            case '-':
            case '+':
            {
                int cc;
                unsigned long n;
                char *ep;

                n = strtoul(tok_text(t) + 2, &ep, 10);
                if (unlikely(*ep))
                    goto invalid;

                if (n && n <= mac->nparam) {
                    n = mmac_rotate(mac, n);
                    tt = mac->params[n];
                }
                cc = find_cc(tt);
                if (cc == -1) {
                    nasm_nonfatal("macro parameter `%s' is not a condition code",
                                  tok_text(t));
                    text = NULL;
                    break;
                }

                type = TOKEN_ID;
                if (text[1] == '-') {
                    int ncc = inverse_ccs[cc];
                    if (unlikely(ncc == -1)) {
                        nasm_nonfatal("condition code `%s' is not invertible",
                                      conditions[cc]);
                        break;
                    }
                    cc = ncc;
                }
                text = conditions[cc];
                break;
            }

            invalid:
                nasm_nonfatal("invalid macro parameter: `%s'", text);
                text = NULL;
                break;
            }
            break;
        }

        case TOKEN_PREPROC_Q:
            if (mac) {
                type = TOKEN_ID;
                text = mac->iname;
                change = true;
            } else {
                change = false;
            }
            break;

        case TOKEN_PREPROC_QQ:
            if (mac) {
                type = TOKEN_ID;
                text = mac->name;
                change = true;
            } else {
                change = false;
            }
            break;

        case TOKEN_INDIRECT:
        {
            Token *tt;

            tt = tokenize(tok_text(t));
            tt = expand_mmac_params(tt);
            tt = expand_smacro(tt);
            tail = steal_tlist(tt, tail);
            text = NULL;
            change = true;
            break;
        }

        default:
            change = false;
            break;
        }

        if (err_not_mac) {
            nasm_nonfatal("`%s': not in a macro call", text);
            text = NULL;
            change = true;
        }

        if (change) {
            if (!text) {
                delete_Token(t);
                nasm_free(newtext);
            } else {
                size_t len = tok_strlen(text);
                *tail = t;
                tail = &t->next;
                t->type = type;
                if (!newtext)
                    set_text(t, text, len);
                else
                    set_text_free(t, newtext, len);
            }
            changed = true;
        } else {
            *tail = t;
            tail = &t->next;
        }
    }

    *tail = NULL;

    if (changed) {
        const struct concat_mask t[] = {
            {
                CONCAT_ID | CONCAT_FLOAT,   /* head */
                CONCAT_ID | CONCAT_NUM | CONCAT_FLOAT | CONCAT_OP /* tail */
            },
            {
                CONCAT_NUM,     /* head */
                CONCAT_NUM      /* tail */
            }
        };
        paste_tokens(&thead, t, ARRAY_SIZE(t), false);
    }

    return thead;
}

static SMacro *expand_one_smacro(Token ***tpp);

/*
 * Process an smacro argument with SPARM_STR.
 * This is factored out so that it is usable by preprocessor functions.
 */
static Token *
expand_sparm_str(Token *param, enum sparmflags flags)
{
    Token *qs;

    qs = expand_smacro_noreset(param);
    if ((flags & SPARM_CONDQUOTE) && tok_is(qs, TOKEN_STR) && !qs->next) {
        /* A single quoted string token - already good */
    } else {
        char *arg = detoken(qs, false);
        delete_tlist(qs);
        qs = make_tok_qstr(NULL, arg);
        nasm_free(arg);
    }
    return qs;
}

/*
 * Expand one single-line macro instance given a specific macro and a
 * specific set of parameters. Returns a pointer to the expansion, and
 * the pointer *epp pointing to the next pointer of the last token of
 * the expansion; if the expansion is empty return NULL and *epp is
 * unchanged.
 *
 * mstart is the token containing the token name *as invoked*.
 */
static Token *
expand_smacro_with_params(SMacro *m, Token *mstart, Token **params,
                          int nparam, Token ***epp)
{
    /* Is it a macro or a preprocessor function? Used for diagnostics. */
    const char * const mtype = m->name[0] == '%' ? "function" : "macro";
    Token *t, *tline, *tup;
    bool cond_comma;
    const struct smac_param *mparm;
    int i;

    /* Expand the macro */
    m->in_progress++;

    /*
     * Postprocessing of of parameters. Note that the ordering matters
     * here.
     *
     * mparm points to the current parameter specification
     * structure (struct smac_param); this may not match the index
     * i in the case of varadic parameters.
     */
    if (nparam) {
        for (i = 0, mparm = m->params; i < nparam;
             i++, mparm += !(mparm->flags & SPARM_VARADIC)) {
            const enum sparmflags flags = mparm->flags;

            if (flags & SPARM_EVAL) {
                /* Evaluate this parameter as a number */
                struct ppscan pps;
                struct tokenval tokval;
                expr *evalresult;
                Token *eval_param;

                eval_param = zap_white(expand_smacro_noreset(params[i]));
                params[i] = NULL;

                if (!eval_param) {
                    /* empty argument */
                    if (mparm->def) {
                        params[i] = dup_tlist(mparm->def, NULL);
                        continue;
                    } else if (flags & SPARM_OPTIONAL) {
                        continue;
                    }
                    /* otherwise, allow evaluate() to generate an error */
                }

                pps.tptr = eval_param;
                pps.ntokens = -1;
                tokval.t_type = TOKEN_INVALID;
                evalresult = evaluate(ppscan, &pps, &tokval, NULL, true, NULL);

                delete_tlist(eval_param);

                if (!evalresult) {
                    /* Nothing meaningful to do */
                } else if (tokval.t_type) {
                    nasm_nonfatal("invalid expression in parameter %d of %s `%s'",
                                  i+1, mtype, m->name);
                } else if (!is_simple(evalresult)) {
                    nasm_nonfatal("non-constant expression in parameter %d of %s `%s'",
                                  i+1, mtype, m->name);
                } else {
                    int64_t v = reloc_value(evalresult);
                    params[i] = make_tok_num_radix(NULL, v, mparm->radix,
                                                   !!(flags & SPARM_UNSIGNED));
                }
            }

            if (flags & SPARM_STR) {
                /* Convert expansion to a quoted string */
                params[i] = expand_sparm_str(params[i], flags);
            }
        }
    }


    /* Note: we own the expansion this returns. */
    t = m->expand(m, params, nparam);

    tup = tline = NULL;
    cond_comma = false;

    while (t) {
        enum token_type type = t->type;
        Token *tnext = t->next;

        switch (type) {
        case TOKEN_PREPROC_Q:
        case TOKEN_PREPROC_SQ:
            nasm_assert(t != mstart);
            delete_Token(t);
            t = dup_Token(tline, mstart);
            break;

        case TOKEN_PREPROC_QQ:
        case TOKEN_PREPROC_SQQ:
        {
            size_t mlen = strlen(m->name);
	    size_t len;
            char *p, *from;

            t->type = mstart->type;
            if (t->type == TOKEN_LOCAL_MACRO) {
		const char *psp; /* prefix start pointer */
                const char *pep; /* prefix end pointer */
		size_t plen;

		psp = tok_text(mstart);
                get_ctx(psp, &pep);
                plen = pep - psp;

                len = mlen + plen;
                from = p = nasm_malloc(len + 1);
                p = mempcpy(p, psp, plen);
            } else {
                len = mlen;
                from = p = nasm_malloc(len + 1);
            }
            p = mempcpy(p, m->name, mlen+1);
	    set_text_free(t, from, len);

            t->next = tline;
            break;
        }

        case TOKEN_COND_COMMA:
            delete_Token(t);
            t = cond_comma ? make_tok_char(tline, ',') : NULL;
            break;

        case TOKEN_ID:
        case TOKEN_PREPROC_ID:
	case TOKEN_LOCAL_MACRO:
        {
            /*
             * Chain this into the target line *before* expanding,
             * that way we pick up any arguments to the new macro call,
             * if applicable.
             */
            Token **tp = &t;
            t->next = tline;
            expand_one_smacro(&tp);
            tline = *tp;        /* First token left after any macro call */
            break;
        }
        default:
            if (is_smac_param(t->type)) {
                int param = smac_nparam(t->type);
                nasm_assert(!tup && param < nparam);
                delete_Token(t);
                t = NULL;
                tup = tnext;
                tnext = dup_tlist_reverse(params[param], NULL);
                cond_comma = false;
            } else {
                t->next = tline;
            }
        }

        if (t) {
            Token *endt = tline;

            tline = t;
            while (!cond_comma && t && t != endt) {
                cond_comma = t->type != TOKEN_WHITESPACE;
                t = t->next;
            }
        }

        if (tnext) {
            t = tnext;
        } else {
            t = tup;
            tup = NULL;
        }
    }

    if (epp) {
        Token **ep = *epp;
        for (t = tline; t; t = t->next)
            ep = &t->next;
        *epp = ep;
    }

    /* Expansion complete */
    m->in_progress--;

    return tline;
}

/*
 * Count the arguments to an smacro call. Returns 0 if the token following
 * is not a left paren. *tp is set to point to the final ) if non-NULL;
 * it is left unchanged for the zero-argument case.
 */
static int count_smacro_args(Token *t, Token **tp)
{
    int nparam;
    int paren, brackets;

    t = skip_white(t);

    if (!tok_is(t, '('))
        return 0;

    paren = 1;
    nparam = 1;
    brackets = 0;

    while (paren) {
        t = t->next;

        if (!t) {
            nasm_nonfatal("macro call expects terminating `)'");
            return 0;
        }

        switch (t->type) {
        case ',':
            if (!brackets && paren == 1)
                nparam++;
            break;

        case '{':
            brackets++;
            break;

        case '}':
            if (brackets > 0)
                brackets--;
            break;

        case '(':
            if (!brackets)
                paren++;
            break;

        case ')':
            if (!brackets)
                paren--;
            break;

        default:
            break;          /* Normal token */
        }
    }

    if (tp)
        *tp = t;
    return nparam;
}

/*
 * Collect the arguments to an smacro call. The size of the array must have
 * been previously counted. It *is* permitted to call this with an nparam
 * value that is too small for the macro in question; in that case the
 * parameters are treated as missing optional arguments, even if they
 * are not optional in the macro specification.
 *
 * *nparamp is adjusted if some arguments got merged as greedy or entered
 * as optional/empty.
 *
 * Moves *tp to point to the final ) token.
 */
static Token **parse_smacro_args(Token **tp, int *nparamp, const SMacro *m)
{
    Token **phead, **pep;
    int white = 0;
    int brackets = 0;
    int paren;
    bool bracketed = false;
    int i;
    enum sparmflags flags;
    const struct smac_param *mparm;
    Token *t = *tp;
    int nparam = *nparamp;
    Token **params;
    /* Is it a macro or a preprocessor function? Used for diagnostics. */
    const char * const mtype = m->name[0] == '%' ? "function" : "macro";

    t = skip_white(t);
    nasm_assert(tok_is(t, '('));

    if (nparam > m->nparam) {
        if (m->params[m->nparam-1].flags & SPARM_GREEDY)
            *nparamp = nparam = m->nparam;
    } else if (nparam < m->nparam) {
        *nparamp = nparam = m->nparam; /* Missing optional arguments = empty */
    }
    paren = 1;
    nasm_newn(params, nparam);
    i = 0;
    mparm = m->params;
    flags = mparm->flags;
    phead = pep = &params[i];
    *pep = NULL;

    while (paren) {
        bool skip;

        t = t->next;

        if (!t)
            nasm_nonfatal("%s `%s' call expects terminating `)'",
                          mtype, m->name);

        skip = false;

        switch (t->type) {
        case TOKEN_WHITESPACE:
            if (!(flags & SPARM_NOSTRIP)) {
                if (brackets || *phead)
                    white++;    /* Keep interior whitespace */
                skip = true;
            }
            break;

        case ',':
            if (!brackets && paren == 1 && !(flags & SPARM_GREEDY)) {
                i++;
                nasm_assert(i < nparam);
                phead = pep = &params[i];
                *pep = NULL;
                bracketed = false;
                skip = true;
                if (!(flags & SPARM_VARADIC)) {
                    mparm++;
                    flags = mparm->flags;
                }
            }
            break;

        case '{':
            if (!bracketed) {
                bracketed = !*phead && !(flags & SPARM_NOSTRIP);
                skip = bracketed;
            }
            brackets++;
            break;

        case '}':
            if (brackets > 0) {
                if (!--brackets)
                    skip = bracketed;
            }
            break;

        case '(':
            if (!brackets)
                paren++;
            break;

        case ')':
            if (!brackets) {
                paren--;
                if (!paren) {
                    skip = true;
                    i++;    /* Found last argument */
                }
            }
            break;

        default:
            break;          /* Normal token */
        }

        if (!skip) {
            Token *tt;

            if (white) {
                *pep = tt = new_White(NULL);
                pep = &tt->next;
                white = 0;
            }
            *pep = tt = dup_Token(NULL, t);
            pep = &tt->next;
        }
    }

    *tp = t;
    return params;
}

/*
 * Expand *one* single-line macro instance. If the first token is not
 * a macro at all, it is simply copied to the output and the pointer
 * advanced.  tpp should be a pointer to a pointer (usually the next
 * pointer of the previous token) to the first token. **tpp is updated
 * to point to the first token of the expansion, and *tpp updated to
 * point to the next pointer of the last token of the expansion.
 *
 * If the expansion is empty, *tpp will be unchanged but **tpp will
 * be advanced past the macro call.
 *
 * Return the macro expanded, or NULL if no expansion took place.
 */
static SMacro *expand_one_smacro(Token ***tpp)
{
    Token **params = NULL;
    const char *mname;
    Token *mstart = **tpp;
    Token *tline  = mstart;
    SMacro *head, *m;
    Token *tafter, **tep;
    int nparam = 0;

    if (!tline)
        return NULL;            /* Empty line, nothing to do */

    mname = tok_text(mstart);

    smacro_deadman.total--;
    smacro_deadman.levels--;

    if (unlikely(smacro_deadman.total < 0 || smacro_deadman.levels < 0)) {
        if (unlikely(!smacro_deadman.triggered)) {
            nasm_nonfatal("interminable macro recursion");
            smacro_deadman.triggered = true;
        }
        goto not_a_macro;
    } else if (tline->type == TOKEN_ID || tline->type == TOKEN_PREPROC_ID) {
        head = (SMacro *)hash_findix(&smacros, mname);
    } else if (tline->type == TOKEN_LOCAL_MACRO) {
        Context *ctx = get_ctx(mname, &mname);
        head = ctx ? (SMacro *)hash_findix(&ctx->localmac, mname) : NULL;
    } else {
        goto not_a_macro;
    }

    /*
     * We've hit an identifier of some sort. First check whether the
     * identifier is a single-line macro at all, then think about
     * checking for parameters if necessary.
     */
    list_for_each(m, head) {
        if (unlikely(m->alias && ppconf.noaliases))
            continue;
        if (!mstrcmp(m->name, mname, m->casesense))
            break;
    }

    if (!m) {
        goto not_a_macro;
    }

    /* Parse parameters, if applicable */

    params = NULL;
    nparam = 0;

    if (m->nparam == 0) {
        /*
         * Simple case: the macro is parameterless.
         * Nothing to parse; the expansion code will
         * drop the macro name token.
         */
    } else {
        /*
         * Complicated case: at least one macro with this name
         * exists and takes parameters. We must find the
         * parameters in the call, count them, find the SMacro
         * that corresponds to that form of the macro call, and
         * substitute for the parameters when we expand. What a
         * pain.
         */
        tline = skip_white(tline->next);
        nparam = count_smacro_args(tline, NULL);
        if (!nparam)
            goto not_a_macro;

        /*
         * Look for a macro matching in both name and parameter count.
         * We already know any matches cannot be anywhere before the
         * current position of "m", so there is no reason to
         * backtrack.
         */
        while (1) {
            if (!m) {
                nasm_warn(WARN_PP_MACRO_PARAMS_SINGLE,
                    "single-line macro `%s' exists, "
                    "but not taking %d parameter%s",
                    mname, nparam, (nparam == 1) ? "" : "s");
                goto not_a_macro;
            }

            if (!mstrcmp(m->name, mname, m->casesense)) {
                if (nparam >= m->nparam_min &&
                    (m->varadic || nparam <= m->nparam))
                    break;      /* It's good */
            }
            m = m->next;
        }
    }

    if (m->in_progress && !m->recursive)
        goto not_a_macro;

   if (nparam) {
        params = parse_smacro_args(&tline, &nparam, m);
    }

    tafter = tline->next;   /* Skip past the macro call */
    tline->next = NULL;     /* Truncate mstart list at the macro call end */
    tline = expand_smacro_with_params(m, mstart, params, nparam, &tep);
    if (tline) {
        **tpp = tline;
        *tep = tafter;
        *tpp = tep;
    } else {
        **tpp = tafter;
    }

    /* Don't do this until after expansion or we will clobber mname */
    delete_tlist(mstart);
    goto done;

    /*
     * No macro expansion needed; roll back to mstart (if necessary)
     * and then advance to the next input token. Note that this is
     * by far the common case!
     */
not_a_macro:
    *tpp = &mstart->next;
    m = NULL;
done:
    delete_tlist_array(params, nparam);
    smacro_deadman.levels++;
    return m;
}

/*
 * Expand all single-line macro calls made in the given line.
 * Return the expanded version of the line. The original is deemed
 * to be destroyed in the process. (In reality we'll just move
 * Tokens from input to output a lot of the time, rather than
 * actually bothering to destroy and replicate.)
 */
static Token *expand_smacro(Token *tline)
{
    smacro_deadman.total  = nasm_limit[LIMIT_MACRO_TOKENS];
    smacro_deadman.levels = nasm_limit[LIMIT_MACRO_LEVELS];
    smacro_deadman.triggered = false;
    return expand_smacro_noreset(tline);
}

static Token *expand_smacro_noreset(Token *org_tline)
{
    Token *tline;
    bool expanded;
    errhold errhold;       /* Hold warning/errors during expansion */

    if (!org_tline)
        return NULL;            /* Empty input */

    /*
     * Trick: we should avoid changing the start token pointer since it can
     * be contained in "next" field of other token. Because of this
     * we allocate a copy of first token and work with it; at the end of
     * routine we copy it back
     */
    tline = dup_Token(org_tline->next, org_tline);

    /*
     * Pretend that we always end up doing expansion on the first pass;
     * that way %+ get processed. However, if we process %+ before the
     * first pass, we end up with things like MACRO %+ TAIL trying to
     * look up the macro "MACROTAIL", which we don't want.
     */
    expanded = true;

    while (true) {
        static const struct concat_mask tmatch[] = {
            {
                CONCAT_ID | CONCAT_LOCAL_MACRO |
                CONCAT_ENVIRON | CONCAT_PREPROC_ID, /* head */
                CONCAT_ID | CONCAT_LOCAL_MACRO |
                CONCAT_ENVIRON | CONCAT_PREPROC_ID |
                CONCAT_NUM      /* tail */
            }
        };
        Token **tail = &tline;

        /*
         * We hold warnings/errors until we are done in this loop. It is
         * possible for nuisance warnings to appear that disappear on later
         * passes.
         */
        errhold = nasm_error_hold_push();

        while (*tail)           /* main token loop */
            expanded |= !!expand_one_smacro(&tail);

         if (!expanded)
            break;              /* Done! */

        /*
         * Now scan the entire line and look for successive TOKEN_IDs
         * that resulted after expansion (they can't be produced by
         * tokenize()). The successive TOKEN_IDs should be concatenated.
         * Also we look for %+ tokens and concatenate the tokens
         * before and after them (without white spaces in between).
         */
        if (!paste_tokens(&tline, tmatch, ARRAY_SIZE(tmatch), true))
            break;              /* Done again! */

        nasm_error_hold_pop(errhold, false);
        expanded = false;
    }
    nasm_error_hold_pop(errhold, true);

    if (!tline) {
        /*
         * The expression expanded to empty line;
         * we can't return NULL because of the "trick" above.
         * Just set the line to a single WHITESPACE token.
	 */

	tline = new_White(NULL);
    }

    steal_Token(org_tline, tline);
    org_tline->next = tline->next;
    delete_Token(tline);

    return org_tline;
}

/*
 * Similar to expand_smacro but used exclusively with macro identifiers
 * right before they are fetched in. The reason is that there can be
 * identifiers consisting of several subparts. We consider that if there
 * are more than one element forming the name, user wants a expansion,
 * otherwise it will be left as-is. Example:
 *
 *      %define %$abc cde
 *
 * the identifier %$abc will be left as-is so that the handler for %define
 * will suck it and define the corresponding value. Other case:
 *
 *      %define _%$abc cde
 *
 * In this case user wants name to be expanded *before* %define starts
 * working, so we'll expand %$abc into something (if it has a value;
 * otherwise it will be left as-is) then concatenate all successive
 * PP_IDs into one.
 */
static Token *expand_id(Token * tline)
{
    Token *cur, *oldnext = NULL;

    if (!tline || !tline->next)
        return tline;

    cur = tline;
    while (cur->next &&
           (cur->next->type == TOKEN_ID ||
            cur->next->type == TOKEN_PREPROC_ID ||
            cur->next->type == TOKEN_LOCAL_MACRO ||
            cur->next->type == TOKEN_NUM))
        cur = cur->next;

    /* If identifier consists of just one token, don't expand */
    if (cur == tline)
        return tline;

    if (cur) {
        oldnext = cur->next;    /* Detach the tail past identifier */
        cur->next = NULL;       /* so that expand_smacro stops here */
    }

    tline = expand_smacro(tline);

    if (cur) {
        /* expand_smacro possibly changhed tline; re-scan for EOL */
        cur = tline;
        while (cur && cur->next)
            cur = cur->next;
        if (cur)
            cur->next = oldnext;
    }

    return tline;
}

/*
 * This is called from find_mmacro_in_list() after finding a suitable macro.
 */
static MMacro *use_mmacro(MMacro *m, int *nparamp, Token ***paramsp)
{
    int nparam = *nparamp;
    Token **params = *paramsp;

    /*
     * This one is right. Just check if cycle removal
     * prohibits us using it before we actually celebrate...
     */
    if (m->in_progress > m->max_depth) {
        if (m->max_depth > 0) {
            /* Document this properly when recursive mmacros re-implemented */
            nasm_warn(WARN_OTHER,
                      "reached maximum recursion depth of %"PRId32,
                      m->max_depth);
        }
        nasm_free(params);
        *nparamp = 0;
        *paramsp = NULL;
        return NULL;
    }

    /*
     * It's right, and we can use it. Add its default
     * parameters to the end of our list if necessary.
     */
    if (m->defaults && nparam < m->nparam_min + m->ndefs) {
        int newnparam = m->nparam_min + m->ndefs;
        params = nasm_realloc(params, sizeof(*params) * (newnparam+2));
        memcpy(&params[nparam+1], &m->defaults[nparam+1-m->nparam_min],
               (newnparam - nparam) * sizeof(*params));
        nparam = newnparam;
    }
    /*
     * If we've gone over the maximum parameter count (and
     * we're in Plus mode), ignore parameters beyond
     * nparam_max.
     */
    if (m->plus && nparam > m->nparam_max)
        nparam = m->nparam_max;

    /*
     * If nparam was adjusted above, make sure the list is still
     * NULL-terminated.
     */
    params[nparam+1] = NULL;

    /* Done! */
    *paramsp = params;
    *nparamp = nparam;
    return m;
}

/*
 * Search a macro list and try to find a match. If matching, call
 * use_mmacro() to set up the macro call. m points to the list of
 * search, which is_mmacro() sets to the first *possible* match.
 */
static MMacro *
find_mmacro_in_list(MMacro *m, const char *finding,
                    int *nparamp, Token ***paramsp)
{
    int nparam = *nparamp;

    while (m) {
        if (m->nparam_min <= nparam
            && (m->plus || nparam <= m->nparam_max)) {
            /*
             * This one matches, use it.
             */
            return use_mmacro(m, nparamp, paramsp);
        }

        /*
         * Otherwise search for the next one with a name match.
         */
        list_for_each(m, m->next) {
            if (!mstrcmp(m->name, finding, m->casesense))
                break;
        }
    }

    return NULL;
}

/*
 * Determine whether the given line constitutes a multi-line macro
 * call, and return the MMacro structure called if so. Doesn't have
 * to check for an initial label - that's taken care of in
 * expand_mmacro - but must check numbers of parameters. Guaranteed
 * to be called with tline->type == TOKEN_ID, so the putative macro
 * name is easy to find.
 */
static MMacro *is_mmacro(Token * tline, int *nparamp, Token ***paramsp)
{
    MMacro *head, *m, *found;
    Token **params, **comma;
    int raw_nparam, nparam;
    const char *finding;
    bool empty_args;

    *nparamp = 0;
    *paramsp = NULL;

    if (!tok_macro_id(tline))
        return NULL;

    finding = tok_text(tline);
    empty_args =  !tline->next;
    head = (MMacro *) hash_findix(&mmacros, finding);

    /*
     * Efficiency: first we see if any macro exists with the given
     * name which isn't already excluded by macro cycle removal.
     * (The cycle removal test here helps optimize the case of wrapping
     * instructions, and is cheap to do here.)
     *
     * If not, we can return NULL immediately. _Then_ we
     * count the parameters, and then we look further along the
     * list if necessary to find the proper MMacro.
     */
    list_for_each(m, head) {
        if (!mstrcmp(m->name, finding, m->casesense) &&
            (m->in_progress != 1 || m->max_depth > 0))
            break;              /* Found something that needs consideration */
    }
    if (!m)
        return NULL;

    /*
     * OK, we have a potential macro. Count and demarcate the
     * parameters.
     */
    comma = count_mmac_params(tline->next, nparamp, paramsp);
    raw_nparam = *nparamp;

    /*
     * Search for an exact match. This cannot come *before* the m
     * found in the list search before, so we can start there.
     *
     * If found is NULL and *paramsp has been cleared, then we
     * encountered an error for which we have already issued a
     * diagnostic, so we should not proceed.
     */
    found = find_mmacro_in_list(m, finding, nparamp, paramsp);
    if (!*paramsp)
        return NULL;

    nparam = *nparamp;
    params = *paramsp;

    /*
     * Special weirdness: in NASM < 2.15, an expansion of
     * *only* whitespace, as can happen during macro expansion under
     * certain circumstances, is counted as zero arguments for the
     * purpose of %0, but one argument for the purpose of macro
     * matching! In particular, this affects:
     *
     * foobar %1
     *
     * ... with %1 being empty; this would call the one-argument
     * version of "foobar" with an empty argument, equivalent to
     *
     * foobar {%1}
     *
     * ... except that %0 would be set to 0 inside foobar, even if
     * foobar is declared with "%macro foobar 1" or equivalent!
     *
     * The proper way to do that is to define "%macro foobar 0-1".
     *
     * To be compatible without doing something too stupid, try to
     * match a zero-argument macro first, but if that fails, try
     * for a one-argument macro with the above behavior.
     *
     * Furthermore, NASM < 2.15 will match stripping a tailing empty
     * argument, but in that case %0 *does* reflect that this argument
     * have been stripped; this is handled in count_mmac_params().
     *
     * To disable these insane legacy behaviors, use:
     *
     * %pragma preproc sane_empty_expansion yes
     */
    if (!ppconf.sane_empty_expansion) {
        if (!found) {
            if (raw_nparam == 0 && !empty_args) {
                /*
                 * A single all-whitespace parameter as the only thing?
                 * Look for a one-argument macro, but don't adjust
                 * *nparamp.
                 */
                int bogus_nparam = 1;
                params[2] = NULL;
                found = find_mmacro_in_list(m, finding, &bogus_nparam, paramsp);
            } else if (raw_nparam > 1 && comma) {
                Token *comma_tail = *comma;

                /*
                 * Drop the terminal argument and try again.
                 * If we fail, we need to restore the comma to
                 * preserve tlist.
                 */
                *comma = NULL;
                *nparamp = raw_nparam - 1;
                found = find_mmacro_in_list(m, finding, nparamp, paramsp);
                if (found)
                    delete_tlist(comma_tail);
                else
                    *comma = comma_tail;
            }

            if (!*paramsp)
                return NULL;
        } else if (comma) {
            delete_tlist(*comma);
            *comma = NULL;
            if (raw_nparam > found->nparam_min &&
                raw_nparam <= found->nparam_min + found->ndefs) {
                /* Replace empty argument with default parameter */
                params[raw_nparam] =
                    found->defaults[raw_nparam - found->nparam_min];
            } else if (raw_nparam > found->nparam_max && found->plus) {
                /* Just drop the comma, don't adjust argument count */
            } else {
                /* Drop argument. This may cause nparam < nparam_min. */
                params[raw_nparam] = NULL;
                *nparamp = nparam = raw_nparam - 1;
            }
        }

        if (found) {
            if (raw_nparam < found->nparam_min ||
                (raw_nparam > found->nparam_max && !found->plus)) {
                nasm_warn(WARN_PP_MACRO_PARAMS_LEGACY,
                          "improperly calling multi-line macro `%s' with %d parameters",
                          found->name, raw_nparam);
            } else if (comma) {
                nasm_warn(WARN_PP_MACRO_PARAMS_LEGACY,
                          "dropping trailing empty parameter in call to multi-line macro `%s'", found->name);
            }
        }
    }

    /*
     * After all that, we didn't find one with the right number of
     * parameters. Issue a warning, and fail to expand the macro.
     */
    if (found)
        return found;

    nasm_warn(WARN_PP_MACRO_PARAMS_MULTI,
               "multi-line macro `%s' exists, but not taking %d parameter%s",
              finding, nparam, (nparam == 1) ? "" : "s");
    nasm_free(*paramsp);
    return NULL;
}


#if 0

/*
 * Save MMacro invocation specific fields in
 * preparation for a recursive macro expansion
 */
static void push_mmacro(MMacro *m)
{
    MMacroInvocation *i;

    i = nasm_malloc(sizeof(MMacroInvocation));
    i->prev = m->prev;
    i->params = m->params;
    i->iline = m->iline;
    i->nparam = m->nparam;
    i->rotate = m->rotate;
    i->paramlen = m->paramlen;
    i->unique = m->unique;
    i->condcnt = m->condcnt;
    m->prev = i;
}


/*
 * Restore MMacro invocation specific fields that were
 * saved during a previous recursive macro expansion
 */
static void pop_mmacro(MMacro *m)
{
    MMacroInvocation *i;

    if (m->prev) {
        i = m->prev;
        m->prev = i->prev;
        m->params = i->params;
        m->iline = i->iline;
        m->nparam = i->nparam;
        m->rotate = i->rotate;
        m->paramlen = i->paramlen;
        m->unique = i->unique;
        m->condcnt = i->condcnt;
        nasm_free(i);
    }
}

#endif

/*
 * List an mmacro call with arguments (-Lm option)
 */
static void list_mmacro_call(const MMacro *m)
{
    const char prefix[] = " ;;; [macro] ";
    size_t namelen, size;
    char *buf, *p;
    unsigned int i;
    const Token *t;

    namelen = strlen(m->iname);
    size = namelen + sizeof(prefix); /* Includes final null (from prefix) */

    for (i = 1; i <= m->nparam; i++) {
        int j = 0;
        size += 3;              /* Braces and space/comma */
        list_for_each(t, m->params[i]) {
            if (j++ >= m->paramlen[i])
                break;
            size += (t->type == TOKEN_WHITESPACE) ? 1 : t->len;
        }
    }

    buf = p = nasm_malloc(size);
    p = mempcpy(p, prefix, sizeof(prefix) - 1);
    p = mempcpy(p, m->iname, namelen);
    *p++ = ' ';

    for (i = 1; i <= m->nparam; i++) {
        int j = 0;
        *p++ = '{';
        list_for_each(t, m->params[i]) {
            if (j++ >= m->paramlen[i])
                break;
	    p = mempcpy(p, tok_text(t), t->len);
        }
        *p++ = '}';
        *p++ = ',';
    }

    *--p = '\0';                /* Replace last delimiter with null */
    lfmt->line(LIST_MACRO, -1, buf);
    nasm_free(buf);
}

/*
 * Collect information about macro invocations for the benefit of
 * the debugger. During execution we create a reverse list; before
 * calling the backend reverse it to definition/invocation order just
 * to be nicer. [XXX: not implemented yet]
 */
struct debug_macro_inv *debug_current_macro;

/* Get/create a addr structure for a seg:inv combo */
static struct debug_macro_addr *
debug_macro_get_addr_inv(int32_t seg, struct debug_macro_inv *inv)
{
    struct debug_macro_addr *addr;
    nasm_static_assert(offsetof(struct debug_macro_addr, tree) == 0);

    if (likely(seg == inv->lastseg))
        return inv->addr.last;

    inv->lastseg = seg;
    addr = (struct debug_macro_addr *)
        rb_search_exact(inv->addr.tree, seg);
    if (unlikely(!addr)) {
        nasm_new(addr);
        addr->tree.key = seg;
        inv->addr.tree = rb_insert(inv->addr.tree, &addr->tree);
        inv->naddr++;
        if (inv->up)
            addr->up = debug_macro_get_addr_inv(seg, inv->up);
    }

    return inv->addr.last = addr;
}

/* Get/create an addr structure for a seg in debug_current_macro */
struct debug_macro_addr *debug_macro_get_addr(int32_t seg)
{
    return debug_macro_get_addr_inv(seg, debug_current_macro);
}

static struct debug_macro_info dmi;
static struct debug_macro_inv_list *current_inv_list = &dmi.inv;

static void debug_macro_start(MMacro *m, struct src_location where)
{
    struct debug_macro_def *def = m->dbg.def;
    struct debug_macro_inv *inv;

    nasm_assert(!m->dbg.inv);

    /* First invocation? Need to create a def structure */
    if (unlikely(!def)) {
        nasm_new(def);
        def->name = nasm_strdup(m->name);
        def->where = m->where;

        def->next = dmi.def.l;
        dmi.def.l = def;
        dmi.def.n++;

        m->dbg.def = def;
    }

    nasm_new(inv);
    inv->lastseg = NO_SEG;
    inv->where = where;
    inv->up = debug_current_macro;
    inv->next = current_inv_list->l;
    inv->def = def;
    current_inv_list->l = inv;
    current_inv_list->n++;
    current_inv_list = &inv->down;

    def->ninv++;
    m->dbg.inv = inv;
    debug_current_macro = inv;
}

static void debug_macro_end(MMacro *m)
{
    struct debug_macro_inv *inv, *minv;
    MMacro *mm = istk->mstk.mmac;

    /* Not actually ending the current macro */
    if (m->mstk.mmac == m)
        return;

    inv = m->dbg.inv;
    nasm_assert(inv == debug_current_macro);

    list_reverse(inv->down.l);

    m->dbg.inv = NULL;
    inv = inv->up;

    minv = mm ? mm->dbg.inv : NULL;

    nasm_assert(inv == minv);
    debug_current_macro = inv;
    if (inv)
        current_inv_list = &inv->down;
    else
        current_inv_list = &dmi.inv;
}

static void free_debug_macro_addr_tree(struct rbtree *tree)
{
    struct rbtree *left, *right;
    nasm_static_assert(offsetof(struct debug_macro_addr,tree) == 0);

    if (!tree)
        return;

    left  = rb_left(tree);
    right = rb_right(tree);

    nasm_free(tree);

    free_debug_macro_addr_tree(left);
    free_debug_macro_addr_tree(right);
}

static void free_debug_macro_inv_list(struct debug_macro_inv *inv)
{
    struct debug_macro_inv *itmp;

    if (!inv)
        return;

    list_for_each_safe(inv, itmp, inv) {
        free_debug_macro_inv_list(inv->down.l);
        free_debug_macro_addr_tree(inv->addr.tree);
        nasm_free(inv);
    }
}

static void free_debug_macro_info(void)
{
    struct debug_macro_def *def, *dtmp;

    list_for_each_safe(def, dtmp, dmi.def.l)
        nasm_free(def);

    free_debug_macro_inv_list(dmi.inv.l);

    nasm_zero(dmi);
}

static void debug_macro_output(void)
{
    list_reverse(dmi.inv.l);
    dfmt->debug_mmacros(&dmi);
    free_debug_macro_info();
}

/*
 * Expand the multi-line macro call made by the given line, if
 * there is one to be expanded. If there is, push the expansion on
 * istk->expansion and return 1. Otherwise return 0.
 */
static int expand_mmacro(Token * tline)
{
    Token *startline = tline;
    Token *label = NULL;
    bool dont_prepend = false;
    Token **params, *t, *tt;
    MMacro *m;
    Line *l, *ll;
    int i, *paramlen;
    const char *mname;
    int nparam = 0;

    t = tline;
    t = skip_white(t);
    if (!tok_macro_id(t))
        return 0;
    m = is_mmacro(t, &nparam, &params);
    if (m) {
        mname = tok_text(t);
    } else {
        Token *last;
        /*
         * We have an id which isn't a macro call. We'll assume
         * it might be a label; we'll also check to see if a
         * colon follows it. Then, if there's another id after
         * that lot, we'll check it again for macro-hood.
         */
        label = last = t;
        t = t->next;
        if (tok_white(t))
            last = t, t = t->next;
        if (tok_is(t, ':')) {
            dont_prepend = true;
            last = t, t = t->next;
            if (tok_white(t))
                last = t, t = t->next;
        }
        m = is_mmacro(t, &nparam, &params);
        if (!m)
            return 0;
        last->next = NULL;
        mname = tok_text(t);
        tline = t;
    }

    if (unlikely(mmacro_deadman.total >= nasm_limit[LIMIT_MMACROS] ||
                 mmacro_deadman.levels >= nasm_limit[LIMIT_MACRO_LEVELS])) {
        if (!mmacro_deadman.triggered) {
            nasm_nonfatal("interminable multiline macro recursion");
            mmacro_deadman.triggered = true;
        }
        return 0;
    }

    mmacro_deadman.total++;
    mmacro_deadman.levels++;

    /*
     * Fix up the parameters: this involves stripping leading and
     * trailing whitespace and stripping braces if they are present.
     */
    nasm_newn(paramlen, nparam+1);

    for (i = 1; i < nparam+1 && (t = params[i]); i++) {
        bool braced = false;
        int brace = 0;
        int white = 0;
        bool comma = !m->plus || i < nparam;

        t = skip_white(t);
        if (tok_is(t, '{')) {
            t = t->next;
            brace = 1;
            braced = true;
            comma = false;
        }

        params[i] = t;
        for (; t; t = t->next) {
            if (tok_white(t)) {
                white++;
                continue;
            }

            switch(t->type) {
            case ',':
                if (comma && !brace)
                    goto endparam;
                break;

            case '{':
                brace++;
                break;

            case '}':
                brace--;
                if (braced && !brace) {
                    paramlen[i] += white;
                    goto endparam;
                }
                break;

            default:
                break;
            }

            paramlen[i] += white + 1;
            white = 0;
        }
    endparam:
        ;
    }

    /*
     * OK, we have a MMacro structure together with a set of
     * parameters. We must now go through the expansion and push
     * copies of each Line on to istk->expansion. Substitution of
     * parameter tokens and macro-local tokens doesn't get done
     * until the single-line macro substitution process; this is
     * because delaying them allows us to change the semantics
     * later through %rotate and give the right semantics for
     * nested mmacros.
     *
     * First, push an end marker on to istk->expansion, mark this
     * macro as in progress, and set up its invocation-specific
     * variables.
     */
    nasm_new(ll);
    ll->next = istk->expansion;
    ll->finishes = get_mmacro(m);
    ll->where = istk->where;
    istk->expansion = ll;

    /*
     * Save the previous MMacro expansion in the case of
     * macro recursion
     */
#if 0
    if (m->max_depth && m->in_progress)
        push_mmacro(m);
#endif

    m->in_progress++;
    m->params = params;
    m->iline = tline;
    m->iname = nasm_strdup(mname);
    m->nparam = nparam;
    m->rotate = 0;
    m->paramlen = paramlen;
    m->unique = unique++;
    m->condcnt = 0;

    m->mstk = istk->mstk;       /* Inherits refcounts */
    istk->mstk.mstk = get_mmacro(m);
    istk->mstk.mmac = get_mmacro(m);

    list_for_each(l, m->expansion) {
        nasm_new(ll);
        ll->next = istk->expansion;
        istk->expansion = ll;
        ll->first = dup_tlist(l->first, NULL);
        ll->where = l->where;
    }

    /*
     * If we had a label, and this macro definition does not include
     * a %00, push it on as the first line of, ot
     * the macro expansion.
     */
    if (label) {
        /*
         * We had a label. If this macro contains an %00 parameter,
         * save the value as a special parameter (which is what it
         * is), otherwise push it as the first line of the macro
         * expansion.
         */
        if (m->capture_label) {
            params[0] = dup_Token(NULL, label);
            paramlen[0] = 1;
            delete_tlist(startline);
       } else {
            nasm_new(ll);
            ll->finishes = NULL;
            ll->next = istk->expansion;
            istk->expansion = ll;
            ll->first = startline;
            ll->where = istk->where;
            if (!dont_prepend) {
                while (label->next)
                    label = label->next;
                label->next = tt = make_tok_char(NULL, ':');
            }
        }
    }

    istk->nolist += !!(m->nolist & NL_LIST);
    istk->noline += !!(m->nolist & NL_LINE);

    if (!istk->nolist) {
        if (list_option('m'))
            list_mmacro_call(m);

        lfmt->uplevel(LIST_MACRO, 0);

        if (ppdbg & PDBG_MMACROS)
            debug_macro_start(m, src_where());
    }

    if (!istk->noline)
        src_macro_push(get_mmacro(m), istk->where);

    return 1;
}

/*
 * This function decides if an error message should be suppressed.
 * It will never be called with a severity level of ERR_FATAL or
 * higher.
 */
bool pp_suppress_error(errflags severity)
{
    /*
     * If we're in a dead branch of IF or something like it, ignore the error.
     * However, because %else etc are evaluated in the state context
     * of the previous branch, errors might get lost:
     *   %if 0 ... %else trailing garbage ... %endif
     * So %else etc should set the ERR_PP_PRECOND flag.
     */
    if (istk && istk->conds &&
	((severity & ERR_PP_PRECOND) ?
	 istk->conds->state == COND_NEVER :
	 !emitting(istk->conds->state)))
        return true;

    return false;
}

static Token *
stdmac_file(const SMacro *s, Token **params, int nparams)
{
    const char *fname = src_get_fname();

    (void)s;
    (void)params;
    (void)nparams;

    return fname ? make_tok_qstr(NULL, fname) : NULL;
}

static Token *
stdmac_line(const SMacro *s, Token **params, int nparams)
{
    (void)s;
    (void)params;
    (void)nparams;

    return make_tok_num(NULL, src_get_linnum());
}

static Token *
stdmac_bits(const SMacro *s, Token **params, int nparams)
{
    (void)s;
    (void)params;
    (void)nparams;

    return make_tok_num(NULL, globl.bits);
}

static Token *
stdmac_ptr(const SMacro *s, Token **params, int nparams)
{
    (void)s;
    (void)params;
    (void)nparams;

    switch (globl.bits) {
    case 16:
	return new_Token(NULL, TOKEN_ID, "word", 4);
    case 32:
	return new_Token(NULL, TOKEN_ID, "dword", 5);
    case 64:
	return new_Token(NULL, TOKEN_ID, "qword", 5);
    default:
        panic();
    }
}

static Token *
stdmac_default(const SMacro *s, Token **params, int nparam)
{
    Token *t = NULL;

    (void)s;
    (void)params;
    (void)nparam;

    t = new_Token(t, TOKEN_ID, globl.rel & EAF_NOTFSGS ? "rel" : "abs", 3);
    t = make_tok_char(t, ',');
    t = new_Token(t, TOKEN_ID, "fs", 2);
    t = make_tok_char(t, ':');
    t = new_Token(t, TOKEN_ID, globl.rel & EAF_FS ? "rel" : "abs", 3);
    t = make_tok_char(t, ',');
    t = new_Token(t, TOKEN_ID, "gs", 2);
    t = make_tok_char(t, ':');
    t = new_Token(t, TOKEN_ID, globl.rel & EAF_GS ? "rel" : "abs", 3);
    t = make_tok_char(t, ',');
    t = new_Token(t, TOKEN_ID, globl.bnd ? "bnd" : "nobnd", 0);

    return t;
}

/* %is...() function macros */
static Token *
stdmac_is(const SMacro *s, Token **params, int nparams)
{
    int retval;
    struct Token *pline = params[0];

    (void)nparams;

    params[0] = NULL;           /* Don't free this later */

    retval = if_condition(pline, s->expandpvt.u, s->name) == COND_IF_TRUE;
    return make_tok_num(NULL, retval);
}

/*
 * Join all expanded macro arguments with commas, e.g. %eval().
 * Remember that this needs to output the tokens in reverse order.
 *
 * This can also be used when only single argument is already ready
 * to be emitted, e.g. %str().
 */
static Token *
stdmac_join(const SMacro *s, Token **params, int nparams)
{
    struct Token *tline = NULL;
    int i;

    (void)s;

    for (i = 0; i < nparams; i++) {
        Token *t, *ttmp;

        if (i)
            tline = make_tok_char(tline, ',');

        list_for_each_safe(t, ttmp, params[i]) {
            t->next = tline;
            tline = t;
        }

        /* Avoid freeing the tokens we "stole" */
        params[i] = NULL;
    }

    return tline;
}

/* %strcat() function */
static Token *
stdmac_strcat(const SMacro *s, Token **params, int nparams)
{
    int i;
    size_t len = 0;
    char *str, *p;
    Token *t;

    (void)s;

    for (i = 0; i < nparams; i++) {
        unquote_token(params[i]);
        len += params[i]->len;
    }

    p = str = nasm_malloc(len+1);

    for (i = 0; i < nparams; i++) {
        p = mempcpy(p, tok_text(params[i]), params[i]->len);
    }
    *p = '\0';

    t = make_tok_qstr_len(NULL, str, p - str);
    nasm_free(str);
    return t;
}

/* %hs2b() function */
static Token *
stdmac_hs2b(const SMacro *s, Token **params, int nparams)
{
    int i;
    size_t len = 0;
    char *str, *q;
    Token *t;

    (void)s;

    for (i = 0; i < nparams; i++) {
        unquote_token(params[i]);
        len += (params[i]->len + 1) >> 1; /* Maximum possible */
    }

    q = str = nasm_malloc(len+1);

    for (i = 0; i < nparams; i++) {
        const char *p = tok_text(params[i]);
        unsigned int j;
        unsigned int len = params[i]->len;
        int v = -1;

        for (j = 0; j < len; j++) {
            unsigned int hv = nasm_hexval(*p++);
            if (hv > 15) {
                /* Separator character or end of string */
                if (v >= 0)
                    *q++ = v;
                v = -1;
            } else {
                if (v >= 0) {
                    *q++ = (v << 4) + hv;
                    v = -1;
                } else {
                    v = hv;
                }
            }
        }
        /* Partial byte at the end? */
        if (v >= 0)
            *q++ = v;
    }
    *q = '\0';

    t = make_tok_qstr_len(NULL, str, q - str);
    nasm_free(str);
    return t;
}

/* %b2hs() function */
static Token *
stdmac_b2hs(const SMacro *s, Token **params, int nparams)
{
    const char * const dchars = nasm_digit_chars(false);
    const char *p;
    const char *sep;
    uint8_t b;
    char *str, *q;
    size_t bytes, len, seplen;
    size_t i;
    Token *t;

    (void)s;
    (void)nparams;

    p = unquote_token(params[0]);

    if (!params[0]->len)
        return make_tok_qstr_len(NULL, "", 0);

    sep    = unquote_token(params[1]);
    bytes  = params[0]->len;
    seplen = params[1]->len;
    len    = (bytes << 1) + (seplen * (bytes-1));

    q = str = nasm_malloc(len+1);

    b = *p++;
    *q++ = dchars[b >> 4];
    *q++ = dchars[b & 15];
    for (i = 1; i < bytes; i++) {
        if (seplen)
            q = mempcpy(q, sep, seplen);
        b = *p++;
        *q++ = dchars[b >> 4];
        *q++ = dchars[b & 15];
    }
    *q = '\0';

    t = make_tok_qstr_len(NULL, str, q - str);
    nasm_free(str);
    return t;
}

/* %substr() function */
static Token *
stdmac_substr(const SMacro *s, Token **params, int nparams)
{
    int64_t start, count;

    (void)nparams;
    (void)s;

    start = get_tok_num(params[1], NULL);
    count = get_tok_num(params[2], NULL);

    return pp_substr_common(params[0], start, count);
}

/* %ord() function */
static Token *
stdmac_ord(const SMacro *s, Token **params, int nparams)
{
    int64_t start, count;
    const uint8_t *txt;
    Token *t;

    (void)nparams;
    (void)s;

    start = get_tok_num(params[1], NULL);
    count = get_tok_num(params[2], NULL);

    txt = (const uint8_t *)pp_get_substr(params[0], start, &count);
    if (!count)
        return NULL;

    t = make_tok_num(NULL, *txt++);
    while (--count) {
        t = make_tok_char(t, ',');
        t = make_tok_num(t, *txt++);
    }
    return t;
}

/* %chr() function */
static Token *
stdmac_chr(const SMacro *s, Token **params, int nparams)
{
    int i;
    char *buf = nasm_malloc(nparams);
    char *p = buf;
    Token *t;

    (void)s;

    for (i = 0; i < nparams; i++) {
        /* Skip empty parameters! */
        if (params[i]) {
            *p++ = get_tok_num(params[i], NULL);
        }
    }

    t = make_tok_qstr_len(NULL, buf, p - buf);
    nasm_free(buf);
    return t;
}

/* %strlen() function */
static Token *
stdmac_strlen(const SMacro *s, Token **params, int nparams)
{
    (void)nparams;
    (void)s;

    unquote_token(params[0]);
    return make_tok_num(NULL, params[0]->len);
}

/* %tok() function */
static Token *
stdmac_tok(const SMacro *s, Token **params, int nparams)
{
    (void)nparams;
    (void)s;

    return reverse_tokens(tokenize(unquote_token_cstr(params[0])));
}

/* %sel() */
static Token *
stdmac_sel(const SMacro *s, Token **params, int nparams)
{
    int64_t which;

    /*
     * params[0] will have been generated by make_tok_num.
     */
    which = get_tok_num(params[0], NULL);

    if (unlikely(which < 1)) {
        nasm_warn(WARN_PP_SEL_RANGE,
                  "%s(%"PRId64") is not a valid selector", s->name, which);
        return NULL;
    } else if (unlikely(which >= nparams)) {
        nasm_warn(WARN_PP_SEL_RANGE,
                  "%s(%"PRId64") exceeds the number of arguments",
                  s->name, which);
        return NULL;
    }

    return new_Token(NULL, tok_smac_param(which), "", 0);
}

/* %cond() */
static Token *
stdmac_cond(const SMacro *s, Token **params, int nparams)
{
    int64_t which;
    (void)s;
    (void)params;

    /*
     * params[0] will have been generated by make_tok_num.
     */
    which = get_tok_num(params[0], NULL);

    /* Booleanize: true -> 1, false -> 2 (else) */
    which = which ? 1 : 2;
    if (which >= nparams) {
        /* false, and no else clause */
        return NULL;
    }

    return new_Token(NULL, tok_smac_param(which), "", 0);
}

/* %selbits() */
static Token *
stdmac_selbits(const SMacro *s, Token **params, int nparams)
{
    int which = ilog2_32(globl.bits)-4;
    (void)s;
    (void)params;

    if (nparams <= which)
        which = nparams - 1;

    return new_Token(NULL, tok_smac_param(which), "", 0);
}

/* %count() function */
static Token *
stdmac_count(const SMacro *s, Token **params, int nparams)
{
    (void)s;
    (void)params;

    return make_tok_num(NULL, nparams);
}

/* %num() function */
static Token *
stdmac_num(const SMacro *s, Token **params, int nparam)
{
    int64_t parm[3];
    uint64_t n;
    int64_t dparm, bparm;
    const int maxlen = 256;
    char numbuf[256+5];
    char *p;
    char decorate;
    int i;

    (void)nparam;

    for (i = 0; i < (int)ARRAY_SIZE(parm); i++)
        parm[i] = get_tok_num(params[i], NULL);

    n      = parm[0];
    dparm  = parm[1];
    bparm  = parm[2];

    decorate = 0;
    if (bparm < 0) {
        bparm = -bparm;
        switch (bparm) {
        case 2:
            decorate = 'b';
            break;
        case 8:
            decorate = 'q';
            break;
        case 10:
            decorate = 'd';
            break;
        case 16:
            decorate = 'x';
            break;
        default:
            bparm = -bparm;     /* Error out below */
            break;
        }
    }

    if (bparm < 2 || bparm > NUMSTR_MAXBASE) {
        nasm_nonfatal("invalid base %"PRId64" in %s()\n", bparm, s->name);
        return NULL;
    }

    if (dparm < -maxlen || dparm > maxlen) {
        nasm_nonfatal("digit count %"PRId64" specified to %s() too large",
                      dparm, s->name);
        dparm = -1;
    }

    /* Are we supposed to generate an empty string for zero? */
    if (!dparm && !n)
        decorate = 0;

    p = numbuf;
    *p++ = '\'';
    if (decorate) {
        *p++ = '0';
        *p++ = decorate;
    }

    p += numstr(p, maxlen, n, dparm, bparm, false);
    *p++ = '\'';
    *p = '\0';

    return new_Token(NULL, TOKEN_STR, numbuf, p - numbuf);
}

/* %abs() function */
static Token *
stdmac_abs(const SMacro *s, Token **params, int nparam)
{
    char numbuf[24];
    int len;
    int64_t v;
    uint64_t u;

    (void)s;
    (void)nparam;

    v = get_tok_num(params[0], NULL);
    u = v < 0 ? -v : v;

    /*
     * Don't use make_tok_num() here, to make sure we don't emit
     * a minus sign for the case of v = -2^63
     */
    len = snprintf(numbuf, sizeof numbuf, "%"PRIu64, u);
    return new_Token(NULL, TOKEN_NUM, numbuf, len);
}

/* %map() function */
static Token *
stdmac_map(const SMacro *s, Token **params, int nparam)
{
    const char *mname, *ctxname;
    SMacro *smac;
    Context *ctx;
    Token *t, *tline, *mstart;
    int i;
    Token *fixargs;
    int fixparams;              /* Number of fixed parameters */
    int mparams;                /* Number of variable parameters */
    int tparams;                /* Total number of parameters */
    int greedify;               /* Number of parameters that must be joined */
    Token **fparam;             /* Fixed parameters */
    Token **cparam;             /* Final list of macro call parameters */

    t = params[0];
    mname = get_id_noskip(&t, "%map");
    if (!mname)
        return NULL;

    mstart = t;

    fixargs = NULL;
    fixparams = 0;
    mparams = 1;
    t = skip_white(t->next);
    if (tok_is(t, ':')) {
        fixargs = t->next;
        fixparams = count_smacro_args(fixargs, &t);
        t = skip_white(t->next);

        if (tok_is(t, ':')) {
            struct ppscan pps;
            struct tokenval tokval;
            expr *evalresult;
            Token *ep;

            pps.tptr = ep = zap_white(expand_smacro_noreset(t->next));
            t->next = NULL;
            pps.ntokens = -1;
            tokval.t_type = TOKEN_INVALID;
            evalresult = evaluate(ppscan, &pps, &tokval, NULL, true, NULL);
            delete_tlist(ep);

            if (!evalresult || tokval.t_type) {
                nasm_nonfatal("invalid expression in parameter count for `%s' in function %s",
                              mname, s->name);
                return NULL;
            } else if (!is_simple(evalresult)) {
                nasm_nonfatal("non-constant expression in parameter count for `%s' in function %s",
                              mname, s->name);
                return NULL;
            }
            mparams = reloc_value(evalresult);
            if (mparams < 1) {
                nasm_nonfatal("invalid parameter count for `%s' in function %s",
                              mname, s->name);
                return NULL;
            }
        }
    }

    nparam--;
    params++;
    if (nparam % mparams) {
        nasm_nonfatal("%s expected a multiple of %d expansion parameters, got %d\n",
                      s->name, mparams, nparam);
    }

    tparams = fixparams + mparams;

    ctx = get_ctx(mname, &ctxname);
    if (!smacro_defined(ctx, ctxname, tparams, &smac, true, false)
        || smac->nparam == 0 || (smac->in_progress && !smac->recursive)) {
        nasm_nonfatal("macro `%s' taking %d parameter%s not found in function %s",
                      mname, tparams, tparams == 1 ? "" : "s", s->name);
        return NULL;
    }

    if (nparam < mparams)
        return NULL;            /* Empty expansion */

    fparam = NULL;
    if (fixparams) {
        int nfp = fixparams;
        fparam = parse_smacro_args(&fixargs, &nfp, smac);
        if (nfp < fixparams) {
            fixparams = nfp;
            tparams = fixparams + mparams;
        }
    }

    greedify = 0;
    if (unlikely(tparams > smac->nparam)) {
        if (smac->params[smac->nparam-1].flags & SPARM_GREEDY)
            greedify = smac->nparam;
    }

    nasm_newn(cparam, tparams);

    tline = NULL;
    while (1) {
        int xparams;

        for (i = 0; i < fixparams; i++) {
            /* expand_smacro_with_params() is allowed to clobber the
             * parameter array, so we need to give it its own copy.
             */
            cparam[i] = dup_tlist(fparam[i], NULL);
        }

        for (i = fixparams; i < tparams; i++) {
            cparam[i] = *params;
            *params = NULL;     /* Taking over ownership */
            params++;
        }

        if (unlikely(greedify)) {
            /* Need to re-concatenate some number of arguments as
               comma-separated lists... */
            int i;
            Token **tp = &cparam[greedify-1];
            while (*tp)
                tp = &(*tp)->next;

            for (i = greedify; i < tparams; i++) {
                *tp = make_tok_char(NULL, ',');
                tp = steal_tlist(cparam[i], &(*tp)->next);
                cparam[i] = NULL;
            }
            xparams = greedify;
        } else {
            xparams = tparams;
        }

        t = expand_smacro_with_params(smac, mstart, cparam, xparams, NULL);
        if (t) {
            Token *rt = reverse_tokens(t);
            t->next = tline;
            tline = rt;
        }

        for (i = 0; i < xparams; i++)
            delete_tlist(cparam[i]);

        nparam -= mparams;
        if (nparam < mparams)
            break;

        tline = make_tok_char(tline, ',');
    }

    nasm_free(fparam);
    nasm_free(cparam);

    return tline;
}

/* %pathsearch() function */
static Token *
stdmac_pathsearch(const SMacro *s, Token **params, int nparam)
{
    (void)nparam;
    return pp_do_pathsearch(&params[0], s->name);
}

/* %depend() function */
static Token *
stdmac_depend(const SMacro *s, Token **params, int nparam)
{
    (void)nparam;
    return pp_do_depend(&params[0], s->name);
}

static Token *
stdmac_realpath(const SMacro *s, Token **params, int nparam)
{
    const struct file_hash_entry *fhe;
    Token *t, *ot;
    (void)nparam;

    t = tlist_filename(&params[0], &ot, s->name);
    if (!t)
        return NULL;

    inc_fopen(tok_text(t), NULL, &fhe, INC_PROBE|INC_EXACT, NF_BINARY);

    if (fhe) {
        delete_Token(t);
        return make_tok_qstr(NULL, fhe->full->path);
    } else {
        return steal_Token(t, ot);
    }
}

static Token *
stdmac_null(const SMacro *s, Token **params, int nparam)
{
    (void)s;
    (void)params;
    (void)nparam;

    return NULL;                /* Empty expansion */
}

#if 0    /* Disabled, because they seem to be expanded inappropriately */
static Token *
stdmac_user_error(const SMacro *s, Token **params, int nparam)
{
    (void)nparam;
    user_error(s->expandpvt.u, &params[0]);
    return NULL;                /* Always expands to empty */
}
#endif

static Token *
stdmac_find(const SMacro *s, Token **params, int nparam)
{
    bool casesense = s->expandpvt.u;
    int i;
    int found = 0;
    const char *ref = unquote_token(params[0]);
    size_t rlen = params[0]->len;

    for (i = 1; i < nparam; i++) {
        const char *cmp;
        size_t clen;

        /*
         * This is done here rather than by declaring it in the
         * argument flags, so that we don't expand ignored arguments
         * (argument short circuiting.)
         */
        params[i] = expand_sparm_str(params[i], SPARM_STR|SPARM_CONDQUOTE);
        cmp  = unquote_token(params[i]);
        clen = params[i]->len;

        if (rlen == clen && !mmemcmp(ref, cmp, rlen, casesense)) {
            found = i;
            break;
        }
    }
    return make_tok_num(NULL, found);
}

static Token *
stdmac_env(const SMacro *s, Token **params, int nparam)
{
    const char *env;
    (void)s;

    env = pp_getenv(params[0], false);
    if (!env) {
        if (nparam > 1)
            return new_Token(NULL, tok_smac_param(1), "", 0);

        env = "";               /* No fallback argument */
    }

    return make_tok_qstr(NULL, env);
}

static Token *
stdmac_limit(const SMacro *s, Token **params, int nparam)
{
    const char *which_str, *limit;
    enum get_limit_which which;
    int64_t val = 0;

    (void)s;
    (void)nparam;

    which_str = unquote_token(params[1]);

    if (!*which_str || !nasm_stricmp(which_str, "current")) {
        which = GET_LIMIT_CURRENT;
    } else if (!strcmp(which_str, "*") ||
               !nasm_stricmp(which_str, "reset") ||
               !nasm_stricmp(which_str, "init")) {
        which = GET_LIMIT_INIT;
    } else if (!nasm_stricmp(which_str, "default")) {
        which = GET_LIMIT_DEFAULT;
    } else if (!nasm_stricmp(which_str, "unlimited") ||
               !nasm_stricmp(which_str, "maximum") ||
               !nasm_stricmp(which_str, "max")) {
        which = GET_LIMIT_MAX;
    } else {
        nasm_nonfatal("invalid second argument `%s' to %s()",
                      which_str, s->name);
        goto err;
    }

    limit = unquote_token(params[0]);
    if (!*limit || !nasm_stricmp(limit, "unlimited"))
        val = LIMIT_MAX_VAL;
    else
        val = nasm_get_limit(limit, which);

err:
    return make_tok_num(NULL, val);
}

/*
 * Given the currently default or command-line default listing options
 */
static Token *
get_list_options(uint64_t optmask)
{
    static const char optchars[63] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char optbuf[65], *p;
    unsigned int i;

    p = optbuf;
    for (i = 0; i < 62; i++) {
        if (optmask & 4)
            *p++ = optchars[i];
        optmask >>= 1;
    }
    *p = '\0';

    return make_tok_qstr_len(NULL, optbuf, p-optbuf);
}

static Token *
stdmac_list_options(const SMacro *s, Token **params, int nparam)
{
    (void)s;
    (void)params;
    (void)nparam;
    return get_list_options(active_list_options);
}

/*
 * Wrapper around define_smacro() which also checks to see if it is
 * a preprocessor directive, so that pp_op_may_be_function[] needs to
 * be set.
 */
static SMacro *define_magic(const char *mname, bool casesense, SMacro *tmpl)
{
    enum preproc_token op = pp_get_directive(mname);
    if (op != PP_invalid)
        pp_op_may_be_function[op] = true;

    /* Magic functions can be recursive */
    if (tmpl && tmpl->nparam && tmpl->expand)
        tmpl->recursive = true;

    return define_smacro(mname, casesense, NULL, tmpl);
}

/*
 * Simple standard magic macros and functions.
 * Note that preprocessor functions (but obviously not zero-argument macros)
 * are allowed to recurse.
 */
static void pp_add_magic_simple(void)
{
    struct magic_macros {
        const char *name;
        bool casesense;
        int nparam;
        enum sparmflags flags;
        ExpandSMacro func;
    };
    static const struct magic_macros magic_macros[] = {
        { "__?FILE?__",  true, 0, 0, stdmac_file },
        { "__?LINE?__",  true, 0, 0, stdmac_line },
        { "__?BITS?__",  true, 0, 0, stdmac_bits },
        { "__?PTR?__",   true, 0, 0, stdmac_ptr },
        { "__?DEFAULT?__", true, 0, 0, stdmac_default },
        { "__?LIST_OPTIONS?__", true, 0, 0, stdmac_list_options },
        { "%abs",        false, 1, SPARM_EVAL, stdmac_abs },
        { "%chr",        false, 1, SPARM_EVAL|SPARM_OPTIONAL|SPARM_VARADIC, stdmac_chr },
        { "%count",      false, 1, SPARM_VARADIC, stdmac_count },
        { "%depend",     false, 1, SPARM_PLAIN, stdmac_depend },
        { "%eval",       false, 1, SPARM_EVAL|SPARM_VARADIC, stdmac_join },
        { "%hs2b",       false, 1, SPARM_STR|SPARM_CONDQUOTE|SPARM_VARADIC, stdmac_hs2b },
        { "%map",	 false, 1, SPARM_VARADIC, stdmac_map },
        { "%null",       false, 1, SPARM_GREEDY, stdmac_null },
        { "%pathsearch", false, 1, SPARM_PLAIN, stdmac_pathsearch },
        { "%realpath",	 false, 1, SPARM_PLAIN, stdmac_realpath },
        { "%selbits",    false, 1, SPARM_PLAIN|SPARM_VARADIC, stdmac_selbits },
        { "%str",        false, 1, SPARM_GREEDY|SPARM_STR, stdmac_join },
        { "%strcat",     false, 1, SPARM_STR|SPARM_CONDQUOTE|SPARM_VARADIC, stdmac_strcat },
        { "%strlen",     false, 1, SPARM_STR|SPARM_CONDQUOTE, stdmac_strlen },
        { "%tok",        false, 1, SPARM_STR|SPARM_CONDQUOTE, stdmac_tok },
    }, *m;
    SMacro tmpl;

    nasm_zero(tmpl);
    array_for_each(m, magic_macros) {
        tmpl.nparam = m->nparam;
        tmpl.expand = m->func;

        if (m->nparam) {
            int i;
            enum sparmflags flags = m->flags;

            nasm_newn(tmpl.params, m->nparam);
            for (i = m->nparam-1; i >= 0; i--) {
                tmpl.params[i].flags = flags;
                /* These flags for the last arg only */
                flags &= ~(SPARM_GREEDY|SPARM_VARADIC|SPARM_OPTIONAL);
            }
        }
        define_magic(m->name, m->casesense, &tmpl);
    }
}

/* %is...() macro functions */
static void pp_add_magic_isfunc(void)
{
    SMacro tmpl;
    enum preproc_token pt;
    char name_buf[PP_TOKLEN_MAX+1];

    nasm_zero(tmpl);
    tmpl.nparam  = 1;
    tmpl.expand  = stdmac_is;
    name_buf[0]  = '%';
    name_buf[1]  = 'i';
    name_buf[2]  = 's';
    for (pt = PP_IF; pt < (PP_IFN+(PP_IFN-PP_IF)); pt++) {
        if (!pp_directives[pt])
            continue;

        nasm_new(tmpl.params);
        tmpl.params[0].flags = SPARM_GREEDY;

        strcpy(name_buf+3, pp_directives[pt]+3);
        tmpl.expandpvt.u = pt;
        define_magic(name_buf, false, &tmpl);
    }
}

/* Message macro functions */
static void pp_add_magic_msgfunc(void)
{
#if 0    /* Disabled, because they seem to be expanded inappropriately */
    static const enum preproc_token msg_macros[] = {
        PP_NOTE,
        PP_WARNING,
        PP_ERROR,
        PP_FATAL
    }, *mm;
    SMacro tmpl;

    nasm_zero(tmpl);
    tmpl.nparam    = 1;
    tmpl.expand    = stdmac_user_error;
    array_for_each(mm, msg_macros) {
        nasm_new(tmpl.params);
        tmpl.params[0].flags = SPARM_GREEDY;
        tmpl.expandpvt.u = *mm;
        define_magic(pp_directives[*mm], false, &tmpl);
    }
#endif
}

/* Ad hoc preprocessor function definitions */
static void pp_add_magic_miscfunc(void)
{
    SMacro tmpl;
    int i;

    /* %hex() function */
    nasm_zero(tmpl);
    tmpl.nparam    = 1;
    tmpl.expand    =  stdmac_join;
    nasm_newn(tmpl.params, tmpl.nparam);
    tmpl.params[0].flags = SPARM_EVAL|SPARM_UNSIGNED|SPARM_VARADIC;
    tmpl.params[0].radix = 'x';
    define_magic("%hex", false, &tmpl);

    /* %sel() function */
    nasm_zero(tmpl);
    tmpl.nparam    = 2;
    tmpl.expand    = stdmac_sel;
    nasm_newn(tmpl.params, tmpl.nparam);
    tmpl.params[0].flags = SPARM_EVAL;
    tmpl.params[1].flags = SPARM_VARADIC;
    define_magic("%sel", false, &tmpl);

    /* %cond() function */
    nasm_zero(tmpl);
    tmpl.nparam = 3;
    tmpl.expand = stdmac_cond;
    nasm_newn(tmpl.params, tmpl.nparam);
    tmpl.params[0].flags = SPARM_EVAL;
    tmpl.params[1].flags = 0;
    tmpl.params[2].flags = SPARM_OPTIONAL;
    define_magic("%cond", false, &tmpl);

    /* %num() function */
    nasm_zero(tmpl);
    tmpl.nparam = 3;
    tmpl.expand = stdmac_num;
    tmpl.recursive = true;
    nasm_newn(tmpl.params, tmpl.nparam);
    tmpl.params[0].flags = SPARM_EVAL;
    tmpl.params[1].flags = SPARM_EVAL|SPARM_OPTIONAL;
    tmpl.params[1].def   = make_tok_num(NULL, -1);
    tmpl.params[2].flags = SPARM_EVAL|SPARM_OPTIONAL;
    tmpl.params[2].def   = make_tok_num(NULL, 10);
    define_magic("%num", false, &tmpl);

    /* %substr() function */
    nasm_zero(tmpl);
    tmpl.nparam = 3;
    tmpl.expand = stdmac_substr;
    tmpl.recursive = true;
    nasm_newn(tmpl.params, tmpl.nparam);
    tmpl.params[0].flags  = SPARM_STR|SPARM_CONDQUOTE;
    tmpl.params[1].flags  = SPARM_EVAL;
    tmpl.params[2].flags  = SPARM_EVAL|SPARM_OPTIONAL;
    tmpl.params[2].def    = make_tok_num(NULL, -1);
    define_magic("%substr", false, &tmpl);

    /* %ord() function */
    nasm_zero(tmpl);
    tmpl.nparam = 3;
    tmpl.expand = stdmac_ord;
    tmpl.recursive = true;
    nasm_newn(tmpl.params, tmpl.nparam);
    tmpl.params[0].flags  = SPARM_STR|SPARM_CONDQUOTE;
    tmpl.params[1].flags  = SPARM_EVAL|SPARM_OPTIONAL;
    tmpl.params[1].def    = make_tok_num(NULL, 1);
    tmpl.params[2].flags  = SPARM_EVAL|SPARM_OPTIONAL;
    tmpl.params[2].def    = make_tok_num(NULL, 1);
    define_magic("%ord", false, &tmpl);

    /* %b2hs() function */
    nasm_zero(tmpl);
    tmpl.nparam = 2;
    tmpl.expand = stdmac_b2hs;
    tmpl.recursive = true;
    nasm_newn(tmpl.params, tmpl.nparam);
    tmpl.params[0].flags  = SPARM_STR|SPARM_CONDQUOTE;
    tmpl.params[1].flags  = SPARM_STR|SPARM_CONDQUOTE|SPARM_OPTIONAL;
    tmpl.params[1].def    = make_tok_qstr_len(NULL, "", 0);
    define_magic("%b2hs", false, &tmpl);

    /* %limit() function */
    nasm_zero(tmpl);
    tmpl.nparam = 2;
    tmpl.expand = stdmac_limit;
    tmpl.recursive = true;
    nasm_newn(tmpl.params, tmpl.nparam);
    tmpl.params[0].flags  = SPARM_STR|SPARM_CONDQUOTE;
    tmpl.params[1].flags  = SPARM_STR|SPARM_CONDQUOTE|SPARM_OPTIONAL;
    define_magic("%limit", false, &tmpl);

    /* %env() function */
    for (i = 1; i <= 2; i++) {
        nasm_zero(tmpl);
        tmpl.nparam = i;
        tmpl.expand = stdmac_env;
        tmpl.recursive = true;
        nasm_newn(tmpl.params, tmpl.nparam);
        tmpl.params[0].flags  = SPARM_STR|SPARM_CONDQUOTE;
        if (i > 1)
            tmpl.params[1].flags  = SPARM_GREEDY;
        define_magic("%env", false, &tmpl);
    }

    /* %find[i]() functions */
    for (i = 0; i < 2; i++) {
        static const char * const names[] = { "%findi", "%find" };
        nasm_zero(tmpl);
        tmpl.nparam = 2;
        tmpl.expand = stdmac_find;
        tmpl.recursive = true;
        nasm_newn(tmpl.params, tmpl.nparam);
        tmpl.params[0].flags  = SPARM_STR|SPARM_CONDQUOTE;
        tmpl.params[1].flags  = SPARM_VARADIC|SPARM_OPTIONAL;
        tmpl.expandpvt.u = i;
        define_magic(names[i], false, &tmpl);
    }
}

static void pp_add_magic_stdmac(void)
{
    pp_add_magic_simple();
    pp_add_magic_isfunc();
    pp_add_magic_msgfunc();
    pp_add_magic_miscfunc();
}

static void pp_start_stdmac(void)
{
    struct Include *inc;

    stdmaclist = &stdmacset[0];

    /*
     * Set up the stdmac packages as a virtual include file,
     * indicated by a null file pointer.
     */
    nasm_new(inc);
    inc->next = istk;
    inc->nolist = inc->noline = !list_option('b');
    inc->where = istk->where;
    istk = inc;
    if (!istk->nolist) {
        lfmt->uplevel(LIST_INCLUDE, 0);
    }
    if (!istk->noline) {
        src_set(0, NULL);
        istk->where = src_where();
        if (ppdbg & PDBG_INCLUDE)
            dfmt->debug_include(true, istk->next->where, istk->where);
    }
}

static void pp_add_limits_stdmac(void)
{
    int i;
    Token *t = NULL;

    for (i = LIMIT_MAX-1; i > 0; i--) {
        t = make_tok_qstr(t, nasm_limit_name(i));
        t = make_tok_char(t, ',');
    }
    t = make_tok_qstr(t, nasm_limit_name(0));

    define_smacro("__?NASM_LIMITS?__", true, t, NULL);
}

static void pp_add_list_options_default_stdmac(void)
{
    define_smacro("__?LIST_OPTIONS_DEFAULT?__", true,
                  get_list_options(cmdline_list_options), NULL);
}

static void pp_reset_stdmac(enum preproc_mode mode)
{
    int apass;

    pp_start_stdmac();

    pp_add_magic_stdmac();

    if (tasm_compatible_mode)
        pp_add_stdmac(&nasm_stdmac_tasm);

    pp_add_stdmac(&nasm_stdmac_nasm);
    pp_add_stdmac(&nasm_stdmac_version);
    pp_add_stdmac(ofmt->stdmac);

    pp_add_limits_stdmac();

    pp_add_list_options_default_stdmac();
    do_predef = true;

    /*
     * Define the __?PASS?__ macro.  This is defined here unlike all the
     * other builtins, because it is special -- it varies between
     * passes -- but there is really no particular reason to make it
     * magic.
     *
     * 0 = dependencies only
     * 1 = preparatory passes
     * 2 = final pass
     * 3 = preprocess only
     */
    switch (mode) {
    case PP_NORMAL:
        apass = pass_final() ? 2 : 1;
        break;
    case PP_DEPS:
        apass = 0;
        break;
    case PP_PREPROC:
        apass = 3;
        break;
    default:
        panic();
    }

    define_smacro("__?PASS?__", true, make_tok_num(NULL, apass), NULL);
}

void pp_reset(const char *file, enum preproc_mode mode,
              struct strlist *dep_list)
{
    cstk = NULL;
    defining = NULL;
    nested_mac_count = 0;
    nested_rep_count = 0;
    unique = 0;
    masm_anon_seq = 0;          /* MASM @@/@F/@B labels: same names every pass */
    deplist = dep_list;
    pp_mode = mode;

    /* Reset options to default */
    nasm_zero(ppconf);

    /* Disable all debugging info, except in the last pass */
    ppdbg = 0;
    if (!(ppopt & PP_TRIVIAL)) {
        if (pass_final()) {
            if (dfmt->debug_mmacros)
                ppdbg |= PDBG_MMACROS;
            if (dfmt->debug_smacros)
                ppdbg |= PDBG_SMACROS;
            if (dfmt->debug_include)
                ppdbg |= PDBG_INCLUDE;
        }

        if (list_option('s'))
            ppdbg |= PDBG_LIST_SMACROS;
    }

    memset(use_loaded, 0, use_package_count * sizeof(bool));

    /* First set up the top level input file */
    nasm_new(istk);
    istk->fp = nasm_open_read(file, NF_TEXT);
    if (!istk->fp) {
	nasm_fatalf(ERR_NOFILE, "unable to open input file `%s'%s%s",
                    file, errno ? " " : "", errno ? strerror(errno) : "");
    }
    src_set(0, file);
    istk->where = src_where();
    istk->lineinc = 1;

    if (ppdbg & PDBG_INCLUDE) {
        /* Let the debug format know the main file */
        dfmt->debug_include(true, src_nowhere(), istk->where);
    }

    strlist_add(deplist, file);

    do_predef = false;

    if (!(ppopt & PP_TRIVIAL))
        pp_reset_stdmac(mode);
}

void pp_init(enum preproc_opt opt)
{
    ppopt = opt;
    nasm_newn(use_loaded, use_package_count);
}

static Include *pop_include_stack(void)
{
    Include *i = istk;

    if (i->fp)
        fclose(i->fp);
    if (i->conds) {
        /*
         * This should never happen for a builtin macro package,
         *  but if it does, at least get an error message out...
         */
        nasm_fatal("expected `%%endif' before end of %s",
                   i->fp ? "file" : "macro package");
    }

    istk = i->next;

    if (!i->nolist)
        lfmt->downlevel(LIST_INCLUDE);
    if (!i->noline) {
        struct src_location whereto
            = istk ? istk->where : src_nowhere();
        if (ppdbg & PDBG_INCLUDE)
            dfmt->debug_include(false, whereto, i->where);
        src_update(whereto);
    }

    return i;
}

/*
 * Get a line of tokens. If we popped the macro expansion/include stack,
 * we return a pointer to the dummy token tok_pop; at that point if
 * istk is NULL then we have reached end of input;
 */
static Token tok_pop;           /* Dummy token placeholder */

static Token *pp_tokline(void)
{
    while (true) {
        Line *l = istk->expansion;
        Token *tline = NULL;
        Token *dtline;
        char *line = NULL;
        bool suppressed = false;

        check_mmacro_refcounts();

        /*
         * Fetch a tokenized line, either from the macro-expansion
         * buffer or from the input file.
         */
        while (l && l->finishes) {
            MMacro *fm = l->finishes;

            if (!fm->name && fm->in_progress > 1 && !l->suppressed) {
                /*
                 * This is a macro-end marker for a macro with no
                 * name, which means it's not really a macro at all
                 * but a %rep block, and the `in_progress' field is
                 * more than 1, meaning that we still need to
                 * repeat. (1 means the natural last repetition; 0
                 * means termination by %exitrep.) We have
                 * therefore expanded up to the %endrep, and must
                 * push the whole block on to the expansion buffer
                 * again. We don't bother to remove the macro-end
                 * marker: we'd only have to generate another one
                 * if we did.
                 */
                fm->in_progress--;
                list_for_each(l, fm->expansion) {
                    Line *ll;

                    nasm_new(ll);
                    ll->next  = istk->expansion;
                    ll->first = dup_tlist(l->first, NULL);
                    ll->where = l->where;
                    istk->expansion = ll;
                }
                l = istk->expansion;
                continue;
            } else {
                MMacro *m = istk->mstk.mstk;

                nasm_assert(m == fm);

                /*
                 * Check whether a `%rep' was started and not ended
                 * within this macro expansion. This can happen and
                 * should be detected. It's a fatal error because
                 * I'm too confused to work out how to recover
                 * sensibly from it.
                 */
                if (defining) {
                    if (defining->name)
                        nasm_panic("defining with name in expansion");
                    else if (m->name)
                        nasm_fatal("`%%rep' without `%%endrep' within"
				   " expansion of macro `%s'", m->name);
                }

                /*
                 * FIXME:  investigate the relationship at this point between
                 * istk->mstk.mstk and fm
                 */
                if (m->name) {
                    /*
                     * This was a real macro call, not a %rep, and
                     * therefore the parameter information needs to
                     * be freed and the iteration count/nesting
                     * depth adjusted.
                     */

                    if (!--mmacro_deadman.levels) {
                        /*
                         * If all mmacro processing done,
                         * clear all counters and the deadman
                         * message trigger.
                         */
                        nasm_zero(mmacro_deadman); /* Clear all counters */
                    }

#if 0
                    if (m->prev) {
                        pop_mmacro(m);
                        fm->in_progress --;
                    } else
#endif
                    {
                        clear_mmacro(m);
                        fm->in_progress = 0;
                    }
                }

                if (fm->nolist & NL_LINE) {
                    istk->noline--;
                } else if (!istk->noline) {
                    MMacro *sm = (MMacro *)src_macro_current();
                    if (sm == fm) {
                        src_macro_pop();
                        put_mmacro(&sm);
                    }
                    src_update(l->where);
                }

                if (fm->nolist & NL_LIST) {
                    istk->nolist--;
                } else if (!istk->nolist) {
                    lfmt->downlevel(LIST_MACRO);
                    if ((ppdbg & PDBG_MMACROS) && fm->name)
                        debug_macro_end(fm);
                }

                istk->where = l->where;
                pop_mstk(&istk->mstk, m);
            }
            check_mmacro_refcounts();

            istk->expansion = l->next;
            free_line(l);

            return &tok_pop;
        }

        if (istk->expansion) {      /* from a macro expansion */
            Line *l = istk->expansion;

            check_mmacro_refcounts();

            istk->expansion = l->next;
            istk->where = l->where;
            suppressed = l->suppressed;

            if (!istk->noline)
                src_update(istk->where);

            tline = l->first;
            l->first = NULL;    /* Otherwise double free at free_line() */

            if (!istk->nolist && !suppressed) {
                char *listline;
                listline = detoken(tline, false);
                lfmt->line(LIST_MACRO, istk->where.lineno, listline);
                nasm_free(listline);
            }

            free_line(l);
        } else if ((line = read_line())) {
            tline = tokenize(line);
            nasm_free(line);
        } else if (istk->expansion) {
            /* read_line() might have modified istk->expansion */
            continue;
        } else {
            /*
             * The current file/input has ended; work down the istk
             */
            Include *i = pop_include_stack();


            put_mmacro(&i->mstk.mstk);
            put_mmacro(&i->mstk.mmac);
            nasm_free(i);
            return &tok_pop;
        }

        /*
         * If in a non-emitting branch, suppress this output line
         */
        suppressed |= istk->conds && !emitting(istk->conds->state);

        /*
         * We must expand MMacro parameters and MMacro-local labels
         * _before_ we plunge into directive processing, to cope
         * with things like `%define something %1' such as STRUC
         * uses. Unless we're _defining_ a MMacro, in which case
         * those tokens should be left alone to go into the
         * definition; and unless we're in a non-emitting
         * condition, in which case we don't want to meddle with
         * anything.
         */
        if (!defining && !suppressed)
            tline = expand_mmac_params(tline);

        /*
         * Check the line to see if it's a preprocessor directive.
         */
        if (do_directive(tline, &dtline, suppressed) == DIRECTIVE_FOUND) {
            if (dtline)
                return dtline;
        } else if (defining) {
            /*
             * We're defining a multi-line macro. We emit nothing
             * at all, and just
             * shove the tokenized line on to the macro definition.
             */
            MMacro *mmac = defining->dstk.mmac;
            Line *l;

            nasm_new(l);
            l->next = defining->expansion;
            l->first = tline;
            l->finishes = NULL;
            l->where = istk->where;
            defining->expansion = l;

            /*
             * Remember if this mmacro expansion contains %00:
             * if it does, we will have to handle leading labels
             * specially.
             */
            if (mmac) {
                const Token *t;
                list_for_each(t, tline) {
                    if (t->type == TOKEN_MMACRO_PARAM &&
                        !memcmp(t->text.a, "%00", 4))
                        mmac->capture_label = true;
                }
            }
        } else if (suppressed) {
            /*
             * We're in a non-emitting branch of a condition block,
             * or a %rep block or macro that has been terminated.
             * Emit nothing at all, not even a blank line: when we
             * emerge from the condition we'll give a line-number
             * directive so we keep our place correctly.
             */
            delete_tlist(tline);
        } else if (masm_mode) {
            /*
             * MASM passes multi-line-macro arguments as unexpanded TEXT (a
             * defined symbol used as an argument stays its name, not its value).
             * So try to expand a macro on the raw tokens first; only when the
             * line is not a macro call do we expand single-line macros (needed
             * for an ordinary instruction line) and retry.
             */
            if (!expand_mmacro(tline)) {
                tline = expand_smacro(tline);
                if (!expand_mmacro(tline))
                    return tline;
            }
        } else {
            tline = expand_smacro(tline);
            if (!expand_mmacro(tline))
                return tline;
        }
    }
}

char *pp_getline(void)
{
    char *line = NULL;
    Token *tline;

    while (true) {
        tline = pp_tokline();
        if (tline == &tok_pop) {
            /*
             * We popped the macro/include stack. If istk is empty,
             * we are at end of input, otherwise just loop back.
             */
            if (!istk)
                break;
        } else {
            /*
             * De-tokenize the line and emit it.
             */
            line = detoken(tline, true);
            delete_tlist(tline);
            break;
        }
    }

    if (list_option('e') && istk && !istk->nolist && line && line[0]) {
        char *buf = nasm_strcat(" ;;; ", line);
        lfmt->line(LIST_MACRO, -1, buf);
        nasm_free(buf);
    }

    return line;
}

void pp_cleanup_pass(void)
{
    if (defining) {
        if (defining->name) {
            nasm_nonfatal("end of input while still defining macro `%s'",
                          defining->name);
        } else {
            nasm_nonfatal("end of input while still in %%rep");
        }

        if (defining->refcnt != 0)
        {
            /* This can happen if a macro is ill formed without a correct ending
             * and the macro is still referenced.
             * In this case, we just set the refcnt to 0 to avoid a memory leak.
             */
            defining->refcnt = 0;
        }

        free_mmacro(defining);
        defining = NULL;
    }

    while (cstk)
        ctx_pop();
    free_macros();
    while (istk)
        nasm_free(pop_include_stack());

    if (ppdbg & PDBG_MMACROS)
        debug_macro_output();
}

void pp_cleanup_session(void)
{
    nasm_free(use_loaded);
    free_llist(predef);
    predef = NULL;
    free_Blocks();
    ipath_list = NULL;
}

void pp_include_path(struct strlist *list)
{
    ipath_list = list;
}

void pp_pre_include(char *fname)
{
    Token *inc, *space, *name;
    Line *l;

    name = new_Token(NULL, TOKEN_INTERNAL_STR, fname, 0);
    space = new_White(name);
    inc = new_Token(space, TOKEN_PREPROC_ID, "%include", 0);

    l = nasm_malloc(sizeof(Line));
    l->next = predef;
    l->first = inc;
    l->finishes = NULL;
    predef = l;
}

void pp_pre_define(char *definition)
{
    Token *def, *space;
    Line *l;
    char *equals;

    equals = strchr(definition, '=');
    space = new_White(NULL);
    def = new_Token(space, TOKEN_PREPROC_ID, "%define", 0);
    if (equals)
        *equals = ' ';
    space->next = tokenize(definition);
    if (equals)
        *equals = '=';

    nasm_new(l);
    l->next = predef;
    l->first = def;
    l->finishes = NULL;
    predef = l;
}

void pp_pre_undefine(char *definition)
{
    Token *def, *space;
    Line *l;

    space = new_White(NULL);
    def = new_Token(space, TOKEN_PREPROC_ID, "%undef", 0);
    space->next = tokenize(definition);

    nasm_new(l);
    l->next = predef;
    l->first = def;
    l->finishes = NULL;
    predef = l;
}

/* Insert an early preprocessor command that doesn't need special handling */
void pp_pre_command(const char *what, char *string)
{
    Token *def;
    Line *l;

    def = tokenize(string);

    if (what) {
        Token *wt = tokenize(what);
        Token **tailp = tlist_endptr(&wt);

        *tailp = new_White(def);
        def = wt;
    }

    nasm_new(l);
    l->next = predef;
    l->first = def;
    l->finishes = NULL;
    predef = l;
}

static void pp_add_stdmac(macros_t *macros)
{
    macros_t **mp;

    /* Find the end of the list and avoid duplicates */
    for (mp = stdmacset; *mp; mp++) {
        if (*mp == macros)
            return;             /* Nothing to do */
    }

    nasm_assert(mp < &stdmacset[ARRAY_SIZE(stdmacset)-1]);

    *mp = macros;
}

/* Create a numeric token, with possible - token in front */
static Token *make_tok_num(Token *next, int64_t val)
{
    char numbuf[32];
    int len;
    uint64_t uval;
    bool minus = val < 0;

    uval = minus ? -val : val;

    len = snprintf(numbuf, sizeof numbuf, "%"PRIu64, uval);
    next = new_Token(next, TOKEN_NUM, numbuf, len);

    if (minus)
        next = make_tok_char(next, '-');

    return next;
}

/*
 * Create a numeric token with specified radix and signedness;
 * prefix the number with 0<radix> if a radix letter is specified,
 * otherwise generate a decimal constant without prefix.
 */
static Token *
make_tok_num_radix(Token *next, int64_t val, char radix, bool uns)
{
    char numbuf[2+64+1]; /* Maximum possible: 0b + binary + null */
    char *p;
    uint64_t uval;
    bool minus = val < 0 && !uns;
    unsigned int base;
    bool upper;

    uval = minus ? -val : val;

    p = numbuf;
    base = 10;
    upper = false;
    if (radix) {
        *p++ = '0';
        *p++ = radix;

        base = radix_letter(radix);
        upper = !(radix & 0x20);
    }

    p += numstr(p, sizeof(numbuf)-2, uval, -1, base, upper);
    next = new_Token(next, TOKEN_NUM, numbuf, p - numbuf);

    if (minus)
        next = make_tok_char(next, '-');

    return next;
}

/*
 * Do the inverse of make_tok_num(). This only needs to be able
 * to parse the output of make_tok_num() or make_tok_hex().
 */
static int64_t get_tok_num(const Token *t, bool *err)
{
    bool minus = false;
    int64_t v;

    if (tok_is(t, '-')) {
        minus = true;
        t = t->next;
    }
    if (!tok_is(t, TOKEN_NUM)) {
        if (err)
            *err = true;
        return 0;
    }

    v = readnum(tok_text(t), err);
    return minus ? -v : v;
}

/* Create a quoted string token */
static Token *make_tok_qstr_len(Token *next, const char *str, size_t len)
{
    char *p = nasm_quote(str, &len);
    return new_Token_free(next, TOKEN_STR, p, len);
}
static Token *make_tok_qstr(Token *next, const char *str)
{
    return make_tok_qstr_len(next, str, strlen(str));
}

/* Create a single-character operator token */
static Token *make_tok_char(Token *next, char op)
{
    Token *t = new_Token(next, op, NULL, 1);
    t->text.a[0] = op;
    return t;
}

/*
 * Descent the macro hierarchy and display the expansion after
 * encountering an error message.
 */
void pp_error_list_macros(errflags severity)
{
    const MMacro *m;

    severity |= ERR_PP_LISTMACRO | ERR_NO_SEVERITY | ERR_HERE;

    while ((m = src_error_down())) {
        if ((m->nolist & NL_LIST) || !m->where.filename)
            break;
	nasm_error(severity, "... from macro `%s' defined", m->name);
    }

    src_error_reset();
}

#if DEBUG_MMACRO_REFCOUNTS

static inline void mmac_dbgref(MMacro *m)
{
    if (m)
        m->refdbg.cnt++;
}

static void mmac_hashref(struct hash_table *mmt)
{
    struct hash_iterator it;
    const struct hash_node *np;

    hash_for_each(mmt, it, np)
        mmac_dbgref(np->data);
}

/* Scan everything and compute correct refcnts, then warn on discrepancies */
static void check_mmacro_refcounts(void)
{
    MMacro *m;
    Include *i;
    Line *l;
    bool err;

    for (m = refdbg_list; m; m = m->refdbg.next)
        m->refdbg.cnt = 0;

    for (m = refdbg_list; m; m = m->refdbg.next) {
        mmac_dbgref(m->next);
        mmac_dbgref(m->mstk.mstk);
        mmac_dbgref(m->mstk.mmac);
        mmac_dbgref(m->dstk.mstk);
        mmac_dbgref(m->dstk.mmac);
    }

    mmac_hashref(&mmacros);

    mmac_dbgref(defining);

    for (i = istk; i; i = i->next) {
        mmac_dbgref(i->mstk.mstk);
        mmac_dbgref(i->mstk.mmac);
        for (l = i->expansion; l; l = l->next)
            mmac_dbgref(l->finishes);
    }

    mmac_dbgref((MMacro *)src_macro_current());

    err = false;
    for (m = refdbg_list; m; m = m->refdbg.next) {
        if (m->refcnt != m->refdbg.cnt) {
            nasm_info("macro %s @ %p refcnt %zu should be %zu\n",
                      m->name ? m->name : "<rep>", (void *)m,
                      m->refcnt, m->refdbg.cnt);
            err = true;
        }
    }

    if (err)
        panic();
}

#endif
