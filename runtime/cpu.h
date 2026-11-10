// Guest x86-32 CPU state + helpers used by generated code.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct Cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    // lazy flags: last flag-setting op
    uint32_t cc_op, cc_sz, cc_a, cc_b, cc_r, cc_c;
    uint32_t df;           // direction flag (0/1)
    uint32_t fs_base;      // TIB guest address
    // x87
    double st[8];
    uint32_t ftop;
    uint32_t fsw;          // C0=0x100 C1=0x200 C2=0x400 C3=0x4000
    uint32_t fcw;
    uint32_t thread_index;
} Cpu;

typedef void (*GuestFn)(Cpu *);

extern uint8_t *g_mem;     // base of 4 GB guest arena

// ---------------------------------------------------------------- memory
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float    __attribute__((aligned(1), may_alias)) f32u;
typedef double   __attribute__((aligned(1), may_alias)) f64u;

#define PTR(a)      (m + (uint32_t)(a))
#define RD8(a)      (*(uint8_t *)PTR(a))
#define RD16(a)     (*(u16u *)PTR(a))
#define RD32(a)     (*(u32u *)PTR(a))
#define RD64(a)     (*(u64u *)PTR(a))
#define RDF32(a)    ((double)*(f32u *)PTR(a))
#define RDF64(a)    (*(f64u *)PTR(a))
#define WR8(a, v)   (*(uint8_t *)PTR(a) = (uint8_t)(v))
#define WR16(a, v)  (*(u16u *)PTR(a) = (uint16_t)(v))
#define WR32(a, v)  (*(u32u *)PTR(a) = (uint32_t)(v))
#define WR64(a, v)  (*(u64u *)PTR(a) = (uint64_t)(v))
#define WRF32(a, v) (*(f32u *)PTR(a) = (float)(v))
#define WRF64(a, v) (*(f64u *)PTR(a) = (double)(v))

// host-side accessors (no local `m`)
static inline uint8_t *gptr(uint32_t a) { return g_mem + a; }
static inline uint32_t g_rd32(uint32_t a) { return *(u32u *)(g_mem + a); }
static inline void g_wr32(uint32_t a, uint32_t v) { *(u32u *)(g_mem + a) = v; }
static inline uint16_t g_rd16(uint32_t a) { return *(u16u *)(g_mem + a); }
static inline void g_wr16(uint32_t a, uint16_t v) { *(u16u *)(g_mem + a) = v; }

#define PUSH32(v) do { uint32_t _v = (v); cpu->esp -= 4; WR32(cpu->esp, _v); } while (0)
#define POP32()   (cpu->esp += 4, RD32(cpu->esp - 4))

// sub-registers
#define R8L(r)  ((uint8_t)(r))
#define R8H(r)  ((uint8_t)((r) >> 8))
#define R16(r)  ((uint16_t)(r))
#define W8L(r, v) ((r) = ((r) & 0xffffff00u) | ((uint32_t)(v) & 0xffu))
#define W8H(r, v) ((r) = ((r) & 0xffff00ffu) | (((uint32_t)(v) & 0xffu) << 8))
#define W16(r, v) ((r) = ((r) & 0xffff0000u) | ((uint32_t)(v) & 0xffffu))

// ---------------------------------------------------------------- flags
enum { CC_ADD = 1, CC_ADC, CC_SUB, CC_SBB, CC_LOGIC, CC_INC, CC_DEC, CC_NEG, CC_CO, CC_EXPL };
// CC_CO: cc_c bit0 = CF, bit1 = OF; ZF/SF/PF from cc_r
// CC_EXPL: cc_c = full eflags image

#define SETF(op, sz, a, b, r) (cpu->cc_op = (op), cpu->cc_sz = (sz), cpu->cc_a = (a), cpu->cc_b = (b), cpu->cc_r = (r))

static inline uint32_t sz_mask(uint32_t sz) { return sz == 4 ? 0xffffffffu : sz == 2 ? 0xffffu : 0xffu; }
static inline uint32_t sz_sign(uint32_t sz) { return sz == 4 ? 0x80000000u : sz == 2 ? 0x8000u : 0x80u; }
static inline int32_t sx(uint32_t v, uint32_t sz) { return sz == 4 ? (int32_t)v : sz == 2 ? (int16_t)v : (int8_t)v; }

static inline uint32_t f_cf(const Cpu *cpu) {
    uint32_t M = sz_mask(cpu->cc_sz), a = cpu->cc_a & M, b = cpu->cc_b & M, r = cpu->cc_r & M;
    switch (cpu->cc_op) {
    case CC_ADD: return r < a;
    case CC_ADC: return cpu->cc_c ? r <= a : r < a;
    case CC_SUB: return a < b;
    case CC_SBB: return cpu->cc_c ? a <= b : a < b;
    case CC_LOGIC: return 0;
    case CC_INC: case CC_DEC: return cpu->cc_c & 1;
    case CC_NEG: return a != 0;
    case CC_CO: return cpu->cc_c & 1;
    case CC_EXPL: return cpu->cc_c & 1;
    }
    return 0;
}
static inline uint32_t f_zf(const Cpu *cpu) {
    if (cpu->cc_op == CC_EXPL) return (cpu->cc_c >> 6) & 1;
    return (cpu->cc_r & sz_mask(cpu->cc_sz)) == 0;
}
static inline uint32_t f_sf(const Cpu *cpu) {
    if (cpu->cc_op == CC_EXPL) return (cpu->cc_c >> 7) & 1;
    return (cpu->cc_r & sz_sign(cpu->cc_sz)) != 0;
}
static inline uint32_t f_pf(const Cpu *cpu) {
    if (cpu->cc_op == CC_EXPL) return (cpu->cc_c >> 2) & 1;
    return !__builtin_parity(cpu->cc_r & 0xff);
}
static inline uint32_t f_of(const Cpu *cpu) {
    uint32_t S = sz_sign(cpu->cc_sz), a = cpu->cc_a, b = cpu->cc_b, r = cpu->cc_r;
    switch (cpu->cc_op) {
    case CC_ADD: case CC_ADC: case CC_INC: return (((a ^ r) & (b ^ r)) & S) != 0;
    case CC_SUB: case CC_SBB: case CC_DEC: case CC_NEG: return (((a ^ b) & (a ^ r)) & S) != 0;
    case CC_LOGIC: return 0;
    case CC_CO: return (cpu->cc_c >> 1) & 1;
    case CC_EXPL: return (cpu->cc_c >> 11) & 1;
    }
    return 0;
}
static inline uint32_t get_eflags(const Cpu *cpu) {
    return 0x202 | f_cf(cpu) | (f_pf(cpu) << 2) | (f_zf(cpu) << 6) | (f_sf(cpu) << 7) | (cpu->df << 10) | (f_of(cpu) << 11);
}
static inline void set_eflags(Cpu *cpu, uint32_t f) {
    cpu->cc_op = CC_EXPL; cpu->cc_c = f; cpu->df = (f >> 10) & 1;
}

// conditions — fast paths for CMP/SUB and TEST/LOGIC fold away when cc_op is a known constant
static inline int c_z(const Cpu *cpu) { return f_zf(cpu); }
static inline int c_b(const Cpu *cpu) { return f_cf(cpu); }
static inline int c_be(const Cpu *cpu) {
    if (cpu->cc_op == CC_SUB) { uint32_t M = sz_mask(cpu->cc_sz); return (cpu->cc_a & M) <= (cpu->cc_b & M); }
    return f_cf(cpu) | f_zf(cpu);
}
static inline int c_s(const Cpu *cpu) { return f_sf(cpu); }
static inline int c_o(const Cpu *cpu) { return f_of(cpu); }
static inline int c_p(const Cpu *cpu) { return f_pf(cpu); }
static inline int c_l(const Cpu *cpu) {
    if (cpu->cc_op == CC_SUB) return sx(cpu->cc_a, cpu->cc_sz) < sx(cpu->cc_b, cpu->cc_sz);
    if (cpu->cc_op == CC_LOGIC) return f_sf(cpu);
    return f_sf(cpu) != f_of(cpu);
}
static inline int c_le(const Cpu *cpu) {
    if (cpu->cc_op == CC_SUB) return sx(cpu->cc_a, cpu->cc_sz) <= sx(cpu->cc_b, cpu->cc_sz);
    if (cpu->cc_op == CC_LOGIC) return f_sf(cpu) | f_zf(cpu);
    return f_zf(cpu) | (f_sf(cpu) != f_of(cpu));
}

// ---------------------------------------------------------------- x87
#define ST(i)   (cpu->st[(cpu->ftop + (i)) & 7])
#define FPUSH(v) do { double _v = (v); cpu->ftop = (cpu->ftop - 1) & 7; cpu->st[cpu->ftop] = _v; } while (0)
#define FPOP()  (cpu->ftop = (cpu->ftop + 1) & 7)

static inline void fcom_set(Cpu *cpu, double a, double b) {
    uint32_t sw = cpu->fsw & ~0x4500u;
    if (a != a || b != b) sw |= 0x4500;
    else if (a < b) sw |= 0x100;
    else if (a == b) sw |= 0x4000;
    cpu->fsw = sw;
}
static inline uint32_t fnstsw(const Cpu *cpu) { return (cpu->fsw & ~0x3800u) | ((cpu->ftop & 7) << 11); }

static inline double fround(const Cpu *cpu, double v) {
    switch ((cpu->fcw >> 10) & 3) {
    case 0: return nearbyint(v);   // host default mode = round-to-nearest-even
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline int32_t fist32(const Cpu *cpu, double v) {
    double r = fround(cpu, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return (int32_t)0x80000000;
    return (int32_t)r;
}
static inline int16_t fist16(const Cpu *cpu, double v) {
    double r = fround(cpu, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return (int16_t)0x8000;
    return (int16_t)r;
}
static inline int64_t fist64(const Cpu *cpu, double v) {
    double r = fround(cpu, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)r;
}
double f80_load(const uint8_t *p);
void f80_store(uint8_t *p, double v);
uint32_t fxam(Cpu *cpu);

// ---------------------------------------------------------------- runtime entry points
void dispatch(Cpu *cpu, uint32_t target);           // call semantics (return addr already pushed)
void trap(Cpu *cpu, uint32_t addr, const char *why) __attribute__((noreturn));
void rt_cpuid(Cpu *cpu);
uint64_t rt_rdtsc(void);
void rt_string(Cpu *cpu, int op, int size, int rep);  // see strops in runtime.c
enum { STR_MOVS, STR_STOS, STR_LODS, STR_SCAS, STR_CMPS };
enum { REP_NONE, REP_E, REP_NE };
