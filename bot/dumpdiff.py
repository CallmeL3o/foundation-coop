# Compare the component dumps (dev_dump_components) of instance A and B for the objects whose GUIDs are given
# (default: every object dumped on both). Pointer-like qwords are skipped.
# usage: python dumpdiff.py [guid ...]
import glob, os, struct, sys
A, B = r"E:\FoundationCoopTest\instances\A", r"E:\FoundationCoopTest\instances\B"
def ptr(q): return 0x10000000000 <= q < 0x30000000000 or (q >> 40) == 0x7f
names = sorted(os.path.basename(f) for f in glob.glob(os.path.join(A, "dump_*.bin")))
if sys.argv[1:]: names = [n for n in names if any(g in n for g in sys.argv[1:])]
for n in names:
    fb = os.path.join(B, n)
    if not os.path.exists(fb): print(n, "MISSING on B"); continue
    a = open(os.path.join(A, n), "rb").read(); b = open(fb, "rb").read()
    out = []
    for off in range(0, min(len(a), len(b)) - 7, 4):
        if a[off:off + 4] == b[off:off + 4]: continue
        q = off & ~7
        if ptr(struct.unpack_from("<Q", a, q)[0]) or ptr(struct.unpack_from("<Q", b, q)[0]): continue
        out.append("+%03x A=%s B=%s" % (off, a[off:off + 4].hex(), b[off:off + 4].hex()))
    if out: print(n, len(out), " ".join(out[:40]))
for f in glob.glob(os.path.join(B, "dump_*.bin")):
    n = os.path.basename(f)
    if (not sys.argv[1:] or any(g in n for g in sys.argv[1:])) and not os.path.exists(os.path.join(A, n)): print(n, "MISSING on A")
