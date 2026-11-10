"""dis.py ADDR [N] — disassemble from the function containing ADDR, annotating imports and strings."""
import sys, json, bisect
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import pe as pelib
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
p = pelib.load('analysis/comm2.exe')
j = json.load(open('analysis/functions.json'))
entries = j['entries']
iat = {i.iat: f'{i.dll}!{i.name}' for i in p.imports}
a = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 80
if len(sys.argv) <= 3:
    k = bisect.bisect_right(entries, a) - 1
    start = entries[k]
else:
    start = a
cs = Cs(CS_ARCH_X86, CS_MODE_32)
def ann(op):
    out = []
    import re
    for h in re.findall(r'0x[0-9a-f]{6,8}', op):
        v = int(h, 16)
        if v in iat: out.append(iat[v])
        elif p.section_of(v) and not p.is_code(v):
            b = p.bytes_at(v, 64).split(b'\0')[0]
            if len(b) >= 3 and all(32 <= c < 127 for c in b): out.append(repr(b.decode()))
    return ('   ; ' + ' '.join(out)) if out else ''
print(f'; function {start:08x}')
for i in cs.disasm(p.bytes_at(start, n * 8), start):
    print(f'{i.address:08x}  {i.mnemonic:8s} {i.op_str}{ann(i.op_str)}')
    n -= 1
    if n <= 0: break
