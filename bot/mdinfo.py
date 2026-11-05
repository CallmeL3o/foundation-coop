# Minimal minidump reader: exception code/address, module base of foundation.exe, and a stack scan for return addresses into it.
import struct, sys
d = open(sys.argv[1], 'rb').read()
sig, ver, nstreams, dirrva = struct.unpack_from('<IIII', d, 0)
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from('<III', d, dirrva + i * 12); streams[t] = (rva, size)
mods = []
if 4 in streams:  # ModuleListStream
    rva, _ = streams[4]; n = struct.unpack_from('<I', d, rva)[0]
    for i in range(n):
        base, size = struct.unpack_from('<QI', d, rva + 4 + i * 108)
        name_rva = struct.unpack_from('<I', d, rva + 4 + i * 108 + 20)[0]
        ln = struct.unpack_from('<I', d, name_rva)[0]; name = d[name_rva + 4:name_rva + 4 + ln].decode('utf-16le')
        mods.append((base, size, name.replace(chr(92), '/').split('/')[-1]))
def where(a):
    for b, s, n in mods:
        if b <= a < b + s: return f"{n}+0x{a-b:x}" + (f" (VA 0x{a-b+0x140000000:x})" if n.lower() == 'foundation.exe' else "")
    return hex(a)
if 6 in streams:  # ExceptionStream
    rva, _ = streams[6]; tid = struct.unpack_from('<I', d, rva)[0]
    code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', d, rva + 8)
    params = struct.unpack_from('<15Q', d, rva + 8 + 32)
    print(f"exception 0x{code:08x} at {where(addr)} thread {tid} params {[hex(p) for p in params[:nparams]]}")
    ctx_size, ctx_rva = struct.unpack_from('<II', d, rva + 8 + 152)
    rsp = struct.unpack_from('<Q', d, ctx_rva + 0x98)[0]; rip = struct.unpack_from('<Q', d, ctx_rva + 0xf8)[0]
    regs = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15']
    print("regs", " ".join(f"{r}={struct.unpack_from('<Q', d, ctx_rva + 0x78 + 8*i)[0]:x}" for i, r in enumerate(regs)))
    # stack scan from memory list
    mem = []
    if 5 in streams:
        r2, _ = streams[5]; n = struct.unpack_from('<I', d, r2)[0]
        for i in range(n):
            start, dsize, drva = struct.unpack_from('<QII', d, r2 + 4 + i * 16); mem.append((start, dsize, drva))
    for start, dsize, drva in mem:
        if start <= rsp < start + dsize:
            hits = 0
            for off in range(rsp - start, min(dsize, rsp - start + 0x2000), 8):
                v = struct.unpack_from('<Q', d, drva + off)[0]
                w = where(v)
                if 'foundation.exe' in w or 'coop.dll' in w:
                    print("  stack", hex(start + off), w); hits += 1
                    if hits > 25: break
print("modules:", [(hex(b), n) for b, s, n in mods if n.lower() in ('foundation.exe', 'coop.dll', 'folderhook.dll')])
