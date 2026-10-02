#!/usr/bin/env python3
"""Minimal PE32 import-table dumper to audit Win95/98 API usage."""
import struct, sys

def parse(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:2] == b'MZ'
    pe_off = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe_off:pe_off+4] == b'PE\0\0'
    coff = pe_off + 4
    nsec = struct.unpack_from('<H', data, coff+2)[0]
    optsize = struct.unpack_from('<H', data, coff+16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', data, opt)[0]
    assert magic == 0x10B, 'not PE32'
    # data directories
    import_rva, import_size = struct.unpack_from('<II', data, opt + 96 + 8)
    secs = []
    for i in range(nsec):
        s = opt + optsize + i*40
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', data, s+8)
        secs.append((va, vsize, rawptr, rawsize))
    def rva2off(rva):
        for va, vsize, rawptr, rawsize in secs:
            if va <= rva < va + max(vsize, rawsize):
                return rawptr + (rva - va)
        return None
    imports = {}
    off = rva2off(import_rva)
    if off is None:
        raise ValueError('import table RVA not found')
    while True:
        ilt, ts, fc, name_rva, iat = struct.unpack_from('<IIIII', data, off)
        if name_rva == 0:
            break
        noff = rva2off(name_rva)
        if noff is None:
            raise ValueError('import name RVA not found')
        name = data[noff:data.index(b'\0', noff)].decode()
        syms = []
        # ILT and IAT have same layout; use ILT if present else IAT
        tbl = rva2off(ilt if ilt else iat)
        if tbl is None:
            raise ValueError('import lookup table RVA not found')
        while True:
            entry = struct.unpack_from('<I', data, tbl)[0]
            if entry == 0:
                break
            if entry & 0x80000000:
                syms.append('#ord%d' % (entry & 0x7FFFFFFF))
            else:
                hoff = rva2off(entry)
                if hoff is None:
                    raise ValueError('import symbol RVA not found')
                sname = data[hoff+2:data.index(b'\0', hoff+2)].decode()
                syms.append(sname)
            tbl += 4
        imports[name] = syms
        off += 20
    return imports

if __name__ == '__main__':
    for p in sys.argv[1:]:
        imp = parse(p)
        print('==== %s ====' % p)
        for dll in sorted(imp):
            print('%s (%d):' % (dll, len(imp[dll])))
            print('  ' + ', '.join(sorted(imp[dll])))
