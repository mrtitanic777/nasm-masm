#!/usr/bin/env python3
# Written by: Logan Greer
"""Differential oracle: compare `nasm --masm` against real Microsoft ML.EXE.

The corpus carries its own byte ground truth, so instruction parity (Track A)
did not need ML. The *authoring* dialect (PROC/INVOKE/.IF/macros/structs) has no
ground truth, so this drives a real ML.EXE and diffs its object code against
`nasm --masm`, per source file.

ML.EXE 6.11c/6.13/6.14 are Win32 executables and run natively on modern Windows
(no DOSBox). MASM uses `/` for options, so under MSYS/Git-Bash we set
MSYS_NO_PATHCONV=1 and invoke ML from the work directory with bare 8.3 names.

  python ml_oracle.py --ml <ML.EXE> file.asm [file.asm ...]
  python ml_oracle.py --ml <ML.EXE> --corpus DIR      # obj1_*.asm w/ ground truth

Note (documented finding): on the Win95 VxD corpus, `nasm --masm' matches the
shipped bytes 100%, while ML 6.0/6.11c diverge on a few SIB base/index orderings
of two-register addressing (both encode `[eax+esi]' with base=esi; the shipped
code, and nasm, use base=eax). Those are functionally identical encodings; the
corpus ground truth is authoritative, so this tool reports such diffs but they
are expected, not nasm bugs.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from run import omf_code, find_nasm, corpus_expected  # reuse the readers


def run_ml(mlexe, asm_path, work):
    """Assemble asm_path with native ML.EXE; return (code_bytes|None, message)."""
    src = os.path.join(work, "T.ASM")
    obj = os.path.join(work, "T.OBJ")
    shutil.copyfile(asm_path, src)
    if os.path.exists(obj):
        os.remove(obj)
    env = dict(os.environ, MSYS_NO_PATHCONV="1")  # keep ML's /c /Fo as options
    r = subprocess.run([mlexe, "/c", "/Fo", "T.OBJ", "T.ASM"],
                       cwd=work, capture_output=True, text=True, env=env)
    if not os.path.exists(obj):
        lines = (r.stdout + r.stderr).strip().splitlines()
        msg = next((l.strip() for l in lines if "error" in l.lower()),
                   lines[-1].strip() if lines else "ML produced no object")
        return None, msg
    return omf_code(open(obj, "rb").read()), ""


def nasm_bytes(nasm, asm_path, work):
    out = os.path.join(work, "N.bin")
    r = subprocess.run([nasm, "--masm", "-f", "bin", asm_path, "-o", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return open(out, "rb").read()


def main():
    ap = argparse.ArgumentParser(description="nasm --masm vs real ML.EXE oracle")
    ap.add_argument("--ml", required=True, help="path to a native ML.EXE (6.11c/6.13/6.14)")
    ap.add_argument("--nasm", help="nasm binary (default: ../../nasm[.exe])")
    ap.add_argument("--corpus", metavar="DIR", help="diff against a corpus's inline ground truth too")
    ap.add_argument("files", nargs="*", help="individual .asm files to diff")
    args = ap.parse_args()

    nasm = find_nasm(args.nasm)
    work = os.path.join(HERE, ".mlwork")
    os.makedirs(work, exist_ok=True)
    print(f"nasm: {nasm}\nml:   {args.ml}\n")

    rc = 0
    targets = list(args.files)
    if args.corpus:
        targets += sorted(glob.glob(os.path.join(args.corpus, "**", "obj1_*.asm"), recursive=True))
    if not targets:
        print("nothing to do: pass .asm files and/or --corpus DIR")
        return 2

    agree = ml_vs_gt = nasm_vs_gt = mlerr = n = 0
    for f in targets:
        n += 1
        ml, msg = run_ml(args.ml, f, work)
        nb = nasm_bytes(nasm, f, work)
        gt = corpus_expected(f) if args.corpus and re.search(r"obj\d+_", os.path.basename(f)) else None
        label = os.path.relpath(f, args.corpus) if args.corpus and f.startswith(args.corpus) else os.path.basename(f)
        if ml is None:
            mlerr += 1
            if len(targets) <= 20:
                print(f"  {label}: ML error: {msg}")
            continue
        if nb is not None and ml == nb:
            agree += 1
        if gt is not None:
            if ml == gt:
                ml_vs_gt += 1
            if nb == gt:
                nasm_vs_gt += 1
        if len(targets) <= 20 and nb is not None and ml != nb:
            d = [i for i in range(min(len(ml), len(nb))) if ml[i] != nb[i]]
            print(f"  {label}: nasm != ML at {len(d)} bytes"
                  + (f" e.g. @0x{d[0]:x} ML={ml[d[0]]:02x} nasm={nb[d[0]]:02x}" if d else ""))

    print(f"\n{n} files: nasm==ML {agree}/{n}, ML errors {mlerr}")
    if args.corpus:
        print(f"vs ground truth: nasm {nasm_vs_gt}/{n}, ML {ml_vs_gt}/{n}"
              " (ML < nasm is expected: SIB base/index ordering — see header)")
    return rc


if __name__ == "__main__":
    sys.exit(main())
