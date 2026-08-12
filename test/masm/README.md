# MASM-mode regression tests

Locks in the byte-level behaviour of `nasm --masm` (the MASM 6.x
source-compatibility mode). See [`../../docs/MASM-COMPAT-ROADMAP.md`](../../docs/MASM-COMPAT-ROADMAP.md).

## Run

```sh
python run.py                    # check every fixture; exit 1 on any mismatch
python run.py --objects          # also check -f obj (OMF) and -f win32 (COFF)
python run.py --update           # regenerate golden/*.hex after an intended change
python run.py --nasm ../../nasm  # pick the nasm binary (default: ../../nasm[.exe])
```

`run.py` assembles each `fixtures/*.asm` with `nasm --masm -f bin` and compares
the emitted bytes to `golden/<name>.hex`.

With `--objects`, each MASM-segment fixture (`directives`, `objseg`) plus the
relocation-free `hll_struct` is also assembled to `-f obj` (OMF) and `-f win32`
(COFF); a self-contained reader pulls the `_TEXT` code back out and asserts it
equals the `-f bin` golden. This proves the `NAME segment ... use32 ...`
scaffolding — and the simplified-segment `.MODEL`/`.CODE` path — yield a
faithful 32-bit code segment in the object backends, not only in flat `-f bin`.
`hll_struct` specifically guards the STRUCT-then-`.CODE` USE32 path: a
`struc`/`endstruc` resets NASM's live BITS to 16, so a regression there would
re-add `66` prefixes and fail the byte compare. (Full OBJ-*record* byte-parity
against `ML.EXE` is a separate, oracle-gated goal — see the roadmap; here we
prove the code path through each backend.)

## Fixtures

Small, **clean, hand-written** MASM — no third-party or OS-derived material. The
golden bytes capture this fork's verified MASM-parity output, so any regression
in a `--masm` behaviour fails the corresponding fixture.

| fixture | behaviour under test |
|---|---|
| `regdir` | reg,reg direction — ML's `reg, r/m` (d=1) ALU/MOV form (`add eax,ebx` → `03 c3`) |
| `accum` | 16-bit accumulator immediate (`and ax,3fh` → `66 25 3f 00`); 8/32-bit unchanged |
| `jumps` | `-O1` default — imm8 sign-extension, forward branches stay near, `short` honored |
| `strings` | MASM documentation operands on `movs/stos/lods/scas/cmps/ins/outs`; SSE `movsd`/`cmpsd` preserved |
| `sib` | `[reg*2]` kept as index*2+disp32; summed `[reg+reg]` still folds |
| `sreg` | redundant `66` on segment-register memory moves |
| `ptrseg` | `<size> ptr [mem]` and `seg:[mem]` overrides |
| `directives` | the directive scaffolding (`title/page/.386/segment/assume/ends/end`) end to end |
| `rawbits` | a raw `bits 32` (no `.386`/`.MODEL`) before a `segment` still yields USE32 in the object backends |

Authoring-dialect fixtures (Track B — the high-level MASM constructs used to
*write* code) lock in the same behaviour that was validated byte-identical to
real ML 6.11:

| fixture | behaviour under test |
|---|---|
| `hll_data` | data-label semantics — a label means its contents (`mov eax,val`→`a1`), `OFFSET`, size inference |
| `hll_proc` | `PROC`/`INVOKE` — frame, params `[ebp+8+4n]`, `leave; ret N`, push-and-call (`= ML`) |
| `hll_local` | `PROC USES` + `LOCAL` — register save/restore, `[ebp-N]` locals |
| `hll_flow` | `.IF`/`.ELSE`/`.WHILE`/`.REPEAT`/`.BREAK` (golden is the `-O1` default: near jumps; `-Ox` gives ML's short jumps) |
| `hll_ifchain` | `.ELSEIF` chains, flag conditions (`CARRY?`/`ZERO?`/`!ZERO?`/`SIGN?`), signed compares (`SDWORD PTR`) — `-Ox` = ML |
| `hll_andor` | boolean `&&` (AND) / `||` (OR) with short-circuit lowering in `.IF` — `-Ox` = ML |
| `hll_macro` | `MACRO`/`ENDM`, `REPT`, `TEXTEQU`, `=` (`= ML`) |
| `hll_for` | `FOR`/`IRP` list iteration and `EXITM` (`= ML`) |
| `hll_strfn` | string functions `SIZESTR`/`CATSTR`/`SUBSTR` (compile-time text → equates, `= ML`) |
| `hll_instr` | `INSTR` substring-search position (0 if absent, optional start) — `= ML` |
| `hll_cond` | `IF`/`IFE`/`IFDEF`/`IFNDEF`/`ELSE`/`ELSEIF`/`ENDIF` assembly-time conditionals (`= ML`) |
| `hll_ifb` | `IFB`/`IFNB`, `IFIDN`/`IFDIF` text conditionals, optional macro params (`= ML`) |
| `hll_struct` | `STRUCT`/`ENDS` (incl. `DUP` arrays, nested), `[reg].STRUCT.member`, `SIZEOF STRUCT` (`= ML`) |
| `hll_struct_inst` | static instances `label S <i,...>` + instance-member access `label.m` (load/store/word/rmw = ML) |
| `hll_union` | `UNION` types — members overlap at offset 0, `SIZEOF` = largest member (`= ML`) |
| `hll_typedef` | `TYPEDEF` aliases — primitive and `PTR`/`PROTO` (pointer=4); usable as a data type + `SIZEOF` (`= ML`) |
| `hll_record` | `RECORD` bit-packed fields — field=shift, `MASK`/`WIDTH` operators, instance packing (`= ML`) |
| `hll_expr` | expression operators `TYPE`/`SIZEOF` (struct + primitive), `LOW`/`HIGH`/`LOWWORD`/`HIGHWORD`, radix `1010b`/`17q` (`= ML`) |

Each `.asm` header explains the behaviour and annotates the expected bytes
inline; `golden/*.hex` is the authoritative comparison.

## Validating against a real disassembly corpus

The fork was developed against a byte-exact Windows 95 VxD disassembly corpus
whose lines carry their original bytes in `; offset bytes` comments. That corpus
is **not** part of this repository (it is a derivative of third-party binaries).
Point the harness at a local copy to reassemble every fragment and diff against
its inline ground truth:

```sh
python run.py --corpus /path/to/win95/src/asm/core
```

Reported: fragments, assemble errors, byte-exact count, and total byte match.
As of the current tree: **539/539 fragments, 100.00% byte-exact.**

## Differential oracle vs real ML.EXE

For the authoring dialect (Track B) there is no byte ground truth, so
`ml_oracle.py` drives a real Microsoft `ML.EXE` and diffs its object code
against `nasm --masm`, per file. ML 6.11c/6.13/6.14 are Win32 and run natively
(no DOSBox):

```sh
python ml_oracle.py --ml /path/to/masm611c/ML.EXE fixtures/objseg.asm
python ml_oracle.py --ml /path/to/ML.EXE --corpus /path/to/win95/src/asm/core
```

Documented findings from wiring this up:
- On the Win95 VxD corpus, **`nasm --masm` matches the shipped bytes more
  faithfully than real ML does** — ML 6.0 and 6.11c both diverge on a few SIB
  base/index orderings of two-register addressing (they encode `[eax+esi]` with
  base=esi; the shipped code, and nasm, use base=eax). Functionally identical;
  the corpus ground truth is authoritative.
- **`nasm --masm` is deliberately more lenient than ML** on the machine-
  generated corpus dialect — e.g. real ML rejects `stosd dword ptr es:[edi],eax`
  ("too many operands"), which nasm accepts so it can assemble the disassembly
  corpus that ML itself cannot.

## Adding a fixture

1. Write `fixtures/<name>.asm` (clean MASM; annotate the expected bytes in the
   header for humans).
2. `python run.py --update` to write `golden/<name>.hex`.
3. Eyeball the golden against the documented encoding, then commit both files.
