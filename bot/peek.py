# print bytes at a virtual address of foundation.exe (image base 0x140000000)
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; n = struct.unpack_from('<H', d, pe+6)[0]; osz = struct.unpack_from('<H', d, pe+20)[0]
base = struct.unpack_from('<Q', d, pe+24+24)[0]
secs = [struct.unpack_from('<8sIIII', d, pe+24+osz+i*40) for i in range(n)]
for a in sys.argv[2:]:
    va = int(a, 16); rva = va - base
    for nm, vs, sva, rs, ro in secs:
        if sva <= rva < sva + vs:
            off = rva - sva + ro; print(hex(va), nm.rstrip(b'\0').decode(), d[off:off+32].hex(' ')); break
print("image base", hex(base))
