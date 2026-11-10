"""x86-32 → C lifter.

Every discovered entry becomes `void f_XXXXXXXX(Cpu *restrict cpu)`. Blocks reachable from the entry
are emitted as labels; jumps to other *call targets* become tail calls, everything else is a goto.
Guest stack/regs live in Cpu + guest memory, so calling conventions are transparent.
"""
import json
import os
import re
import sys
from collections import defaultdict

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86 as X

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import pe as pelib

REG32 = {'eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'}
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
REG8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
REG8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}

CC = {  # jcc/setcc suffix -> C condition
    'o': 'c_o(cpu)', 'no': '!c_o(cpu)', 'b': 'c_b(cpu)', 'ae': '!c_b(cpu)', 'e': 'c_z(cpu)', 'ne': '!c_z(cpu)',
    'be': 'c_be(cpu)', 'a': '!c_be(cpu)', 's': 'c_s(cpu)', 'ns': '!c_s(cpu)', 'p': 'c_p(cpu)', 'np': '!c_p(cpu)',
    'l': 'c_l(cpu)', 'ge': '!c_l(cpu)', 'le': 'c_le(cpu)', 'g': '!c_le(cpu)',
}
SZ = {1: '8', 2: '16', 4: '32'}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}


class Unsupported(Exception):
    pass


def hx(v):
    return '0x%xu' % (v & 0xffffffff)


class Lifter:
    def __init__(self, pe, analysis, import_syms):
        self.pe = pe
        self.blocks = {int(k): v for k, v in analysis['blocks'].items()}
        self.entries = set(analysis['entries'])
        self.jumptables = {int(k): v for k, v in analysis['jumptables'].items()}
        self.iat = {i.iat: sym for i, sym in import_syms}
        self.cs = Cs(CS_ARCH_X86, CS_MODE_32)
        self.cs.detail = True
        self.call_targets = set()
        self.overrides = set()
        self.setjmp_funcs = set()
        self.unsupported = defaultdict(int)
        self.text_lo = pe.base + pe.text.va
        self.text_hi = self.text_lo + pe.text.vsize
        self._scan_call_targets()

    def _scan_call_targets(self):
        for a, (e, _, _) in self.blocks.items():
            for i in self.cs.disasm(self.pe.bytes_at(a, e - a), a):
                if i.mnemonic == 'call' and i.operands[0].type == X.X86_OP_IMM:
                    self.call_targets.add(i.operands[0].imm & 0xffffffff)
        # data-referenced entries (vtables, callbacks) that look like real function starts are also call targets
        self.call_targets |= self.entries & set(self.blocks)

    # ------------------------------------------------------------ operands
    def addr(self, i, op):
        m = op.mem
        parts = []
        for r in (m.base, m.index):
            if r and i.reg_name(r) not in REG32:
                raise Unsupported('16-bit addressing')
        if m.base:
            parts.append('cpu->' + i.reg_name(m.base))
        if m.index:
            r = 'cpu->' + i.reg_name(m.index)
            parts.append(r if m.scale == 1 else f'{r}*{m.scale}')
        if m.disp or not parts:
            parts.append(hx(m.disp))
        e = ' + '.join(parts)
        if m.segment == X.X86_REG_FS:
            e = f'cpu->fs_base + {e}'
        elif m.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES, X.X86_REG_CS):
            raise Unsupported('segment ' + i.reg_name(m.segment))
        return f'(uint32_t)({e})'

    def rd(self, i, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            n = i.reg_name(op.reg)
            if n in REG32: return f'cpu->{n}'
            if n in REG16: return f'R16(cpu->{REG16[n]})'
            if n in REG8L: return f'R8L(cpu->{REG8L[n]})'
            if n in REG8H: return f'R8H(cpu->{REG8H[n]})'
            raise Unsupported('reg ' + n)
        if op.type == X.X86_OP_IMM:
            return hx(op.imm & int(MASK[size][:-1], 16))
        if op.type == X.X86_OP_MEM:
            return f'RD{SZ[size]}({self.addr(i, op)})'
        raise Unsupported('op type')

    def wr(self, i, op, val):
        if op.type == X.X86_OP_REG:
            n = i.reg_name(op.reg)
            if n in REG32: return f'cpu->{n} = (uint32_t)({val});'
            if n in REG16: return f'W16(cpu->{REG16[n]}, {val});'
            if n in REG8L: return f'W8L(cpu->{REG8L[n]}, {val});'
            if n in REG8H: return f'W8H(cpu->{REG8H[n]}, {val});'
            raise Unsupported('wreg ' + n)
        if op.type == X.X86_OP_MEM:
            return f'WR{SZ[op.size]}({self.addr(i, op)}, {val});'
        raise Unsupported('write op')

    # ------------------------------------------------------------ x87 operands
    def st_index(self, i, op):
        n = i.reg_name(op.reg)
        m = re.fullmatch(r'st\((\d)\)', n)
        if not m:
            raise Unsupported('fpreg ' + n)
        return int(m.group(1))

    def fmem(self, i, op, intop=False):
        a = self.addr(i, op)
        if intop:
            return {2: f'(double)(int16_t)RD16({a})', 4: f'(double)(int32_t)RD32({a})',
                    8: f'(double)(int64_t)RD64({a})'}[op.size]
        if op.size == 4: return f'RDF32({a})'
        if op.size == 8: return f'RDF64({a})'
        if op.size == 10: return f'f80_load(PTR({a}))'
        raise Unsupported('fmem size %d' % op.size)

    # ------------------------------------------------------------ instruction
    def insn(self, i, out, ctx):
        mn = i.mnemonic
        ops = i.operands
        nxt = i.address + i.size
        prefix = None
        if ' ' in mn:
            prefix, mn = mn.split(' ', 1)
            if prefix == 'lock':
                prefix = None
        E = out.append

        def bin_arith(cc, expr, store=True, carry=None):
            d, s = ops[0], ops[1]
            sz = d.size
            src = self.rd(i, s, sz) if s.type != X.X86_OP_IMM else hx(s.imm & int(MASK[sz][:-1], 16))
            pre = ''
            if carry:
                pre = 'uint32_t c = f_cf(cpu); '
            E(f'{{ {pre}uint32_t a = {self.rd(i, d)}, b = {src}, r = ({expr}) & {MASK[sz]};')
            if store:
                E('  ' + self.wr(i, d, 'r'))
            if carry:
                E(f'  SETF({cc}, {sz}, a, b, r); cpu->cc_c = c; }}')
            else:
                E(f'  SETF({cc}, {sz}, a, b, r); }}')

        if mn == 'mov':
            d, s = ops
            if d.type == X.X86_OP_REG and i.reg_name(d.reg) in ('es', 'ds', 'ss', 'fs', 'gs', 'cs') or \
                    s.type == X.X86_OP_REG and i.reg_name(s.reg) in ('es', 'ds', 'ss', 'fs', 'gs', 'cs'):
                raise Unsupported('mov seg')
            E(self.wr(i, d, self.rd(i, s, d.size)))
        elif mn in ('movzx', 'movsx'):
            d, s = ops
            v = self.rd(i, s)
            if mn == 'movsx':
                v = f'(uint32_t)(int32_t)(int{8 * s.size}_t){v}'
            E(self.wr(i, d, v))
        elif mn == 'lea':
            E(self.wr(i, ops[0], self.addr(i, ops[1])))
        elif mn == 'push':
            s = ops[0]
            if s.type == X.X86_OP_IMM:
                E(f'PUSH32({hx(s.imm)});')
            elif s.size == 4:
                E(f'PUSH32({self.rd(i, s)});')
            else:
                raise Unsupported('push16')
        elif mn == 'pop':
            d = ops[0]
            if d.type == X.X86_OP_MEM:
                # address computed after esp increment (x86 semantics)
                E(f'{{ uint32_t v = RD32(cpu->esp); cpu->esp += 4; {self.wr(i, d, "v")} }}')
            else:
                E(f'{{ uint32_t v = POP32(); {self.wr(i, d, "v")} }}')
        elif mn == 'add': bin_arith('CC_ADD', 'a + b')
        elif mn == 'sub': bin_arith('CC_SUB', 'a - b')
        elif mn == 'cmp': bin_arith('CC_SUB', 'a - b', store=False)
        elif mn == 'adc': bin_arith('CC_ADC', 'a + b + c', carry=True)
        elif mn == 'sbb': bin_arith('CC_SBB', 'a - b - c', carry=True)
        elif mn == 'and': bin_arith('CC_LOGIC', 'a & b')
        elif mn == 'or': bin_arith('CC_LOGIC', 'a | b')
        elif mn == 'xor':
            d, s = ops
            if d.type == s.type == X.X86_OP_REG and d.reg == s.reg:
                E(self.wr(i, d, '0') + f' SETF(CC_LOGIC, {d.size}, 0, 0, 0);')
            else:
                bin_arith('CC_LOGIC', 'a ^ b')
        elif mn == 'test': bin_arith('CC_LOGIC', 'a & b', store=False)
        elif mn in ('inc', 'dec'):
            d = ops[0]
            sz = d.size
            cc, opx = ('CC_INC', '+') if mn == 'inc' else ('CC_DEC', '-')
            if prefix is None and i.prefix[0] == 0xf0 and d.type == X.X86_OP_MEM and sz == 4:
                fn = '__atomic_add_fetch' if mn == 'inc' else '__atomic_sub_fetch'
                E(f'{{ uint32_t c = f_cf(cpu); uint32_t r = {fn}((uint32_t *)PTR({self.addr(i, d)}), 1, __ATOMIC_SEQ_CST);'
                  f' uint32_t a = r {"-" if mn == "inc" else "+"} 1; SETF({cc}, 4, a, 1, r); cpu->cc_c = c; }}')
            else:
                E(f'{{ uint32_t c = f_cf(cpu); uint32_t a = {self.rd(i, d)}, r = (a {opx} 1) & {MASK[sz]};')
                E(f'  {self.wr(i, d, "r")} SETF({cc}, {sz}, a, 1, r); cpu->cc_c = c; }}')
        elif mn == 'neg':
            d = ops[0]
            sz = d.size
            E(f'{{ uint32_t b = {self.rd(i, d)}, r = (0u - b) & {MASK[sz]}; {self.wr(i, d, "r")}'
              f' SETF(CC_NEG, {sz}, b, b, r); cpu->cc_a = b; cpu->cc_b = b; }}')
            # CC_NEG: CF = (operand != 0); OF formula uses a=0,b=operand -> handle via SUB form
            out[-1] = (f'{{ uint32_t b = {self.rd(i, d)}, r = (0u - b) & {MASK[sz]}; {self.wr(i, d, "r")}'
                       f' SETF(CC_SUB, {sz}, 0, b, r); }}')
        elif mn == 'not':
            d = ops[0]
            E(self.wr(i, d, f'~{self.rd(i, d)}'))
        elif mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            self.shift(i, mn, out)
        elif mn in ('shld', 'shrd'):
            d, s, c = ops
            cnt = self.rd(i, c, 1) if c.type != X.X86_OP_IMM else hx(c.imm)
            E(f'{{ uint32_t n = ({cnt}) & 31; if (n) {{ uint32_t a = {self.rd(i, d)}, b = {self.rd(i, s)}, r, cf;')
            if mn == 'shld':
                E('  r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1;')
            else:
                E('  r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1;')
            E(f'  {self.wr(i, d, "r")} cpu->cc_op = CC_CO; cpu->cc_sz = 4; cpu->cc_r = r;'
              f' cpu->cc_c = cf | ((((a ^ r) >> 31) & 1) << 1); }} }}')
        elif mn == 'xchg':
            a, b = ops
            E(f'{{ uint32_t t = {self.rd(i, a)}; {self.wr(i, a, self.rd(i, b))} {self.wr(i, b, "t")} }}')
        elif mn in ('cdq', 'cwde', 'cbw', 'cwd'):
            E({'cdq': 'cpu->edx = (int32_t)cpu->eax < 0 ? 0xffffffffu : 0;',
               'cwde': 'cpu->eax = (uint32_t)(int32_t)(int16_t)cpu->eax;',
               'cbw': 'W16(cpu->eax, (uint16_t)(int16_t)(int8_t)cpu->eax);',
               'cwd': 'W16(cpu->edx, ((int16_t)cpu->eax < 0) ? 0xffff : 0);'}[mn])
        elif mn in ('mul', 'imul', 'div', 'idiv'):
            self.muldiv(i, mn, out)
        elif mn.startswith('set') and mn[3:] in CC:
            E(self.wr(i, ops[0], f'({CC[mn[3:]]}) ? 1 : 0'))
        elif mn == 'leave':
            E('cpu->esp = cpu->ebp; cpu->ebp = POP32();')
        elif mn == 'enter':
            sz, lvl = ops[0].imm, ops[1].imm
            if lvl != 0:
                raise Unsupported('enter level')
            E(f'PUSH32(cpu->ebp); cpu->ebp = cpu->esp; cpu->esp -= {hx(sz)};')
        elif mn in ('nop', 'wait', 'fwait'):
            pass
        elif mn == 'cld': E('cpu->df = 0;')
        elif mn == 'std': E('cpu->df = 1;')
        elif mn == 'clc': E('set_eflags(cpu, get_eflags(cpu) & ~1u);')
        elif mn == 'stc': E('set_eflags(cpu, get_eflags(cpu) | 1u);')
        elif mn == 'cmc': E('set_eflags(cpu, get_eflags(cpu) ^ 1u);')
        elif mn == 'sahf': E('set_eflags(cpu, (get_eflags(cpu) & ~0xd5u) | (R8H(cpu->eax) & 0xd5u));')
        elif mn == 'lahf': E('W8H(cpu->eax, get_eflags(cpu));')
        elif mn == 'pushfd': E('PUSH32(get_eflags(cpu));')
        elif mn == 'popfd': E('set_eflags(cpu, POP32());')
        elif mn == 'pushal':
            E('{ uint32_t s = cpu->esp; PUSH32(cpu->eax); PUSH32(cpu->ecx); PUSH32(cpu->edx); PUSH32(cpu->ebx);'
              ' PUSH32(s); PUSH32(cpu->ebp); PUSH32(cpu->esi); PUSH32(cpu->edi); }')
        elif mn == 'popal':
            E('cpu->edi = POP32(); cpu->esi = POP32(); cpu->ebp = POP32(); cpu->esp += 4;'
              ' cpu->ebx = POP32(); cpu->edx = POP32(); cpu->ecx = POP32(); cpu->eax = POP32();')
        elif mn in ('bt', 'bts', 'btr', 'btc'):
            d, s = ops
            if d.type == X.X86_OP_MEM and s.type != X.X86_OP_IMM:
                # bit string: address = base + (sign-extended bit offset >> 3), dword-granular
                a = self.addr(i, d)
                E(f'{{ int32_t bo = (int32_t){self.rd(i, s, 4)}; uint32_t ea = {a} + (uint32_t)((bo >> 5) * 4);'
                  f' uint32_t v = RD32(ea), n = (uint32_t)bo & 31; set_eflags(cpu, (get_eflags(cpu) & ~1u) | ((v >> n) & 1));')
                if mn != 'bt':
                    v = {'bts': 'v | (1u << n)', 'btr': 'v & ~(1u << n)', 'btc': 'v ^ (1u << n)'}[mn]
                    E(f'  WR32(ea, {v});')
                E('}')
                return
            E(f'{{ uint32_t v = {self.rd(i, d)}, n = ({self.rd(i, s, 4) if s.type != X.X86_OP_IMM else hx(s.imm)}) & 31;'
              f' set_eflags(cpu, (get_eflags(cpu) & ~1u) | ((v >> n) & 1));')
            if mn != 'bt':
                v = {'bts': 'v | (1u << n)', 'btr': 'v & ~(1u << n)', 'btc': 'v ^ (1u << n)'}[mn]
                E('  ' + self.wr(i, d, v))
            E('}')
        elif mn == 'xlatb':
            E('W8L(cpu->eax, RD8(cpu->ebx + R8L(cpu->eax)));')
        elif mn == 'cpuid':
            E('rt_cpuid(cpu);')
        elif mn == 'rdtsc':
            E('{ uint64_t t = rt_rdtsc(); cpu->eax = (uint32_t)t; cpu->edx = (uint32_t)(t >> 32); }')
        elif mn in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                    'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'):
            op = {'movs': 'STR_MOVS', 'stos': 'STR_STOS', 'lods': 'STR_LODS', 'scas': 'STR_SCAS', 'cmps': 'STR_CMPS'}[mn[:4]]
            sz = {'b': 1, 'w': 2, 'd': 4}[mn[4]]
            rep = {None: 'REP_NONE', 'rep': 'REP_E', 'repe': 'REP_E', 'repne': 'REP_NE'}[prefix]
            if op in ('STR_MOVS', 'STR_STOS', 'STR_LODS') and rep == 'REP_NE':
                rep = 'REP_E'
            E(f'rt_string(cpu, {op}, {sz}, {rep});')
        # ------------------------------------------------------------ x87
        elif mn[0] == 'f':
            self.x87(i, mn, out)
        else:
            raise Unsupported(mn)

    def shift(self, i, mn, out):
        E = out.append
        d = i.operands[0]
        sz = d.size
        bits = 8 * sz
        if len(i.operands) == 1:
            cnt = '1u'
        else:
            c = i.operands[1]
            cnt = hx(c.imm) if c.type == X.X86_OP_IMM else self.rd(i, c, 1)
        E(f'{{ uint32_t n = ({cnt}) & 31; if (n) {{ uint32_t a = {self.rd(i, d)}, r, cf, of;')
        if mn in ('shl', 'sal'):
            E(f'  r = (n < 32 ? (a << n) : 0) & {MASK[sz]}; cf = n <= {bits} ? (a >> ({bits} - n)) & 1 : 0;'
              f' of = ((r >> {bits - 1}) & 1) ^ cf;')
        elif mn == 'shr':
            E(f'  r = (a >> n) & {MASK[sz]}; cf = (a >> (n - 1)) & 1; of = (a >> {bits - 1}) & 1;')
        elif mn == 'sar':
            E(f'  int32_t s = (int32_t)(int{bits}_t)a; r = (uint32_t)(s >> (n > {bits - 1} ? {bits - 1} : n)) & {MASK[sz]};'
              f' cf = (uint32_t)(s >> (n > {bits} ? {bits - 1} : n - 1)) & 1; of = 0;')
        elif mn in ('rol', 'ror'):
            E(f'  uint32_t k = n % {bits}; uint32_t fl = get_eflags(cpu);')
            if mn == 'rol':
                E(f'  r = k ? ((a << k) | (a >> ({bits} - k))) & {MASK[sz]} : a; cf = r & 1;'
                  f' of = ((r >> {bits - 1}) & 1) ^ cf;')
            else:
                E(f'  r = k ? ((a >> k) | (a << ({bits} - k))) & {MASK[sz]} : a; cf = (r >> {bits - 1}) & 1;'
                  f' of = ((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1;')
            E(f'  {self.wr(i, d, "r")} set_eflags(cpu, (fl & ~0x801u) | cf | (of << 11)); }} }}')
            return
        elif mn in ('rcl', 'rcr'):
            E(f'  uint32_t fl = get_eflags(cpu); uint32_t c = fl & 1; r = a & {MASK[sz]};')
            E(f'  for (uint32_t k = 0; k < n % {bits + 1}; k++) {{')
            if mn == 'rcl':
                E(f'    uint32_t hi = (r >> {bits - 1}) & 1; r = ((r << 1) | c) & {MASK[sz]}; c = hi; }}')
                E(f'  cf = c; of = ((r >> {bits - 1}) & 1) ^ cf;')
            else:
                E(f'    uint32_t lo = r & 1; r = (r >> 1) | (c << {bits - 1}); c = lo; }}')
                E(f'  cf = c; of = ((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1;')
            E(f'  {self.wr(i, d, "r")} set_eflags(cpu, (fl & ~0x801u) | cf | (of << 11)); }} }}')
            return
        E(f'  {self.wr(i, d, "r")} cpu->cc_op = CC_CO; cpu->cc_sz = {sz}; cpu->cc_r = r; cpu->cc_c = cf | (of << 1); }} }}')

    def muldiv(self, i, mn, out):
        E = out.append
        ops = i.operands
        if mn == 'imul' and len(ops) >= 2:
            d = ops[0]
            if len(ops) == 2:
                a, b = self.rd(i, ops[0]), self.rd(i, ops[1])
            else:
                a, b = self.rd(i, ops[1]), hx(ops[2].imm)
            bits = 8 * d.size
            E(f'{{ int64_t p = (int64_t)(int{bits}_t)({a}) * (int64_t)(int{bits}_t)({b}); uint32_t r = (uint32_t)p & {MASK[d.size]};')
            E(f'  uint32_t o = p != (int64_t)(int{bits}_t)r; {self.wr(i, d, "r")}'
              f' cpu->cc_op = CC_CO; cpu->cc_sz = {d.size}; cpu->cc_r = r; cpu->cc_c = o | (o << 1); }}')
            return
        s = ops[0]
        sz = s.size
        v = self.rd(i, s)
        if mn in ('mul', 'imul'):
            sg = mn == 'imul'
            if sz == 4:
                t = 'int64_t)(int32_t' if sg else 'uint64_t)(uint32_t'
                E(f'{{ uint64_t p = (uint64_t)(({t})cpu->eax * ({t})({v}));'
                  ' cpu->eax = (uint32_t)p; cpu->edx = (uint32_t)(p >> 32);')
                o = 'p != (uint64_t)(int64_t)(int32_t)cpu->eax' if sg else 'cpu->edx != 0'
            elif sz == 2:
                t = 'int32_t)(int16_t' if sg else 'uint32_t)(uint16_t'
                E(f'{{ uint32_t p = (uint32_t)(({t})cpu->eax * ({t})({v})); W16(cpu->eax, p); W16(cpu->edx, p >> 16);')
                o = 'p != (uint32_t)(int32_t)(int16_t)p' if sg else '(p >> 16) != 0'
            else:
                t = 'int32_t)(int8_t' if sg else 'uint32_t)(uint8_t'
                E(f'{{ uint32_t p = (uint32_t)(({t})cpu->eax * ({t})({v})); W16(cpu->eax, p);')
                o = '(uint16_t)p != (uint16_t)(int16_t)(int8_t)p' if sg else '(p >> 8 & 0xff) != 0'
            E(f'  uint32_t o = {o}; cpu->cc_op = CC_CO; cpu->cc_sz = {sz}; cpu->cc_r = (uint32_t)p; cpu->cc_c = o | (o << 1); }}')
            return
        # div / idiv
        ia = hex(i.address)
        if sz == 4:
            if mn == 'div':
                E(f'{{ uint32_t d = {v}; if (!d) trap(cpu, {ia}, "div0"); uint64_t n = ((uint64_t)cpu->edx << 32) | cpu->eax;'
                  f' uint64_t q = n / d; if (q >> 32) trap(cpu, {ia}, "divovf"); cpu->eax = (uint32_t)q; cpu->edx = (uint32_t)(n % d); }}')
            else:
                E(f'{{ int32_t d = (int32_t){v}; if (!d) trap(cpu, {ia}, "div0"); int64_t n = (int64_t)(((uint64_t)cpu->edx << 32) | cpu->eax);'
                  f' int64_t q = n / d; if (q != (int32_t)q) trap(cpu, {ia}, "divovf"); cpu->eax = (uint32_t)q; cpu->edx = (uint32_t)(n % d); }}')
        elif sz == 2:
            t, w = ('uint32_t', 'uint16_t') if mn == 'div' else ('int32_t', 'int16_t')
            E(f'{{ {t} d = ({w}){v}; if (!d) trap(cpu, {ia}, "div0"); {t} n = ({t})((R16(cpu->edx) << 16) | R16(cpu->eax));'
              f' W16(cpu->eax, n / d); W16(cpu->edx, n % d); }}')
        else:
            t, w, w16 = ('uint32_t', 'uint8_t', 'uint16_t') if mn == 'div' else ('int32_t', 'int8_t', 'int16_t')
            E(f'{{ {t} d = ({w}){v}; if (!d) trap(cpu, {ia}, "div0"); {t} n = ({w16})R16(cpu->eax);'
              f' W8L(cpu->eax, n / d); W8H(cpu->eax, n % d); }}')

    def x87(self, i, mn, out):
        E = out.append
        ops = i.operands
        regs = [o for o in ops if o.type == X.X86_OP_REG]
        mems = [o for o in ops if o.type == X.X86_OP_MEM]

        consts = {'fld1': '1.0', 'fldz': '0.0', 'fldpi': 'M_PI', 'fldln2': 'M_LN2', 'fldl2e': 'M_LOG2E',
                  'fldlg2': '0.30102999566398119521', 'fldl2t': '3.32192809488736234787'}
        if mn in consts:
            E(f'FPUSH({consts[mn]});')
            return
        if mn in ('fld', 'fild'):
            if mems:
                E(f'FPUSH({self.fmem(i, mems[0], mn == "fild")});')
            else:
                E(f'{{ double v = ST({self.st_index(i, regs[0])}); FPUSH(v); }}')
            return
        if mn in ('fst', 'fstp'):
            if mems:
                a = self.addr(i, mems[0])
                s = mems[0].size
                E({4: f'WRF32({a}, ST(0));', 8: f'WRF64({a}, ST(0));', 10: f'f80_store(PTR({a}), ST(0));'}[s])
            else:
                E(f'ST({self.st_index(i, regs[0])}) = ST(0);')
            if mn == 'fstp':
                E('FPOP();')
            return
        if mn in ('fist', 'fistp'):
            m = mems[0]
            a = self.addr(i, m)
            E({2: f'WR16({a}, fist16(cpu, ST(0)));', 4: f'WR32({a}, fist32(cpu, ST(0)));',
               8: f'WR64({a}, fist64(cpu, ST(0)));'}[m.size])
            if mn == 'fistp':
                E('FPOP();')
            return
        if mn == 'fbstp':
            raise Unsupported('fbstp')
        if mn == 'fxch':
            k = self.st_index(i, regs[-1]) if regs else 1
            if k == 0 and len(regs) == 2:
                k = self.st_index(i, regs[0])
            E(f'{{ double t = ST(0); ST(0) = ST({k}); ST({k}) = t; }}')
            return
        arith = {'fadd': '{a} + {b}', 'fsub': '{a} - {b}', 'fsubr': '{b} - {a}', 'fmul': '{a} * {b}',
                 'fdiv': '{a} / {b}', 'fdivr': '{b} / {a}'}
        base = mn[:-1] if mn.endswith('p') and mn[:-1] in arith else mn
        if base.startswith('fi') and 'f' + base[2:] in arith:
            src = self.fmem(i, mems[0], True)
            E(f'ST(0) = {arith["f" + base[2:]].format(a="ST(0)", b=src)};')
            return
        if base in arith:
            pop = mn.endswith('p')
            if mems:
                E(f'ST(0) = {arith[base].format(a="ST(0)", b=self.fmem(i, mems[0]))};')
            elif pop:
                k = self.st_index(i, regs[0]) if regs else 1
                if len(regs) == 2:
                    k = self.st_index(i, regs[0])
                E(f'ST({k}) = {arith[base].format(a=f"ST({k})", b="ST(0)")}; FPOP();')
            elif len(regs) == 1:   # `fadd st(i)` == st(0) = st(0) op st(i)
                s = self.st_index(i, regs[0])
                E(f'ST(0) = {arith[base].format(a="ST(0)", b=f"ST({s})")};')
            else:
                d, s = self.st_index(i, regs[0]), self.st_index(i, regs[1])
                E(f'ST({d}) = {arith[base].format(a=f"ST({d})", b=f"ST({s})")};')
            return
        if mn in ('fcom', 'fcomp', 'fucom', 'fucomp', 'fcompp', 'fucompp', 'ficom', 'ficomp', 'ftst'):
            if mn == 'ftst':
                E('fcom_set(cpu, ST(0), 0.0);')
                return
            if mems:
                src = self.fmem(i, mems[0], mn.startswith('fi'))
            elif regs:
                src = f'ST({self.st_index(i, regs[-1])})'
            else:
                src = 'ST(1)'
            E(f'fcom_set(cpu, ST(0), {src});')
            if mn.endswith('pp'):
                E('FPOP(); FPOP();')
            elif mn.endswith('p'):
                E('FPOP();')
            return
        simple = {'fchs': 'ST(0) = -ST(0);', 'fabs': 'ST(0) = fabs(ST(0));', 'fsqrt': 'ST(0) = sqrt(ST(0));',
                  'fsin': 'ST(0) = sin(ST(0)); cpu->fsw &= ~0x400u;', 'fcos': 'ST(0) = cos(ST(0)); cpu->fsw &= ~0x400u;',
                  'fptan': 'ST(0) = tan(ST(0)); FPUSH(1.0); cpu->fsw &= ~0x400u;',
                  'fsincos': '{ double v = ST(0); ST(0) = sin(v); FPUSH(cos(v)); cpu->fsw &= ~0x400u; }',
                  'fpatan': 'ST(1) = atan2(ST(1), ST(0)); FPOP();',
                  'fyl2x': 'ST(1) = ST(1) * log2(ST(0)); FPOP();',
                  'f2xm1': 'ST(0) = exp2(ST(0)) - 1.0;',
                  'fscale': 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                  'frndint': 'ST(0) = fround(cpu, ST(0));',
                  'fprem': 'ST(0) = fmod(ST(0), ST(1)); cpu->fsw &= ~0x400u;',
                  'fprem1': 'ST(0) = remainder(ST(0), ST(1)); cpu->fsw &= ~0x400u;',
                  'fxam': 'fxam(cpu);',
                  'fnclex': 'cpu->fsw &= 0x7f00u;', 'fninit': 'cpu->fcw = 0x27f; cpu->fsw = 0; cpu->ftop = 0;',
                  'fincstp': 'FPOP();', 'fdecstp': 'cpu->ftop = (cpu->ftop - 1) & 7;', 'ffree': ''}
        if mn in simple:
            E(simple[mn])
            return
        if mn == 'fnstsw':
            if regs:
                E('W16(cpu->eax, fnstsw(cpu));')
            else:
                E(f'WR16({self.addr(i, mems[0])}, fnstsw(cpu));')
            return
        if mn == 'fnstcw':
            E(f'WR16({self.addr(i, mems[0])}, cpu->fcw);')
            return
        if mn == 'fldcw':
            E(f'cpu->fcw = RD16({self.addr(i, mems[0])});')
            return
        raise Unsupported(mn)

    # ------------------------------------------------------------ control flow
    def call_or_tail(self, target, tail):
        """C statement invoking guest function at target (after return address handling)."""
        if target in self.blocks and target in self.entries:
            s = f'f_{target:08x}(cpu);'
        else:
            s = f'dispatch(cpu, {hx(target)});'
        return s + (' return;' if tail else '')

    def jump(self, fn_entry, target, local):
        """Statement transferring control to target (jmp semantics)."""
        if target in local:
            return f'goto L_{target:08x};'
        if not (self.text_lo <= target < self.text_hi):
            return f'trap(cpu, {hx(target)}, "jump outside text");'
        # tail call to another function
        return self.call_or_tail(target, True)

    def function_blocks(self, entry):
        seen = set()
        order = []
        work = [entry]
        while work:
            b = work.pop()
            if b in seen or b not in self.blocks:
                continue
            if b != entry and b in self.call_targets:
                continue
            seen.add(b)
            order.append(b)
            for s in self.blocks[b][1]:
                work.append(s)
        return sorted(seen)

    def lift_function(self, entry):
        local = set(self.function_blocks(entry))
        name = f'orig_f_{entry:08x}' if entry in self.overrides else f'f_{entry:08x}'
        out = [f'void {name}(Cpu *restrict cpu) {{', '  uint8_t *const m = g_mem; (void)m;',
               f'  goto L_{entry:08x};']
        for b in sorted(local):
            end, succs, kind = self.blocks[b]
            out.append(f'L_{b:08x}:;')
            body = []
            insns = list(self.cs.disasm(self.pe.bytes_at(b, end - b), b))
            last = insns[-1] if insns else None
            terminated = False
            for i in insns:
                is_last = i is last
                mn = i.mnemonic
                try:
                    if X.X86_GRP_RET in i.groups:
                        n = i.operands[0].imm if i.operands else 0
                        body.append(f'cpu->esp += {4 + n}; return;')
                        terminated = True
                    elif X.X86_GRP_CALL in i.groups:
                        body.append(f'PUSH32({hx(i.address + i.size)});')
                        op = i.operands[0]
                        if op.type == X.X86_OP_IMM and (op.imm & 0xffffffff) in self.setjmp_funcs:
                            # host setjmp lives in this (caller) frame so a guest longjmp can resume here
                            body.append(f'if (_setjmp(*rt_jmpbuf_for(RD32(cpu->esp + 4))) == 0) {{ '
                                        f'{self.call_or_tail(op.imm & 0xffffffff, False)} }}')
                        elif op.type == X.X86_OP_IMM:
                            body.append(self.call_or_tail(op.imm & 0xffffffff, False))
                        elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 \
                                and (op.mem.disp & 0xffffffff) in self.iat:
                            body.append(f'{self.iat[op.mem.disp & 0xffffffff]}(cpu);')
                        else:
                            # target evaluated before the push in x86; recompute with esp+4 adjustment if esp-based
                            tgt = self.rd(i, op, 4)
                            if 'esp' in tgt:
                                body[-1] = f'{{ uint32_t t = {tgt}; PUSH32({hx(i.address + i.size)}); dispatch(cpu, t); }}'
                            else:
                                body.append(f'dispatch(cpu, {tgt});')
                        if kind == 'noreturn' and is_last:
                            body.append(f'trap(cpu, {hx(i.address)}, "noreturn call returned");')
                            terminated = True
                    elif X.X86_GRP_JUMP in i.groups or mn in ('loop', 'loope', 'loopne', 'jecxz'):
                        op = i.operands[0]
                        if mn == 'jmp':
                            if op.type == X.X86_OP_IMM:
                                body.append(self.jump(entry, op.imm & 0xffffffff, local))
                            elif i.address in self.jumptables:
                                tgt = self.rd(i, op, 4)
                                body.append(f'switch ({tgt}) {{')
                                for t in sorted(set(self.jumptables[i.address])):
                                    body.append(f'  case {hx(t)}: {self.jump(entry, t, local)}')
                                body.append(f'  default: dispatch(cpu, {tgt}); return; }}')
                            elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 \
                                    and (op.mem.disp & 0xffffffff) in self.iat:
                                body.append(f'{self.iat[op.mem.disp & 0xffffffff]}(cpu); return;')
                            else:
                                body.append(f'dispatch(cpu, {self.rd(i, op, 4)}); return;')
                            terminated = True
                        else:
                            t = op.imm & 0xffffffff
                            if mn in ('jecxz', 'jcxz'):
                                cond = 'cpu->ecx == 0'
                            elif mn == 'loop':
                                cond = '--cpu->ecx != 0'
                            elif mn in ('loope', 'loopz'):
                                cond = '--cpu->ecx != 0 && c_z(cpu)'
                            elif mn in ('loopne', 'loopnz'):
                                cond = '--cpu->ecx != 0 && !c_z(cpu)'
                            elif mn[1:] in CC:
                                cond = CC[mn[1:]]
                            else:
                                raise Unsupported(mn)
                            body.append(f'if ({cond}) {{ {self.jump(entry, t, local)} }}')
                    elif mn in ('int3', 'hlt', 'int', 'int1', 'into', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd',
                                'cli', 'sti', 'bound', 'arpl', 'lds', 'les', 'ljmp', 'lcall', 'sldt', 'aaa', 'aas',
                                'aam', 'aad', 'daa', 'das', 'salc', 'movups', 'fnsave', 'frstor', 'fnstenv', 'fldenv',
                                'fbstp', 'iretd', 'retf', 'ud2'):
                        body.append(f'trap(cpu, {hx(i.address)}, "{mn}");')
                        terminated = True
                        self.unsupported[mn] += 1
                        break
                    else:
                        self.insn(i, body, None)
                except Unsupported as ex:
                    self.unsupported[str(ex)] += 1
                    body.append(f'trap(cpu, {hx(i.address)}, "unsupported: {i.mnemonic} {i.op_str}");')
                    terminated = True
                    break
            if not terminated:
                if kind in ('bad', 'trap'):
                    body.append(f'trap(cpu, {hx(end)}, "{kind}");')
                elif kind in ('fall', 'jcc', 'switch') and kind != 'switch':
                    body.append(self.jump(entry, end, local))
                elif kind == 'noreturn':
                    body.append(f'trap(cpu, {hx(end)}, "noreturn");')
            out.extend('  ' + l for l in body)
        out.append('}')
        return out


def import_symbols(pe):
    res = []
    for imp in pe.imports:
        dll = imp.dll.split('.')[0].upper()
        name = imp.name.replace('#', 'ord').replace('@', '_')
        res.append((imp, f'imp_{dll}_{name}'))
    return res


def write_if_changed(p, data):
    if not os.path.exists(p) or open(p).read() != data:
        open(p, 'w').write(data)


def main(want, per_file=600):
    from modules import MODULES, path, ANALYSIS_DIR, GEN_DIR, OVERRIDES
    root = path(None, GEN_DIR)
    os.makedirs(root, exist_ok=True)
    overrides = set()
    ovf = path(None, OVERRIDES)
    if os.path.exists(ovf):
        for line in open(ovf):
            line = line.split('#')[0].strip()
            if line:
                overrides.add(int(line, 16))
    all_entries, all_syms, all_exports = [], {}, []
    import_rows = []
    lifters = []
    for mi, m in enumerate(MODULES):
        pe = pelib.load(path(m))
        an = json.load(open(path(m, ANALYSIS_DIR, f'functions_{m["name"]}.json')))
        syms = import_symbols(pe)
        L = Lifter(pe, an, syms)
        L.overrides = overrides & set(L.entries)
        L.setjmp_funcs = set(m.get('setjmp', []))
        entries = sorted(e for e in L.entries if e in L.blocks)
        lifters.append((m, L, entries))
        all_entries += entries
        for imp, sym in syms:
            all_syms[sym] = imp
            import_rows.append((mi, imp, sym))
        for name, va in sorted(pe.exports.items()):
            all_exports.append((mi, name, va))
    missing = overrides - set(all_entries)
    if missing:
        sys.exit(f'override addresses are not function entries: {[hex(x) for x in missing]}')
    hdr = ['#pragma once', '#include "cpu.h"', '#include <setjmp.h>', 'jmp_buf *rt_jmpbuf_for(uint32_t guest_jmpbuf);']
    hdr += [f'void {s}(Cpu *cpu);' for s in sorted(all_syms)]
    hdr += [f'void f_{e:08x}(Cpu *restrict cpu);' for e in sorted(all_entries)]
    hdr += [f'void orig_f_{e:08x}(Cpu *restrict cpu);' for e in sorted(overrides)]
    write_if_changed(os.path.join(root, 'gen.h'), '\n'.join(hdr) + '\n')
    for m, L, entries in lifters:
        if m['name'] not in want:
            continue
        outdir = os.path.join(root, m['name'])
        os.makedirs(outdir, exist_ok=True)
        nfile = lines_total = 0
        for k in range(0, len(entries), per_file):
            chunk = entries[k:k + per_file]
            out = ['#include "../gen.h"', '#pragma clang diagnostic ignored "-Wunused-label"', '']
            for e in chunk:
                out += L.lift_function(e)
                out.append('')
            lines_total += len(out)
            write_if_changed(os.path.join(outdir, f'gen_{nfile:03d}.c'), '\n'.join(out) + '\n')
            nfile += 1
        for f in os.listdir(outdir):   # stale chunks
            if f.startswith('gen_') and int(f[4:7]) >= nfile:
                os.remove(os.path.join(outdir, f))
        print(f'{m["name"]}: {len(entries)} functions, {nfile} files, {lines_total} lines', file=sys.stderr)
        print('  unsupported:', dict(sorted(L.unsupported.items(), key=lambda x: -x[1])[:40]), file=sys.stderr)
    # combined tables
    t = ['#include "gen.h"', 'const struct { uint32_t addr; GuestFn fn; } g_dispatch_table[] = {']
    t += [f'  {{0x{e:08x}u, f_{e:08x}}},' for e in sorted(all_entries)]
    t += ['};', f'const uint32_t g_dispatch_count = {len(all_entries)};']
    write_if_changed(os.path.join(root, 'dispatch_table.c'), '\n'.join(t) + '\n')
    t = ['#include "gen.h"', 'const char *const g_module_names[] = {' + ', '.join(f'"{m["name"]}"' for m in MODULES) + '};',
         'const char *const g_module_files[] = {' + ', '.join(f'"{os.path.basename(m["file"])}"' for m in MODULES) + '};',
         f'const uint32_t g_module_count = {len(MODULES)};',
         f'const uint32_t g_exe_entry = 0x{MODULES[0]["entry"]:08x}u;',
         'const struct ImportDef { uint32_t module; uint32_t iat; const char *dll; const char *name; GuestFn fn; } g_imports[] = {']
    t += [f'  {{{mi}, 0x{imp.iat:08x}u, "{imp.dll}", "{imp.name}", {s}}},' for mi, imp, s in import_rows]
    t += ['};', f'const uint32_t g_import_count = {len(import_rows)};',
          'const struct ExportDef { uint32_t module; const char *name; uint32_t va; } g_exports[] = {']
    t += [f'  {{{mi}, "{n}", 0x{va:08x}u}},' for mi, n, va in all_exports]
    t += ['};', f'const uint32_t g_export_count = {len(all_exports)};']
    write_if_changed(os.path.join(root, 'import_table.c'), '\n'.join(t) + '\n')
    t = ['#include "gen.h"', 'void rt_unimplemented(Cpu *cpu, const char *name) __attribute__((noreturn));']
    t += [f'__attribute__((weak)) void {s}(Cpu *cpu) {{ rt_unimplemented(cpu, "{imp.dll}!{imp.name}"); }}'
          for s, imp in sorted(all_syms.items())]
    write_if_changed(os.path.join(root, 'import_stubs.c'), '\n'.join(t) + '\n')


if __name__ == '__main__':
    from modules import MODULES
    main(sys.argv[1:] or [m['name'] for m in MODULES])
