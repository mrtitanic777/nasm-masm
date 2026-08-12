# MASM-mode regression tests

Locks in the byte-level behaviour of `nasm --masm` (the MASM 6.x
source-compatibility mode). See [`../../docs/MASM-COMPAT-ROADMAP.md`](../../docs/MASM-COMPAT-ROADMAP.md).

## Run

```sh
python run.py                    # check every fixture; exit 1 on any mismatch
python run.py --update           # regenerate golden/*.hex after an intended change
python run.py --nasm ../../nasm  # pick the nasm binary (default: ../../nasm[.exe])
```

`run.py` assembles each `fixtures/*.asm` with `nasm --masm -f bin` and compares
the emitted bytes to `golden/<name>.hex`.

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

## Adding a fixture

1. Write `fixtures/<name>.asm` (clean MASM; annotate the expected bytes in the
   header for humans).
2. `python run.py --update` to write `golden/<name>.hex`.
3. Eyeball the golden against the documented encoding, then commit both files.
