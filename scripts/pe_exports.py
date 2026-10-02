#!/usr/bin/env python3
"""Minimal PE32 export-table dumper.

Usage: pe_exports.py file.dll     
  prints one exported name per line
"""
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
    export_rva, export_size = struct.unpack_from('<II', data, opt + 96)
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
    def cstr(rva):
        o = rva2off(rva)
        if o is None:
            raise ValueError('name RVA not found')
        return data[o:data.index(b'\0', o)].decode()
    if export_rva == 0:
        return []
    off = rva2off(export_rva)
    if off is None:
        raise ValueError('export table RVA not found')
    (base, nfuncs, nnames, funcs_rva, names_rva,
     ords_rva) = struct.unpack_from('<IIIIII', data, off + 16)
    named = set()
    out = []
    noff = rva2off(names_rva)
    ooff = rva2off(ords_rva)
    if noff is None:
        raise ValueError('name table RVA not found')
    if ooff is None:
        raise ValueError('ordinal table RVA not found')
    for i in range(nnames):
        name = cstr(struct.unpack_from('<I', data, noff + 4*i)[0])
        named.add(struct.unpack_from('<H', data, ooff + 2*i)[0])
        out.append(name)
    foff = rva2off(funcs_rva)
    if foff is None:
        raise ValueError('function table RVA not found')
    for i in range(nfuncs):
        if i not in named and struct.unpack_from('<I', data, foff + 4*i)[0]:
            out.append('#ord%d' % (base + i))
    return out

if __name__ == '__main__':
    for p in sys.argv[1:]:
        for name in sorted(parse(p)):
            print(name)
