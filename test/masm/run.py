#!/usr/bin/env python3
# Written by: Logan Greer
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


# --- object-backend code faithfulness (self-contained OMF + COFF readers) -----
#
# The MASM `NAME segment ... use32 ...' scaffolding must yield a 32-bit code
# segment in the object backends too, not only in `-f bin'. These minimal
# readers pull the code bytes back out of a `-f obj' (OMF) and `-f win32' (COFF)
# object so we can assert they equal the `-f bin' golden. (Full OBJ-record
# byte-parity against ML is a separate, oracle-gated goal; here we prove the
# code path through each backend is faithful.)

def _omf_index(b, p):
    return ((((b[p] & 0x7F) << 8) | b[p + 1]), p + 2) if b[p] & 0x80 else (b[p], p + 1)


def omf_code(data):
    """Code bytes of the CODE/_TEXT segment from an OMF object (LNAMES/SEGDEF/LEDATA)."""
    i = 0
    lnames = [""]
    segs = []
    segb = {}
    while i + 3 <= len(data):
        t = data[i]
        ln = data[i + 1] | (data[i + 2] << 8)
        body = data[i + 3:i + 3 + ln - 1]
        i += 3 + ln
        if t == 0x96:                                   # LNAMES
            p = 0
            while p < len(body):
                l = body[p]
                lnames.append(body[p + 1:p + 1 + l].decode("latin1"))
                p += 1 + l
        elif t in (0x98, 0x99):                         # SEGDEF / SEGDEF32
            wide = t == 0x99
            p = 0
            acbp = body[p]; p += 1
            if (acbp >> 5) == 0:                         # absolute segment: frame+offset
                p += 3
            p += 4 if wide else 2                        # segment length
            si, p = _omf_index(body, p)
            ci, p = _omf_index(body, p)
            name = lnames[si] if si < len(lnames) else "_TEXT"
            cls = lnames[ci] if ci < len(lnames) else ""
            segs.append((name, cls))
            segb.setdefault(name, bytearray())
        elif t in (0xA0, 0xA1):                         # LEDATA / LEDATA32
            wide = t == 0xA1
            p = 0
            si, p = _omf_index(body, p)
            off = int.from_bytes(body[p:p + (4 if wide else 2)], "little")
            p += 4 if wide else 2
            dat = body[p:]
            if 0 < si <= len(segs):
                bb = segb[segs[si - 1][0]]
                if len(bb) < off + len(dat):
                    bb.extend(b"\0" * (off + len(dat) - len(bb)))
                bb[off:off + len(dat)] = dat
    for name, cls in segs:
        if "CODE" in cls.upper() or name.upper().endswith("TEXT"):
            return bytes(segb[name])
    return None


def coff_code(data):
    """Code bytes of the _TEXT/.text section from a COFF object."""
    import struct
    nsec = struct.unpack_from("<H", data, 2)[0]
    opt = struct.unpack_from("<H", data, 16)[0]
    off = 20 + opt
    for k in range(nsec):
        sh = data[off + k * 40: off + k * 40 + 40]
        name = sh[0:8].rstrip(b"\0")
        _vs, _va, size, ptr = struct.unpack_from("<IIII", sh, 8)
        if name.upper().endswith(b"TEXT") and size and ptr:
            return data[ptr:ptr + size]
    return None


def check_objects(nasm):
    """For each fixture that defines a segment, assemble -f obj and -f win32 and
    assert the extracted code equals the fixture's -f bin golden."""
    readers = [("obj", "obj", omf_code), ("win32", "coff", coff_code)]
    npass = nfail = nskip = 0
    for asm in sorted(glob.glob(os.path.join(FIXTURES, "*.asm"))):
        name = os.path.splitext(os.path.basename(asm))[0]
        gold = os.path.join(GOLDEN, name + ".hex")
        if not os.path.exists(gold):
            continue
        with open(asm) as f:
            src = f.read()
        # Object-check fixtures that declare a `NAME segment', plus a few
        # simplified-segment (.code/.model) fixtures whose code is relocation-
        # free so the extracted object code equals the -f bin golden exactly.
        # hll_struct also guards the STRUCT-then-.code USE32 path: a regression
        # to USE16 would add 66 prefixes and fail the byte compare.
        OBJ_SAFE = {"hll_struct"}
        # Some segment-declaring fixtures test inherently 16-bit or multi-segment
        # behaviour that the 32-bit code-extraction check cannot mirror: a 16-bit
        # FAR pointer needs a 16-bit relocation (COFF/win32 has none), and the
        # SEGMENT/ENDS resume fixture splits its bytes across two physical
        # segments, so the extracted _TEXT never equals the concatenated -f bin
        # golden. These are -f bin tests only.
        OBJ_SKIP = {"masm_lds_farptr", "masm_segment_resume"}
        if name in OBJ_SKIP:
            nskip += 1
            continue
        if not re.search(r"(?im)^\s*\S+\s+segment\b", src) and name not in OBJ_SAFE:
            nskip += 1               # flat fixture: an -f bin test, not an object
            continue
        with open(gold) as f:
            want = bytes.fromhex(f.read().strip())
        for fmt, tag, reader in readers:
            tmp = os.path.join(HERE, f".{name}.{tag}")
            r = subprocess.run([nasm, "--masm", "-f", fmt, asm, "-o", tmp],
                               capture_output=True, text=True)
            if r.returncode != 0:
                first = (r.stderr.strip().splitlines() or [""])[0]
                print(f"FAIL {name} [-f {fmt}]: assemble error: {first}")
                nfail += 1
                continue
            with open(tmp, "rb") as fh:
                code = reader(fh.read())
            os.remove(tmp)
            if code == want:
                print(f"PASS {name} [-f {fmt}] ({len(code)} bytes)")
                npass += 1
            else:
                got = code.hex() if code else "<no code segment>"
                print(f"FAIL {name} [-f {fmt}]:\n  got  {got}\n  want {want.hex()}")
                nfail += 1
    print(f"\nobjects: {npass} passed, {nfail} failed, {nskip} skipped (flat, -f bin only)")
    return nfail


def main():
    ap = argparse.ArgumentParser(description="MASM-mode regression harness")
    ap.add_argument("--nasm", help="nasm binary (default: ../../nasm[.exe])")
    ap.add_argument("--update", action="store_true", help="regenerate golden files")
    ap.add_argument("--corpus", metavar="DIR", help="also validate an external corpus by path")
    ap.add_argument("--objects", action="store_true",
                    help="also check -f obj (OMF) and -f win32 (COFF) code faithfulness")
    args = ap.parse_args()

    nasm = find_nasm(args.nasm)
    if not (os.path.exists(nasm) or args.nasm is None):
        print(f"nasm not found: {nasm}", file=sys.stderr)
        return 2
    print(f"nasm: {nasm}\n")

    rc = check_fixtures(nasm, args.update)
    if args.objects and not args.update:
        print()
        rc += check_objects(nasm)
    if args.corpus and not args.update:
        print()
        rc += check_corpus(nasm, args.corpus)
    return 1 if rc else 0


if __name__ == "__main__":
    sys.exit(main())
