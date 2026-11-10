"""Minimal PE32 loader: maps sections into a flat image indexed by RVA."""
import struct
from dataclasses import dataclass, field


@dataclass
class Section:
    name: str
    va: int      # RVA
    vsize: int
    raw: int
    rsize: int
    chars: int

    @property
    def executable(self):
        return bool(self.chars & 0x20000000)


@dataclass
class Import:
    dll: str
    name: str
    iat: int     # absolute VA of IAT slot


@dataclass
class PE:
    path: str
    base: int
    entry: int                 # absolute VA
    image: bytearray           # indexed by RVA
    sections: list
    imports: list = field(default_factory=list)
    relocs: list = field(default_factory=list)   # absolute VAs of HIGHLOW fixup locations
    exports: dict = field(default_factory=dict)  # name (or '#ord') -> absolute VA

    def u32(self, va):
        return struct.unpack_from('<I', self.image, va - self.base)[0]

    def bytes_at(self, va, n):
        o = va - self.base
        return bytes(self.image[o:o + n])

    def section_of(self, va):
        r = va - self.base
        for s in self.sections:
            if s.va <= r < s.va + max(s.vsize, s.rsize):
                return s
        return None

    def is_code(self, va):
        s = self.section_of(va)
        return s is not None and s.name == '.text'

    @property
    def text(self):
        return next(s for s in self.sections if s.name == '.text')


def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    assert d[pe:pe + 4] == b'PE\0\0'
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    ep = struct.unpack_from('<I', d, opt + 16)[0]
    base = struct.unpack_from('<I', d, opt + 28)[0]
    size = struct.unpack_from('<I', d, opt + 56)[0]
    hdrsz = struct.unpack_from('<I', d, opt + 60)[0]
    dirs = [struct.unpack_from('<II', d, opt + 96 + 8 * i) for i in range(16)]
    img = bytearray(size)
    img[:hdrsz] = d[:hdrsz]
    secs = []
    o = opt + optsz
    for i in range(nsec):
        s = d[o + 40 * i:o + 40 * i + 40]
        name = s[:8].rstrip(b'\0').decode('latin1')
        vs, va, rs, rp = struct.unpack_from('<IIII', s, 8)
        ch = struct.unpack_from('<I', s, 36)[0]
        n = min(rs, len(d) - rp)
        img[va:va + n] = d[rp:rp + n]
        secs.append(Section(name, va, vs, rp, rs, ch))
    p = PE(path, base, base + ep, img, secs)

    def cstr(rva):
        e = img.index(0, rva)
        return img[rva:e].decode('latin1')

    # imports
    irva, _ = dirs[1]
    o = irva
    while True:
        oft, _, _, name, ft = struct.unpack_from('<IIIII', img, o)
        if name == 0:
            break
        dll = cstr(name)
        t = oft or ft
        i = 0
        while True:
            v = struct.unpack_from('<I', img, t + 4 * i)[0]
            if v == 0:
                break
            fn = ('#%d' % (v & 0xffff)) if v & 0x80000000 else cstr(v + 2)
            p.imports.append(Import(dll, fn, base + ft + 4 * i))
            i += 1
        o += 20

    # exports
    erva, esz = dirs[0]
    if erva:
        nfun, nnames, afun, anames, aords = struct.unpack_from('<IIIII', img, erva + 20)
        obase = struct.unpack_from('<I', img, erva + 16)[0]
        funcs = [struct.unpack_from('<I', img, afun + 4 * k)[0] for k in range(nfun)]
        named = set()
        for k in range(nnames):
            nrva = struct.unpack_from('<I', img, anames + 4 * k)[0]
            o = struct.unpack_from('<H', img, aords + 2 * k)[0]
            p.exports[cstr(nrva)] = base + funcs[o]
            named.add(o)
        for k, f in enumerate(funcs):
            if f and k not in named:
                p.exports['#%d' % (obase + k)] = base + f

    # base relocations
    rrva, rsz = dirs[5]
    o = rrva
    while o < rrva + rsz:
        page, bsz = struct.unpack_from('<II', img, o)
        if bsz == 0:
            break
        for j in range((bsz - 8) // 2):
            e = struct.unpack_from('<H', img, o + 8 + 2 * j)[0]
            if e >> 12 == 3:
                p.relocs.append(base + page + (e & 0xfff))
        o += bsz
    return p
