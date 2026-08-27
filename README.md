NASM, the Netwide Assembler
===========================

[![master](https://travis-ci.org/netwide-assembler/nasm.svg?branch=master)](https://travis-ci.org/netwide-assembler/nasm)

Many many developers all over the net respect NASM for what it is:
a widespread (thus netwide), portable (thus netwide!), very flexible
and mature assembler tool with support for many output formats (thus netwide!!).

Now we have good news for you: NASM is licensed under the "simplified"
[(2-clause) BSD license](https://opensource.org/licenses/BSD-2-Clause).
This means its development is open to even wider society of programmers
wishing to improve their lovely assembler.

Visit our [nasm.us](https://www.nasm.us/) website for more details.

With best regards, the NASM crew.

---

## This fork: `--masm` — MASM source compatibility

This is a fork of NASM that adds a **`--masm`** mode: NASM assembles
Microsoft-MASM–dialect source directly, so vintage and modern MASM code can be
built with an open, cross-platform assembler instead of `ML.EXE`. Every
`--masm` behaviour is gated on the flag — **without `--masm`, this binary is
stock NASM, byte-for-byte.**

**What it does**

- **Instruction encoding parity.** Under `--masm`, NASM matches `ML.EXE`'s
  encoding choices (reg,reg direction, accumulator-immediate forms, jump
  sizing, redundant-prefix elision, …). Validated **byte-exact on a
  539-fragment Win95 VxD disassembly corpus** (100%).
- **Full MASM directive & dialect coverage.** Segments (`SEGMENT`/`ENDS` with
  `USE16`/`USE32` and nesting), groups, simplified segments (`.MODEL`/`.CODE`),
  `PROC`/`ENDP`/`INVOKE`/`PROTO`/`LOCAL`/`USES`, `STRUCT`/`UNION`/`RECORD`/
  `TYPEDEF`, `.IF`/`.WHILE`/`.REPEAT`, `MACRO`/`REPT`/`FOR`/`IRP`, the full
  conditional-assembly and string-function families, `OFFSET`/`SIZE`/`TYPE`/
  `PTR` and the MASM word operators, `EQU` text/numeric/size-cast aliases, and
  the Win16 `cmacros.inc` procedure/segment model. The authoring-dialect
  constructs are byte-validated against `ML.EXE`.
- **Real-world source.** Assembles real Microsoft DDK/SDK headers
  (`windows.inc`) and Win3.1 `cmacros`-based driver/kernel source.

**Validation.** `test/masm/` holds a golden-byte regression suite (85+ clean,
hand-written fixtures, each pinned to this fork's verified MASM-parity output)
plus the corpus checker. Run `python test/masm/run.py`. See
[`test/masm/README.md`](test/masm/README.md) and the roadmap
[`docs/MASM-COMPAT-ROADMAP.md`](docs/MASM-COMPAT-ROADMAP.md).

**Scope & limits.** Fidelity is *functional MASM syntax* validated by corpus
byte-parity, not byte-parity on encoding-choice ties (documented in the
roadmap). Object-*record* byte-parity against `ML.EXE` (as opposed to code-byte
parity, which holds) is a separate goal. Assembling a full driver/kernel tree
additionally needs that tree's own `cmacros`/header environment; the assembler
itself is complete for the validated scope above.
