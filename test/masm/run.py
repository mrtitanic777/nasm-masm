#!/usr/bin/env python3
"""MASM-mode regression harness for the nasm-masm fork.

Assembles each fixture under fixtures/ with `nasm --masm -f bin' and compares
the emitted bytes to golden/<name>.hex. The fixtures are clean, hand-written
MASM (no third-party or Win95-derived material); the golden bytes capture this
fork's verified MASM-parity output, so a regression in any --masm behaviour is
caught here.

  python run.py                 # check all fixtures; exit 1 on any mismatch
  python run.py --update        # regenerate golden/*.hex from current output
  python run.py --nasm PATH     # nasm to use (default: ../../nasm[.exe])
  python run.py --corpus DIR    # ALSO validate an external disassembly corpus:
                                #   obj1_*.asm files whose `; offset bytes'
                                #   comments (and `db' operands) are the ground
                                #   truth. Supplied by path; nothing from it is
                                #   read into or committed to this repo.

Exit status is non-zero if any fixture (or, with --corpus, any corpus fragment)
is not byte-exact.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")
GOLDEN = os.path.join(HERE, "golden")


def find_nasm(explicit):
    if explicit:
        return explicit
    for cand in ("../../nasm.exe", "../../nasm"):
        p = os.path.normpath(os.path.join(HERE, cand))
        if os.path.exists(p):
            return p
    return "nasm"


def assemble(nasm, asm, out):
    r = subprocess.run([nasm, "--masm", "-f", "bin", asm, "-o", out],
                       capture_output=True, text=True)
    return r.returncode, r.stderr


def check_fixtures(nasm, update):
    os.makedirs(GOLDEN, exist_ok=True)
    npass = nfail = 0
    for asm in sorted(glob.glob(os.path.join(FIXTURES, "*.asm"))):
        name = os.path.splitext(os.path.basename(asm))[0]
        tmp = os.path.join(HERE, "." + name + ".out")
        rc, err = assemble(nasm, asm, tmp)
        if rc != 0:
            first = (err.strip().splitlines() or [""])[0]
            print(f"FAIL {name}: assemble error: {first}")
            nfail += 1
            continue
        with open(tmp, "rb") as f:
            got = f.read()
        os.remove(tmp)
        gold = os.path.join(GOLDEN, name + ".hex")
        if update:
            with open(gold, "w") as f:
                f.write(got.hex() + "\n")
            print(f"updated {name} ({len(got)} bytes)")
            npass += 1
            continue
        if not os.path.exists(gold):
            print(f"FAIL {name}: no golden file (run --update)")
            nfail += 1
            continue
        with open(gold) as f:
            want = bytes.fromhex(f.read().strip())
        if got == want:
            print(f"PASS {name} ({len(got)} bytes)")
            npass += 1
        else:
            print(f"FAIL {name}:\n  got  {got.hex()}\n  want {want.hex()}")
            nfail += 1
    print(f"\nfixtures: {npass} passed, {nfail} failed")
    return nfail


# --- external corpus validation (ground truth from inline comments) ----------

def _masm_byte(tok):
    tok = tok.strip().lower().rstrip(",")
    if tok.endswith("h"):
        try:
            return int(tok[:-1], 16)
        except ValueError:
            return None
    if re.fullmatch(r"[0-9]+", tok):
        return int(tok, 10)
    return None


def corpus_expected(path):
    """Reconstruct ground-truth bytes: `db' operands are the bytes; instruction
    lines carry them in the trailing `; OFFSET b0 b1 ...' comment."""
    out = bytearray()
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            code, _, comment = raw.partition(";")
            code = code.strip()
            if code.lower().startswith("db "):
                for tok in code[3:].split(","):
                    b = _masm_byte(tok)
                    if b is not None:
                        out.append(b & 0xFF)
                continue
            toks = comment.split()
            if toks and re.fullmatch(r"[0-9a-fA-F]{4,6}", toks[0]):
                for t in toks[1:]:
                    if re.fullmatch(r"[0-9a-fA-F]{2}", t):
                        out.append(int(t, 16))
                    else:
                        break
    return bytes(out)


def check_corpus(nasm, cdir):
    frags = sorted(glob.glob(os.path.join(cdir, "**", "obj1_*.asm"), recursive=True))
    if not frags:
        print(f"corpus: no obj1_*.asm under {cdir}")
        return 1
    tot = match = exact = errors = 0
    bad = []
    for f in frags:
        tmp = os.path.join(HERE, ".corpus.out")
        rc, _ = assemble(nasm, f, tmp)
        if rc != 0:
            errors += 1
            bad.append(os.path.relpath(f, cdir))
            continue
        with open(tmp, "rb") as fh:
            act = fh.read()
        os.remove(tmp)
        exp = corpus_expected(f)
        n = min(len(exp), len(act))
        m = sum(1 for i in range(n) if exp[i] == act[i])
        tot += len(exp)
        match += m
        if m == n and len(exp) == len(act):
            exact += 1
        else:
            bad.append(os.path.relpath(f, cdir))
    nfrag = len(frags)
    pct = (100.0 * match / tot) if tot else 100.0
    print(f"corpus: {nfrag} fragments, {errors} assemble errors, "
          f"{exact}/{nfrag} byte-exact, {pct:.2f}% bytes")
    if bad:
        print("  not byte-exact:", ", ".join(bad[:12]) + (" ..." if len(bad) > 12 else ""))
    return 0 if (exact == nfrag and errors == 0) else 1


def main():
    ap = argparse.ArgumentParser(description="MASM-mode regression harness")
    ap.add_argument("--nasm", help="nasm binary (default: ../../nasm[.exe])")
    ap.add_argument("--update", action="store_true", help="regenerate golden files")
    ap.add_argument("--corpus", metavar="DIR", help="also validate an external corpus by path")
    args = ap.parse_args()

    nasm = find_nasm(args.nasm)
    if not (os.path.exists(nasm) or args.nasm is None):
        print(f"nasm not found: {nasm}", file=sys.stderr)
        return 2
    print(f"nasm: {nasm}\n")

    rc = check_fixtures(nasm, args.update)
    if args.corpus and not args.update:
        print()
        rc += check_corpus(nasm, args.corpus)
    return 1 if rc else 0


if __name__ == "__main__":
    sys.exit(main())
