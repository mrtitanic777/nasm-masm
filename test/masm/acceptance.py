#!/usr/bin/env python3
"""
Optional acceptance check: confirm this fork's `nasm` is a drop-in replacement.

Point this at a directory from any project that already builds its assembly with
NASM (a `vmm/` and/or `vxd/` subdirectory of NASM-syntax `.asm` files), and it
re-assembles every module with THIS binary. Because that source is plain NASM
syntax it runs the stock (non-`--masm`) path, so a clean pass proves the `--masm`
work never regressed stock NASM behaviour -- the fork is byte-for-byte stock NASM
without the flag.

Usage:
    python acceptance.py PROJECT_DIR [--nasm PATH] [--masm]

PROJECT_DIR is the downstream project root (looked up for vmm/*.asm, vxd/*.asm).
If omitted, a sibling `../../../a downstream project` checkout is tried as a convenience.
Exit status is non-zero if any module fails to assemble.
"""
import os
import sys
import glob
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def find_nasm(explicit):
    if explicit:
        return explicit
    for c in ("nasm.exe", "nasm"):
        p = os.path.join(HERE, "..", "..", c)
        if os.path.exists(p):
            return p
    return "nasm"


def main():
    args = sys.argv[1:]
    nasm = None
    masm = False
    project = None
    i = 0
    while i < len(args):
        if args[i] == "--nasm":
            nasm = args[i + 1]
            i += 2
        elif args[i] == "--masm":
            masm = True
            i += 1
        else:
            project = args[i]
            i += 1

    nasm = find_nasm(nasm)
    if not project:
        # default: sibling GitHub checkout
        project = os.path.normpath(
            os.path.join(HERE, "..", "..", "..", "a downstream project"))

    src = []
    for sub in ("vmm", "vxd"):
        src += sorted(glob.glob(os.path.join(project, sub, "*.asm")))
    if not src:
        print(f"acceptance: no vmm/*.asm or vxd/*.asm under {project}")
        print("            pass the a downstream project directory as the first argument.")
        return 2

    print(f"nasm:    {nasm}")
    print(f"a downstream project: {project}")
    ok = fail = 0
    for f in src:
        cmd = [nasm]
        if masm:
            cmd.append("--masm")
        cmd += ["-f", "elf32", "-I", os.path.dirname(f) + os.sep,
                f, "-o", os.devnull]
        r = subprocess.run(cmd, capture_output=True, text=True)
        name = os.path.relpath(f, project)
        if r.returncode == 0:
            ok += 1
        else:
            fail += 1
            first = (r.stderr or r.stdout).strip().splitlines()
            print(f"FAIL {name}: {first[0] if first else '(no message)'}")

    print(f"\nacceptance: {ok}/{ok + fail} modules assembled"
          f"{' (--masm)' if masm else ''}")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
