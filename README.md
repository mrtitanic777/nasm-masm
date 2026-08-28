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

## `--masm` — assemble MASM source with NASM

This is a fork of NASM with **one added feature**: a `--masm` flag that makes
NASM understand **Microsoft Macro Assembler (MASM) syntax**. It lets you build
`.asm` files written for Microsoft's `ML.EXE` with a free, open-source,
cross-platform assembler instead — on Linux, macOS, or Windows.

**Without `--masm`, this is ordinary NASM**, byte-for-byte. The flag is the only
difference, and it does nothing unless you pass it.

### Quick start

Build it exactly like upstream NASM. From a git checkout:

```sh
sh autogen.sh      # generate the configure script (first time only)
./configure
make               # produces the `nasm` binary
```

(On Windows, build under MSYS2/MinGW, or use the project files in `Mkfiles/` —
same as stock NASM.)

Then assemble a MASM source file by adding `--masm` and choosing an output
format with `-f`:

```sh
nasm --masm -f win32 hello.asm -o hello.obj    # 32-bit COFF object
nasm --masm -f obj   hello.asm -o hello.obj    # 16-bit OMF object
nasm --masm -f elf32 hello.asm -o hello.o      # 32-bit ELF (Linux)
nasm --masm -f bin   hello.asm -o hello.bin    # flat binary, no object headers
```

NASM only *assembles* — link the resulting object with whatever linker you
already use (`link.exe`, `ld`, `gcc`, …).

A minimal example (`add.asm`):

```asm
        .386
        .model flat
        .code
add_two proc
        mov     eax, ecx
        add     eax, edx
        ret
add_two endp
        end
```

```sh
nasm --masm -f win32 add.asm -o add.obj
```

### Important: bring the project's own includes

`--masm` understands the MASM *language and encoding*. Assembling a real-world
codebase still needs **that project's own include files** — its `cmacros.inc`,
`windows.inc`, and any other headers it `include`s — present on disk, exactly as
`ML.EXE` would require. This tool understands the syntax; it does not ship
anyone else's source. Point NASM at your include directories with `-I`:

```sh
nasm --masm -I ./inc -f obj driver.asm -o driver.obj
```

### What MASM syntax is supported

- **Segments & model:** `NAME SEGMENT`/`ENDS` (with `USE16`/`USE32` and nesting),
  `GROUP`, `ASSUME`, and the simplified `.MODEL` / `.CODE` / `.DATA` directives.
- **Procedures:** `PROC`/`ENDP`/`RET`, `INVOKE`, `PROTO`, `LOCAL`, `USES`.
- **Data types:** `STRUCT`/`UNION`/`RECORD`/`TYPEDEF` and typed data labels
  (a bare data label reads as its contents, as in MASM).
- **Control flow:** `.IF`/`.ELSE`/`.ELSEIF`/`.ENDIF`, `.WHILE`, `.REPEAT`.
- **Metaprogramming:** `MACRO`/`ENDM`, `REPT`, `FOR`/`IRP`, the full
  `IF`/`IFDEF`/`IFB`/`IFIDN`/… conditional-assembly family, and the string
  functions (`CATSTR`, `SUBSTR`, `INSTR`, `SIZESTR`).
- **Operators:** `OFFSET`, `SIZE`/`SIZEOF`, `TYPE`, `PTR`, `LOW`/`HIGH`, the MASM
  word operators (`EQ`/`NE`/`LT`/`SHL`/`AND`/…), and `EQU` text/numeric aliases.
- **Win16 `cmacros.inc`** procedure/segment conventions (`cProc`/`cBegin`/
  `cCall`/`sBegin`/`createSeg`/…).

It also reproduces **MASM's instruction encoding**, which differs from NASM's
defaults in a few places (e.g. `xor eax, eax` assembles to `33 c0` as MASM does,
not NASM's `31 c0`), so the emitted code bytes match what `ML.EXE` produces.

### Compatibility target

The reference dialect is **MASM 6.11** (behaviour is also checked against MASM
6.0 and 6.14). Coverage runs from the older 16-bit segmented / `cmacros` style
of the early-1990s Microsoft toolchain through the 32-bit `.MODEL flat` /
`PROC` / `.IF` authoring dialect.

Correctness is established by assembling **real Microsoft-generated assembly and
comparing the emitted bytes to what `ML.EXE` produces** (hundreds of code
fragments, byte-for-byte), plus the self-contained regression suite below.

### Limitations

- Fidelity is *functional* MASM compatibility — a byte-for-byte match on real
  code, not a bit-perfect match in the cases where MASM and NASM each make an
  equally valid but different encoding choice.
- Object *file-record* layout parity (beyond the code bytes, which do match) is
  not a goal.
- A few rarely-used constructs are not implemented; see
  [`docs/MASM-COMPAT-ROADMAP.md`](docs/MASM-COMPAT-ROADMAP.md).

### Tests

`test/masm/` holds a self-contained regression suite of small, hand-written MASM
fixtures whose expected output bytes ship with the repo:

```sh
python test/masm/run.py            # assemble every fixture, compare to golden bytes
python test/masm/run.py --objects  # also check the OMF and COFF object backends
```

See [`test/masm/README.md`](test/masm/README.md) for details.
