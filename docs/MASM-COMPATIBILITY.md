<!-- Written by: Logan Greer -->
# MASM compatibility reference

This fork adds a `--masm` flag that makes NASM assemble Microsoft Macro
Assembler (MASM) source. This document lists what the mode supports, what it
does not, and how it is put together. Everything below applies **only** under
`--masm`; without the flag the binary is stock NASM, unchanged.

The reference dialect is **MASM 6.11** (behaviour is also checked against MASM
6.0 and 6.14). Coverage runs from the early-1990s 16-bit segmented / `cmacros`
style through the 32-bit `.MODEL flat` authoring dialect.

---

## Supported

### Segments and memory model
- Full segment directives: `NAME SEGMENT [attrs]` / `NAME ENDS`, with `USE16` /
  `USE32`, class/align/combine attributes, and correct **nesting** (`ENDS`
  resumes the enclosing segment). A segment's USE size is fixed at definition.
- `GROUP`, `ASSUME` (accepted; segment-register assumptions carry no encoding).
- The `END` directive terminates the module.
- Simplified segments: `.MODEL` (`flat` and the segmented models), `.CODE`,
  `.DATA`, `.CONST`, `.STACK`.
- The `.8086`/`.286`/`.386`/`.486`/`.586`/`.686[p]` CPU directives and the FPU
  and listing directives (accepted; the CPU level does not restrict encoding).

### Procedures and calling
- `PROC` / `ENDP` / `RET`, framed or frameless, with `stdcall` / `c` / `pascal`
  calling conventions and stack cleanup.
- `LOCAL` stack variables and `USES` saved-register lists.
- `INVOKE`, `PROTO`, and `ADDR`.

### Data definition and the type system
- `STRUCT`/`ENDS`, `UNION`, `RECORD` (bit fields), and `TYPEDEF`.
- Static structure/union/record instances with positional `<...>` initialisers,
  including `DUP` and expression member counts.
- MASM **typed-label semantics**: a bare data label reads as its *contents*
  (a sized memory reference), while `OFFSET label` yields its address.

### Control flow
- `.IF` / `.ELSEIF` / `.ELSE` / `.ENDIF`, `.WHILE` / `.ENDW`,
  `.REPEAT` / `.UNTIL`, with `.BREAK` / `.CONTINUE`.
- Signed and unsigned comparisons, `&&` / `||`, and the flag conditions
  (`CARRY?`, `ZERO?`, `SIGN?`, `OVERFLOW?`, `PARITY?`).

### Macros and text processing
- `MACRO` / `ENDM` with optional and variadic parameters, `LOCAL` macro labels,
  and `EXITM`.
- `REPT`/`REPEAT`, `FOR`/`IRP`, `FORC`/`IRPC`.
- The `&` substitution operator for name construction (`ln&OFFSET`) and the
  string functions `CATSTR`, `SUBSTR`, `INSTR`, `SIZESTR`.

### Conditional assembly and diagnostics
- The full `IF` family: `IF`/`IFE`, `IFDEF`/`IFNDEF` (true for `EQU`/`=`
  constants, not only macros), `IFB`/`IFNB`, `IFIDN[I]`/`IFDIF[I]`, `IF1`/`IF2`,
  `ELSEIF`, `ELSE`, `ENDIF`.
- `.ERR` / `.ERRE` / `.ERRNZ` assertions; `ECHO` / `%OUT` accepted.
- An undefined symbol used in an `IF` expression evaluates to 0 (as in MASM).

### Operators and expressions
- `OFFSET` (anywhere in an expression), `SEG`, `SIZE`/`SIZEOF`, `TYPE`, `PTR`,
  `LOW`/`HIGH`/`LOWWORD`/`HIGHWORD`, `MASK`/`WIDTH`.
- The MASM word operators `EQ` `NE` `LT` `GT` `LE` `GE` `MOD` `SHL` `SHR`
  `AND` `OR` `XOR` `NOT`.
- `EQU` (text, numeric, and bare size-cast aliases such as `wptr EQU word ptr`)
  and `=` numeric equates, including forward references in their value
  expressions.
- Parentheses may wrap a whole operand as grouping (`mov cx, ( count )`).

### Instruction encoding (byte parity)
Under `--masm`, NASM matches `ML.EXE`'s encoding choices so the emitted code
bytes are identical, including: the reg,reg direction bit (`xor eax, eax` →
`33 c0`, not `31 c0`); 16-bit accumulator-immediate forms; `-O1` jump/immediate
sizing; `[reg*2]` SIB scaling; the `66` prefix on segment-register memory moves;
and omission of a redundant `ds:` segment override on a default-DS operand.

### Case-insensitive symbols
MASM is case-insensitive. Under `--masm`, every symbol folds to a canonical case
so `CurTDB` and `curTDB` are the same symbol — across labels, `EQU`/`=`
constants, macro names, and struct/record/typedef fields — while stock NASM
stays case-sensitive. Casing never affects the emitted bytes.

### Win16 `cmacros.inc`
The 16-bit `cProc` / `parmX` / `localX` / `cBegin` / `cEnd` / `cCall` / `sBegin` /
`createSeg` / `defGrp` procedure and segment conventions are supported, so
`cmacros`-based Windows source assembles with byte-correct 16-bit frames.

---

## Not supported / partial

These are deliberate boundaries — either they do not map cleanly onto NASM's
model, or they are out of scope for functional source compatibility.

- **`&` substitution inside a string literal** (`IRPC c,<abc>` / `db '&c'`).
  NASM string literals do not expand macro parameters, and the out-of-string
  paste form would be ambiguous with the bitwise `&` that `.IF a & b` relies on.
  `&` name construction *outside* strings works; character iteration works.
- **A nested-aggregate initialiser** — a struct field that is itself another
  struct given an inline `<...>` initialiser. Scalar, array, and pointer members
  work; a nested-struct *initialiser* does not.
- **Object *record-level* parity with `ML.EXE`.** The emitted *code bytes* match,
  and the OMF/COFF backends are exercised by the test suite, but the exact `.obj`
  file-record layout (SEGDEF/FIXUPP/class/align ordering) is not a goal.
- **Encoding-choice ties.** Fidelity is *functional* MASM compatibility. In the
  rare cases where MASM and NASM each pick an equally valid but different
  encoding (e.g. SIB base/index ordering for two-register addressing), the
  result is functionally identical but not byte-identical, and this is expected.

---

## How `--masm` is structured

The MASM handling is layered, from the most isolated to the deepest, and is
**entirely gated on `masm_mode`** so upstream NASM behaviour is never changed:

| Layer | File(s) | Responsibility |
|---|---|---|
| Macro package | `macros/masm.mac` | Auto-loaded by `--masm`; pure syntax mappings, CPU/segment/`.MODEL` directives, the authoring dialect (`PROC`/`.IF`/…). |
| Front-end translator | `asm/preproc.c` | Rewrites MASM lines to NASM before the preprocessor: `MACRO`/`REPT`, conditional assembly, `STRUCT`/`UNION`/`RECORD`, `EQU`, operator rewrites, the `cmacros` interface. |
| Tokenizer / parser | `asm/stdscan.c`, `asm/parser.c` | MASM operators and keywords, `PTR`/size inference, struct-member access, typed-label-to-memory conversion, operand-paren grouping. |
| Evaluator | `asm/eval.c` | `OFFSET`/`SEG` semantics, undefined-in-`IF`-is-0. |
| Encoder | `asm/assemble.c` | Byte-parity encoding preferences (the reg,reg direction family, accumulator immediates, prefix elision, …). |
| Symbols | `asm/labels.c` | Case-insensitive symbol folding. |
| Backends | `output/outobj.c`, `output/outcoff.c` | 16-bit OMF and 32-bit COFF object output. |
| Driver | `asm/nasm.c` | The `--masm` flag and `masm_mode` global. |
| cmacros shim | `test/masm/cmacros_shim.inc` | A NASM-native reimplementation of the Win16 `cmacros.inc` machinery, used when assembling real `cmacros`-based source. |

---

## Development history

The full build-out record — the original phased plan, decisions, and progress
log — is kept for provenance in [`HISTORY.md`](HISTORY.md) and
[`../test/masm/HISTORY.md`](../test/masm/HISTORY.md). Those are development
diaries, not current reference material; this file is the reference.
