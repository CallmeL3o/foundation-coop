# Compare two Foundation save files field by field (drift detector for lockstep tests).
# Save file: header + PNG thumbnail + zlib stream(s); the world is the largest zlib stream, in the engine's binary tree:
#   u32 nkeys, nkeys NUL-terminated key names, u32 datasize, data[datasize]; data[0] = offset of the root node.
#   node = {u32 key, u32 next sibling, u32 X, u32 Y, ...}
#     leaf:   X = type (small), payload from +0xc (0 int32, 4 float, 5 double, 7 string, 8 guid, 6 list -> Y = first item)
#     object: X = first child element (offset, or 0), Y = first attribute (offset)
# Both trees are walked side by side (objects matched by their "id" attribute, else by name + index), so memory
# stays small even for ~100 MB worlds.
# usage: python savediff.py A.foundation B.foundation [--show N] [--ignore regex]
import sys, zlib, struct, re, collections

def world_stream(path):
    d = open(path, "rb").read(); mv = memoryview(d)
    best, i, n = b"", 0, len(d)
    while i < n - 2:
        if d[i] == 0x78 and ((d[i] << 8) | d[i + 1]) % 31 == 0:
            try:
                zlib.decompressobj().decompress(mv[i:i + 256])    # cheap validity probe
                z = zlib.decompressobj(); raw = z.decompress(mv[i:])
                if z.eof:
                    if len(raw) > len(best): best = raw
                    i = n - len(z.unused_data); continue
            except zlib.error:
                pass
        i += 1
    return best

class Tree:
    def __init__(self, raw):
        n = struct.unpack_from("<I", raw, 0)[0]; p = 4; self.keys = []
        for _ in range(n):
            e = raw.index(b"\0", p); self.keys.append(raw[p:e].decode("latin1")); p = e + 1
        self.size = struct.unpack_from("<I", raw, p)[0]; self.base = p + 4; self.raw = raw
        self.id_key = self.keys.index("id") if "id" in self.keys else -1
    def w(self, o): return struct.unpack_from("<I", self.raw, self.base + o)[0]
    def ok(self, o): return 4 <= o and o + 16 <= self.size
    def name(self, o):
        k = self.w(o); return self.keys[k] if k < len(self.keys) else "?%d" % k
    def is_object(self, o):
        # the writer appends: an object's children always come after it in the buffer
        x, y = self.w(o + 8), self.w(o + 12)
        if x > 15: return x > o and self.ok(x) and (y == 0 or (y > o and self.ok(y)))
        if x != 0 or y < o + 16 or not self.ok(y) or self.w(y) >= len(self.keys): return False
        nx = self.w(y + 4)
        return nx == 0 or nx > y
    def value(self, o):
        t = self.w(o + 8); b = self.base + o + 12; r = self.raw
        if t == 7: return r[b:r.index(b"\0", b)].decode("latin1", "replace")
        if t == 4: return "%.6g" % struct.unpack_from("<f", r, b)[0]
        if t == 5: return "%.9g" % struct.unpack_from("<d", r, b)[0]
        if t == 0: return str(struct.unpack_from("<i", r, b)[0])
        if t == 8: return r[b:b + 16].hex()
        if t == 1: return str(struct.unpack_from("<q", r, b)[0])
        if t == 17:   # UTF-16 string
            e = b
            while e + 1 < len(r) and (r[e] or r[e + 1]): e += 2
            return r[b:e].decode("utf-16-le", "replace")
        return "t%d:" % t + r[b:b + 16].hex()
    def label(self, o):   # an object's "id" GUID attribute, if any
        a = self.w(o + 12); steps = 0
        while a and self.ok(a) and steps < 64:
            if self.w(a) == self.id_key and self.w(a + 8) == 8: return self.raw[self.base + a + 12:self.base + a + 28].hex()
            a = self.w(a + 4); steps += 1
        return None
    def keylabel(self, o):   # map entries ("Item" with a "Key" child holding a value): label by that key
        for first in (self.w(o + 12), self.w(o + 8) if self.w(o + 8) > 15 else 0):
            a, steps = first, 0
            while a and self.ok(a) and steps < 16:
                if self.name(a) == "Key":
                    if not self.is_object(a): return self.value(a)
                    v = self.w(a + 12)
                    if v and self.ok(v) and self.name(v) == "value" and not self.is_object(v): return self.value(v)
                a = self.w(a + 4); steps += 1
        return None
    def children(self, first):
        """sibling chain -> ordered dict  label -> (offset, kind)  kind: 'o' object, 'l' list, 'v' value"""
        out, seen, o, steps = {}, collections.Counter(), first, 0
        while o and self.ok(o) and steps < 5_000_000:
            nm = self.name(o)
            if self.is_object(o):
                lab = self.label(o)
                if lab is None and nm == "Item": lab = self.keylabel(o)
                seen[nm] += 1
                out["%s[%s]" % (nm, lab if lab else seen[nm] - 1)] = (o, "o")
            else:
                seen[nm] += 1
                t = self.w(o + 8)
                out[nm + ("#%d" % (seen[nm] - 1) if seen[nm] > 1 else "")] = (o, "l" if t == 6 else "v" if t in (0, 1, 4, 5, 7, 8, 17) else "x")
            o = self.w(o + 4); steps += 1
        return out

_seen = set()
def compare(A, B, oa, ob, path, kind, rep, depth=0):
    if depth > 300 or (kind != "v" and oa in _seen): return
    if kind != "v": _seen.add(oa)
    if kind == "v":
        va, vb = A.value(oa), B.value(ob)
        if va != vb: rep(path, va, vb)
        return
    if kind == "x":   # other typed node: 4 payload words; forward offsets are child chains, the rest plain values
        def split(T, o):
            plain, links = [T.w(o + 8)], []
            for i in range(4):
                v = T.w(o + 12 + 4 * i)
                if v > o and T.ok(v) and T.w(v) < len(T.keys): links.append(v)
                else: plain.append(v)
            return plain, links
        pa, la = split(A, oa); pb, lb = split(B, ob)
        # words that are key indices are compared by key name (the key tables can differ, e.g. extra UI keys)
        na = [A.keys[v] if v < len(A.keys) else v for v in pa]; nb = [B.keys[v] if v < len(B.keys) else v for v in pb]
        if (pa != pb and na != nb) or len(la) != len(lb): rep(path, str(na), str(nb)); return
        for i, (x1, x2) in enumerate(zip(la, lb)):
            ca, cb = A.children(x1), B.children(x2)
            for k, (o1, k1) in ca.items():
                if k not in cb: rep("%s/@%d/%s" % (path, i, k), "<present>", "<missing>"); continue
                compare(A, B, o1, cb[k][0], "%s/@%d/%s" % (path, i, k), k1, rep, depth + 1)
            for k in cb:
                if k not in ca: rep("%s/@%d/%s" % (path, i, k), "<missing>", "<present>")
        return
    lists = [(A.w(oa + 12), B.w(ob + 12))] if kind == "l" else [(A.w(oa + 12), B.w(ob + 12)), (A.w(oa + 8), B.w(ob + 8))]
    for fa, fb in lists:
        ca, cb = A.children(fa), B.children(fb)
        for k, (o1, k1) in ca.items():
            if k not in cb: rep(path + "/" + k, "<present>", "<missing>"); continue
            o2, k2 = cb[k]
            if k1 != k2: rep(path + "/" + k, "kind " + k1, "kind " + k2); continue
            compare(A, B, o1, o2, path + "/" + k, k1, rep, depth + 1)
        for k in cb:
            if k not in ca: rep(path + "/" + k, "<missing>", "<present>")

def main():
    sys.setrecursionlimit(20000)
    A, B = Tree(world_stream(sys.argv[1])), Tree(world_stream(sys.argv[2]))
    ign = re.compile(sys.argv[sys.argv.index("--ignore") + 1]) if "--ignore" in sys.argv else None
    # fields that are not simulation state (wall clock, UI, notifications, visual animation); counted separately
    nonsim = re.compile(r"^/unscaledTime$|^/assets\[|Notification|SettingMap|BookContextual|LastPageIdOpened|CurrentSort|IsSortAscending"
                        r"|Animation|LoopBreaker|DelayToEraseSituationMessage|UnscaledStartTime|NewlyUnlocked"
                        # house upgrade requirement cache: lazily filled (values look uninitialized on the other side)
                        r"|InstRequirementListByLevel\[[^\]]*\]/Item\[[^\]]*\]/Level")
    ignored = collections.Counter()
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 40
    groups, examples, total = collections.Counter(), [], [0]
    pending = []
    def rep(path, va, vb):
        if ign and ign.search(path): return
        if nonsim.search(path):
            ignored[re.sub(r"\[[^\]]*\]", "", path).rsplit("/", 2)[-2]] += 1; return
        pending.append((path, va, vb))
    print("world: A %d bytes / %d keys, B %d bytes / %d keys" % (A.size, len(A.keys), B.size, len(B.keys)))
    compare(A, B, A.w(0), B.w(0), "", "o", rep)
    # each player's camera (an object with a LookAt component) and the render settings are local view state
    cams = {m.group(1) for p, _, _ in pending if "/LookAt[" in p for m in [re.search(r"(.*)/component\[[^\]]*\]/LookAt\[", p)] if m}
    for path, va, vb in pending:
        if path.startswith("/grahicRoot") or any(path.startswith(c + "/") for c in cams):
            ignored["camera/render"] += 1; continue
        total[0] += 1
        groups[re.sub(r"\[[0-9a-f]{32}\]", "[id]", re.sub(r"\[\d+\]", "[n]", path))] += 1
        if len(examples) < show: examples.append((path, va, vb))
    print("ignored (UI / visual / wall clock): %d %s" % (sum(ignored.values()), dict(ignored.most_common(8))))
    print("differing simulation fields: %d" % total[0])
    for g, c in groups.most_common(30): print("  %6d  %s" % (c, g))
    for p, x, y in examples: print("  %s\n      A=%s\n      B=%s" % (p, x, y))
    print("RESULT: " + ("IDENTICAL" if total[0] == 0 else "DIFFERENT"))

main()
