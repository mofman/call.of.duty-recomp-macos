"""Disassemble guest code: python -I tools/dis.py <module-file> <hexaddr> [count]"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))
import pe as pelib
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
p = pelib.load(sys.argv[1]); a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
iat = {i.iat: f'{i.dll}!{i.name}' for i in p.imports}
md = Cs(CS_ARCH_X86, CS_MODE_32)
for i in list(md.disasm(p.bytes_at(a, n * 8), a))[:n]:
    s = i.op_str
    for k, v in iat.items():
        s = s.replace(hex(k), v)
    print(f'{i.address:08x}  {i.bytes.hex():<20} {i.mnemonic} {s}')
