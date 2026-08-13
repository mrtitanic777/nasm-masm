#!/usr/bin/env python3
"""Build an external MASM module tree with `nasm --masm', under the tree's own
build configuration.

The Win95/Win3.1 disassembly modules are real Microsoft source and are NOT part
of this repo -- this driver only points nasm at a directory the user supplies.
It replicates the original build recipe so a module is assembled the way its
toolchain intended (otherwise build-config-gated macros, e.g. GENTER32 behind
PMODE32/PM386, look "undefined" and misparse into colliding labels).

The Win3.1 KERNEL recipe (from its KBUILD.BAT / BUILDALL.BAT):

    masm -DPM386 -t <module>.ASM

so the default here is `-D PM386=1', which every kernel module is built with
(WOW is left undefined -> kernel.inc's `ifndef WOW' default of 0).  Override
with --define / --no-default-defines for a different tree.

  python kbuild.py DIR                     # assemble DIR/*.asm, kernel defaults
  python kbuild.py DIR -I SHIMDIR          # extra include dir (e.g. the shim)
  python kbuild.py DIR -D FOO=1 -D BAR     # extra/other defines
  python kbuild.py DIR --errors MODULE     # print MODULE's errors and exit
  python kbuild.py DIR --top 20            # per-module error table (worst 20)

Nothing from DIR is read into or committed to this repo; results are printed.
Exit status is the number of modules that did not assemble cleanly (capped 255).
"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The Win3.1 KERNEL build config (KBUILD.BAT: `masm -DPM386 -t').
KERNEL_DEFAULT_DEFINES = ["PM386=1"]


def find_nasm(explicit):
    if explicit:
        return explicit
    for cand in ("../../nasm.exe", "../../nasm"):
        p = os.path.normpath(os.path.join(HERE, cand))
        if os.path.exists(p):
            return p
    return "nasm"


def is_standalone(path):
    """A module is standalone if it pulls in the segment/macro framework itself
    (include kernel.inc or cmacros). Fragments that are `include'd BY another
    module (e.g. aliases.asm) are not meant to assemble alone -- the original
    build fails them too -- so we flag rather than count them."""
    try:
        with open(path, "r", errors="replace") as f:
            head = f.read()
    except OSError:
        return True
    return bool(re.search(r"(?im)^\s*include\s+(kernel\.inc|cmacros\.inc)\b", head))


def assemble(nasm, asm, incdirs, defines, outfmt, out):
    cmd = [nasm, "--masm", "-f", outfmt]
    for d in defines:
        cmd += ["-D", d]
    for d in incdirs:
        cmd += ["-i", d + os.sep]
    cmd += [asm, "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    errs = [ln for ln in r.stderr.splitlines() if ": error:" in ln]
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", help="directory of MASM .asm modules to build")
    ap.add_argument("-I", "--include", action="append", default=[],
                    help="extra include directory (repeatable)")
    ap.add_argument("-D", "--define", action="append", default=[],
                    help="extra preprocessor define NAME[=val] (repeatable)")
    ap.add_argument("--no-default-defines", action="store_true",
                    help="drop the kernel -DPM386 default")
    ap.add_argument("--nasm", help="nasm to use (default ../../nasm[.exe])")
    ap.add_argument("--outfmt", default="obj", help="output format (default obj)")
    ap.add_argument("--errors", metavar="MODULE",
                    help="print one module's errors and exit")
    ap.add_argument("--top", type=int, default=0,
                    help="also print the N worst modules")
    args = ap.parse_args()

    nasm = find_nasm(args.nasm)
    defines = ([] if args.no_default_defines else list(KERNEL_DEFAULT_DEFINES))
    defines += args.define
    incdirs = [args.dir] + args.include
    tmp = os.path.join(HERE, ".kbuild.out")

    # Case-insensitive filesystems (Windows) return the same file for *.asm and
    # *.ASM, so dedupe by normalized path.
    seen = {}
    for g in (glob.glob(os.path.join(args.dir, "*.asm")) +
              glob.glob(os.path.join(args.dir, "*.ASM"))):
        seen.setdefault(os.path.normcase(os.path.abspath(g)), g)
    asms = sorted(seen.values())
    if not asms:
        print(f"no .asm files in {args.dir}", file=sys.stderr)
        return 2

    if args.errors:
        target = next((a for a in asms
                       if os.path.splitext(os.path.basename(a))[0].lower()
                       == args.errors.lower()), None)
        if not target:
            print(f"module {args.errors} not found", file=sys.stderr)
            return 2
        for e in assemble(nasm, target, incdirs, defines, args.outfmt, tmp):
            print(e)
        if os.path.exists(tmp):
            os.remove(tmp)
        return 0

    print(f"nasm --masm, defines={defines or '(none)'}, "
          f"include={incdirs}\n")
    results = []          # (name, nerrs, standalone)
    for asm in asms:
        name = os.path.splitext(os.path.basename(asm))[0]
        errs = assemble(nasm, asm, incdirs, defines, args.outfmt, tmp)
        results.append((name, len(errs), is_standalone(asm)))
    if os.path.exists(tmp):
        os.remove(tmp)

    standalone = [r for r in results if r[2]]
    fragments = [r for r in results if not r[2]]
    clean = [r for r in standalone if r[1] == 0]
    total_err = sum(r[1] for r in standalone)

    print(f"standalone modules : {len(standalone)}")
    print(f"  assembling clean : {len(clean)}")
    print(f"  total errors     : {total_err}")
    print(f"include fragments  : {len(fragments)} "
          f"(not built standalone; the original build fails them too)")

    if args.top:
        worst = sorted(standalone, key=lambda r: -r[1])[:args.top]
        print(f"\nworst {len(worst)} standalone modules:")
        for name, n, _ in worst:
            print(f"  {name:<14} {n}")

    nfail = len(standalone) - len(clean)
    return min(nfail, 255)


if __name__ == "__main__":
    sys.exit(main())
