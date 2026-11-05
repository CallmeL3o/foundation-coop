import struct, sys

def rva2off(secs, rva):
    for va, vs, raw, rs in secs:
        if va <= rva < va + max(vs, rs):
            return rva - va + raw
    raise ValueError(hex(rva))

def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    secs = []
    for i in range(nsec):
        s = opt + optsz + 40 * i
        vs, va, rs, raw = struct.unpack_from('<IIII', d, s + 8)
        secs.append((va, vs, raw, rs))
    return d, opt, secs

def exports(path):
    d, opt, secs = load(path)
    erva, esz = struct.unpack_from('<II', d, opt + 112)
    e = rva2off(secs, erva)
    base, nfun, nnames, af, an, ao = struct.unpack_from('<IIIIII', d, e + 16)
    names = {}
    for i in range(nnames):
        nrva = struct.unpack_from('<I', d, rva2off(secs, an) + 4 * i)[0]
        o = struct.unpack_from('<H', d, rva2off(secs, ao) + 2 * i)[0]
        no = rva2off(secs, nrva)
        names[o] = d[no:d.index(b'\0', no)].decode()
    out = []
    for i in range(nfun):
        frva = struct.unpack_from('<I', d, rva2off(secs, af) + 4 * i)[0]
        if frva:
            out.append((base + i, names.get(i)))
    return out

def imports(path, dll):
    d, opt, secs = load(path)
    irva = struct.unpack_from('<I', d, opt + 120)[0]
    p = rva2off(secs, irva)
    res = []
    while True:
        oft, _, _, name, ft = struct.unpack_from('<IIIII', d, p)
        if not name: break
        nm = d[rva2off(secs, name):d.index(b'\0', rva2off(secs, name))].decode()
        if nm.lower() == dll.lower():
            t = rva2off(secs, oft or ft)
            while True:
                v = struct.unpack_from('<Q', d, t)[0]
                if not v: break
                if v >> 63: res.append('#%d' % (v & 0xffff))
                else:
                    o = rva2off(secs, v & 0x7fffffff) + 2
                    res.append(d[o:d.index(b'\0', o)].decode())
                t += 8
        p += 20
    return res

if __name__ == '__main__':
    if sys.argv[1] == 'exports':
        for o, n in exports(sys.argv[2]): print(o, n or '')
    else:
        print(imports(sys.argv[2], sys.argv[3]))
