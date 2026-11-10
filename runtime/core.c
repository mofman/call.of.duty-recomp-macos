// Core runtime: guest memory, PE mapping, dispatch, thunks, traps, string ops, x87 helpers.
#include "rt.h"
#include <sys/mman.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <limits.h>
#include <execinfo.h>

uint8_t *g_mem;
int g_trace_imports;
uint32_t g_hinstance = 0x400000;
static char g_game_dir[PATH_MAX];
static char g_exe_path[PATH_MAX];
static char g_bin_dir[PATH_MAX];
const char *rt_bin_dir(void) { return g_bin_dir; }
static char g_cmdline_buf[4096];
const char *rt_cmdline(void) { return g_cmdline_buf; }

extern const struct { uint32_t addr; GuestFn fn; } g_dispatch_table[];
extern const uint32_t g_dispatch_count;
extern const struct ImportDef { uint32_t module; uint32_t iat; const char *dll; const char *name; GuestFn fn; } g_imports[];
extern const uint32_t g_import_count;
extern const struct ExportDef { uint32_t module; const char *name; uint32_t va; } g_exports[];
extern const uint32_t g_export_count;
extern const char *const g_module_names[];
extern const char *const g_module_files[];
extern const uint32_t g_module_count;
extern const uint32_t g_exe_entry;

// ------------------------------------------------------------------ thunks
#define THUNK_BASE 0xFFF00000u
#define THUNK_MAX  8192
static struct { GuestFn fn; const char *name; } g_thunks[THUNK_MAX];
static uint32_t g_nthunks;
static pthread_mutex_t g_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

__thread const char *t_thunk_name;

uint32_t rt_thunk(GuestFn fn, const char *name) {
    pthread_mutex_lock(&g_thunk_lock);
    for (uint32_t i = 0; i < g_nthunks; i++)
        if (g_thunks[i].fn == fn && (g_thunks[i].name == name || (name && g_thunks[i].name && !strcmp(g_thunks[i].name, name)))) {
            pthread_mutex_unlock(&g_thunk_lock); return THUNK_BASE + 16 * i;
        }
    if (g_nthunks >= THUNK_MAX) { fprintf(stderr, "thunk table full\n"); abort(); }
    g_thunks[g_nthunks].fn = fn;
    g_thunks[g_nthunks].name = name;
    uint32_t a = THUNK_BASE + 16 * g_nthunks++;
    pthread_mutex_unlock(&g_thunk_lock);
    return a;
}

// ------------------------------------------------------------------ dispatch
static __thread uint32_t t_last_addr;
static __thread GuestFn t_last_fn;

static GuestFn lookup(uint32_t a) {
    uint32_t lo = 0, hi = g_dispatch_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_dispatch_table[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    if (lo < g_dispatch_count && g_dispatch_table[lo].addr == a) return g_dispatch_table[lo].fn;
    return NULL;
}

// true if a lifted function starts at or below a (within 64 KB): cheap "is this a code address" test
static int lookup_containing(uint32_t a) {
    uint32_t lo = 0, hi = g_dispatch_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_dispatch_table[mid].addr <= a) lo = mid + 1; else hi = mid;
    }
    return lo > 0 && a - g_dispatch_table[lo - 1].addr < 0x10000 && a != g_dispatch_table[lo - 1].addr;
}

void dispatch(Cpu *cpu, uint32_t target) {
    if (target == t_last_addr && t_last_fn) { t_last_fn(cpu); return; }
    if (target >= THUNK_BASE) {
        uint32_t i = (target - THUNK_BASE) / 16;
        if (i < g_nthunks && (target & 15) == 0) { t_thunk_name = g_thunks[i].name; g_thunks[i].fn(cpu); return; }
        trap(cpu, target, "bad thunk address");
    }
    GuestFn f = lookup(target);
    if (!f) trap(cpu, target, "dispatch: no lifted function at target");
    t_last_addr = target; t_last_fn = f;
    f(cpu);
}

uint32_t call_guest(Cpu *cpu, uint32_t fn, int cdecl_, int nargs, ...) {
    va_list ap;
    uint32_t args[16];
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = cpu->esp;
    for (int i = nargs - 1; i >= 0; i--) { cpu->esp -= 4; g_wr32(cpu->esp, args[i]); }
    cpu->esp -= 4; g_wr32(cpu->esp, 0xFFFFFFF0u);   // fake return address
    dispatch(cpu, fn);
    if (cdecl_) cpu->esp += 4 * nargs;
    if (cpu->esp != esp0)
        LOG("call_guest(%08x): esp imbalance %d bytes", fn, (int)(cpu->esp - esp0));
    cpu->esp = esp0;
    return cpu->eax;
}

// ------------------------------------------------------------------ traps
static void host_bt(void) {
    void *f[64];
    int n = backtrace(f, 64);
    char **sy = backtrace_symbols(f, n);
    fprintf(stderr, "  host/guest call chain:");
    for (int i = 0; i < n; i++) {
        const char *q = strstr(sy[i], " f_");
        if (!q) q = strstr(sy[i], " imp_");
        if (!q) q = strstr(sy[i], " S_"); 
        if (q) { char b[64]; sscanf(q + 1, "%63s", b); fprintf(stderr, " %s", b); }
    }
    fprintf(stderr, "\n");
}

void rt_host_bt(void) { host_bt(); }
static void dump(Cpu *cpu) {
    host_bt();
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x\n  esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi);
    fprintf(stderr, "  stack:");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%s%08x", i % 8 ? " " : "\n    ", g_rd32(cpu->esp + 4 * i));
    fprintf(stderr, "\n  likely return addresses on stack:");
    int n = 0;
    for (uint32_t a = cpu->esp; a < cpu->esp + 0x2000 && n < 24; a += 4) {
        uint32_t v = g_rd32(a);
        if (v >= 0x401000 && v < 0x50000000 && lookup_containing(v)) { fprintf(stderr, " %08x", v); n++; }
    }
    fprintf(stderr, "\n");
}

void trap(Cpu *cpu, uint32_t addr, const char *why) {
    fprintf(stderr, "\n*** TRAP at %08x: %s\n", addr, why);
    dump(cpu);
    fflush(stderr);
    abort();
}

void rt_unimplemented(Cpu *cpu, const char *name) {
    fprintf(stderr, "\n*** UNIMPLEMENTED IMPORT: %s (called from %08x)\n", name, g_rd32(cpu->esp));
    dump(cpu);
    fflush(stderr);
    exit(3);
}

// ------------------------------------------------------------------ guest heap
// Regions: images < 0x50000000 (exe 0x400000, game 0x20000000, cgame 0x30000000, ui 0x40000000),
// heap 0x50000000-0xB0000000, valloc 0xB0000000-0xF0000000, host thunks 0xFFF00000+.
#define HEAP_LO 0x50000000u
#define HEAP_HI 0xB0000000u
#define VA_LO   0xB0000000u
#define VA_HI   0xF0000000u
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_top = HEAP_LO, g_va_top = VA_LO;
#define NBUCKET 56
static uint32_t g_free[NBUCKET];   // singly linked via first dword

static int bucket_of(uint32_t sz, uint32_t *rounded) {
    // 16-byte granularity buckets up to 512, then powers of two
    if (sz <= 512) { uint32_t r = (sz + 15) & ~15u; if (!r) r = 16; *rounded = r; return r / 16 - 1; }
    int b = 32 - __builtin_clz(sz - 1);      // ceil log2
    *rounded = 1u << b;
    return 32 + (b - 10);                      // 1K -> 32 ...
}

uint32_t galloc(uint32_t size) {
    uint32_t r;
    int b = bucket_of(size, &r);
    if (b >= NBUCKET) b = -1;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t p = 0;
    if (b >= 0 && g_free[b]) {
        p = g_free[b];
        g_free[b] = g_rd32(p);
    } else {
        if (b < 0) r = (size + 4095) & ~4095u;
        uint32_t hdr = g_heap_top;
        if (hdr + 16 + r > HEAP_HI) { pthread_mutex_unlock(&g_heap_lock); LOG("guest heap exhausted"); return 0; }
        g_heap_top += 16 + r;
        g_wr32(hdr, r);                // [p-16] capacity, [p-12] requested size, [p-8] magic
        g_wr32(hdr + 8, 0x48454150);   // 'HEAP'
        p = hdr + 16;
    }
    pthread_mutex_unlock(&g_heap_lock);
    g_wr32(p - 12, size);
    memset(gptr(p), 0, size);
    return p;
}

uint32_t gsize(uint32_t p) { return p ? g_rd32(p - 12) : 0; }

void gfree(uint32_t p) {
    if (!p) return;
    if (g_rd32(p - 8) != 0x48454150) { LOG("gfree: bad pointer %08x", p); return; }
    uint32_t r = g_rd32(p - 16), rr;
    int b = bucket_of(r, &rr);
    if (b < 0 || b >= NBUCKET || rr != r) return;   // large blocks leak (rare)
    pthread_mutex_lock(&g_heap_lock);
    g_wr32(p, g_free[b]);
    g_free[b] = p;
    pthread_mutex_unlock(&g_heap_lock);
}

uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return galloc(size);
    uint32_t cap = g_rd32(p - 16), old = gsize(p);
    if (size <= cap) { if (size > old) memset(gptr(p + old), 0, size - old); g_wr32(p - 12, size); return p; }
    uint32_t q = galloc(size);
    if (!q) return 0;
    memcpy(gptr(q), gptr(p), old < size ? old : size);
    gfree(p);
    return q;
}

uint32_t gvalloc(uint32_t size) {
    size = (size + 0xffff) & ~0xffffu;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t p = g_va_top;
    if (p + size > VA_HI) { pthread_mutex_unlock(&g_heap_lock); return 0; }
    g_va_top += size;
    pthread_mutex_unlock(&g_heap_lock);
    return p;
}

uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = galloc(n);
    memcpy(gptr(p), s, n);
    return p;
}

// ------------------------------------------------------------------ handles
#define MAX_HANDLES 4096
static RtHandle g_handles[MAX_HANDLES];
static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;

uint32_t h_new(HKind kind, void *p) {
    pthread_mutex_lock(&g_hlock);
    for (int i = 16; i < MAX_HANDLES; i++) {
        if (g_handles[i].kind == H_FREE) {
            g_handles[i] = (RtHandle){kind, p, -1, 0};
            pthread_mutex_unlock(&g_hlock);
            return 0x1000 + 4 * i;
        }
    }
    pthread_mutex_unlock(&g_hlock);
    LOG("out of handles");
    return 0;
}
RtHandle *h_get(uint32_t h) {
    if (h < 0x1000 || (h & 3)) return NULL;
    uint32_t i = (h - 0x1000) / 4;
    if (i >= MAX_HANDLES || g_handles[i].kind == H_FREE) return NULL;
    return &g_handles[i];
}
void h_free(uint32_t h) {
    RtHandle *x = h_get(h);
    if (x) x->kind = H_FREE;
}

// ------------------------------------------------------------------ threads / TIB
static __thread Cpu *t_cpu;
static __thread uint32_t t_last_error;
Cpu *rt_cpu(void) { return t_cpu; }
static Cpu *g_main_cpu;
int rt_is_main_thread(void) { return t_cpu == g_main_cpu; }
void rt_set_last_error(uint32_t e) { t_last_error = e; }
uint32_t rt_last_error(void) { return t_last_error; }

static uint32_t g_next_tid = 0x100;

uint32_t rt_new_thread_cpu(Cpu *c) {
    memset(c, 0, sizeof *c);
    uint32_t stack = gvalloc(GUEST_STACK_SIZE);
    uint32_t tib = galloc(0x1000);
    g_wr32(tib + 0x00, 0xFFFFFFFFu);                // SEH chain end
    g_wr32(tib + 0x04, stack + GUEST_STACK_SIZE);   // stack base (top)
    g_wr32(tib + 0x08, stack);                      // stack limit
    g_wr32(tib + 0x18, tib);                        // self
    g_wr32(tib + 0x20, 0x42);                       // pid
    g_wr32(tib + 0x24, __atomic_fetch_add(&g_next_tid, 4, __ATOMIC_SEQ_CST));
    c->fs_base = tib;
    c->esp = stack + GUEST_STACK_SIZE - 64;
    c->fcw = 0x27f;
    t_cpu = c;
    return c->esp;
}

// ------------------------------------------------------------------ string ops
void rt_string(Cpu *cpu, int op, int size, int rep) {
    uint8_t *m = g_mem;
    int32_t step = cpu->df ? -size : size;
    uint32_t mask = sz_mask(size);
    if (rep != REP_NONE && cpu->ecx == 0) return;
    // fast paths
    if (rep != REP_NONE && !cpu->df && op == STR_MOVS) {
        uint32_t n = cpu->ecx * size;
        memmove(PTR(cpu->edi), PTR(cpu->esi), n);
        cpu->esi += n; cpu->edi += n; cpu->ecx = 0;
        return;
    }
    if (rep != REP_NONE && !cpu->df && op == STR_STOS) {
        uint32_t n = cpu->ecx;
        if (size == 1) memset(PTR(cpu->edi), (uint8_t)cpu->eax, n);
        else for (uint32_t k = 0; k < n; k++) {
            if (size == 2) WR16(cpu->edi + 2 * k, cpu->eax); else WR32(cpu->edi + 4 * k, cpu->eax);
        }
        cpu->edi += n * size; cpu->ecx = 0;
        return;
    }
    for (;;) {
        uint32_t a = 0, b = 0;
        switch (op) {
        case STR_MOVS:
            if (size == 1) WR8(cpu->edi, RD8(cpu->esi)); else if (size == 2) WR16(cpu->edi, RD16(cpu->esi));
            else WR32(cpu->edi, RD32(cpu->esi));
            cpu->esi += step; cpu->edi += step; break;
        case STR_STOS:
            if (size == 1) WR8(cpu->edi, cpu->eax); else if (size == 2) WR16(cpu->edi, cpu->eax); else WR32(cpu->edi, cpu->eax);
            cpu->edi += step; break;
        case STR_LODS:
            if (size == 1) W8L(cpu->eax, RD8(cpu->esi)); else if (size == 2) W16(cpu->eax, RD16(cpu->esi));
            else cpu->eax = RD32(cpu->esi);
            cpu->esi += step; break;
        case STR_SCAS:
            a = cpu->eax & mask;
            b = size == 1 ? RD8(cpu->edi) : size == 2 ? RD16(cpu->edi) : RD32(cpu->edi);
            SETF(CC_SUB, size, a, b, (a - b) & mask);
            cpu->edi += step; break;
        case STR_CMPS:
            a = size == 1 ? RD8(cpu->esi) : size == 2 ? RD16(cpu->esi) : RD32(cpu->esi);
            b = size == 1 ? RD8(cpu->edi) : size == 2 ? RD16(cpu->edi) : RD32(cpu->edi);
            SETF(CC_SUB, size, a, b, (a - b) & mask);
            cpu->esi += step; cpu->edi += step; break;
        }
        if (rep == REP_NONE) return;
        if (--cpu->ecx == 0) return;
        if (op == STR_SCAS || op == STR_CMPS) {
            int z = a == b;
            if (rep == REP_E && !z) return;
            if (rep == REP_NE && z) return;
        }
    }
}

void rt_cpuid(Cpu *cpu) {
    switch (cpu->eax) {
    case 0: cpu->eax = 1; cpu->ebx = 0x756e6547; cpu->edx = 0x49656e69; cpu->ecx = 0x6c65746e; break; // GenuineIntel
    case 1: cpu->eax = 0x683; cpu->ebx = 0; cpu->ecx = 0; cpu->edx = 0x00000011; break;           // FPU + TSC
    default: cpu->eax = cpu->ebx = cpu->ecx = cpu->edx = 0;
    }
}

// ------------------------------------------------------------------ x87 80-bit
double f80_load(const uint8_t *p) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
    int sign = se >> 15, exp = se & 0x7fff;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
void f80_store(uint8_t *p, double v) {
    uint64_t mant = 0; uint16_t se = 0;
    if (v != 0) {
        int sign = signbit(v);
        if (isinf(v)) { se = 0x7fff; mant = 0x8000000000000000ull; }
        else if (isnan(v)) { se = 0x7fff; mant = 0xC000000000000000ull; }
        else {
            int e; double f = frexp(fabs(v), &e);  // f in [0.5,1)
            mant = (uint64_t)ldexp(f, 64);
            se = (uint16_t)(e - 1 + 16383);
        }
        if (sign) se |= 0x8000;
    } else if (signbit(v)) se = 0x8000;
    memcpy(p, &mant, 8); memcpy(p + 8, &se, 2);
}
uint32_t fxam(Cpu *cpu) {
    double v = ST(0);
    uint32_t c = 0;
    if (isnan(v)) c = 0x100;
    else if (isinf(v)) c = 0x500;
    else if (v == 0) c = 0x4000;
    else if (fpclassify(v) == FP_SUBNORMAL) c = 0x4400;
    else c = 0x400;
    if (signbit(v)) c |= 0x200;
    cpu->fsw = (cpu->fsw & ~0x4700u) | c;
    return c;
}

// ------------------------------------------------------------------ paths
const char *rt_game_dir(void) { return g_game_dir; }

static char g_cwd[PATH_MAX] = "";   // guest cwd relative to game dir, using '/'

// Resolve one component case-insensitively inside dir.
static int ci_component(char *dir, const char *comp, size_t cap) {
    size_t dl = strlen(dir);
    snprintf(dir + dl, cap - dl, "/%s", comp);
    struct stat st;
    if (stat(dir, &st) == 0) return 1;
    dir[dl] = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcasecmp(e->d_name, comp) == 0) {
                snprintf(dir + dl, cap - dl, "/%s", e->d_name);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    snprintf(dir + dl, cap - dl, "/%s", comp);
    return 0;
}

char *rt_map_path(const char *wp, char *out, size_t cap) {
    char tmp[PATH_MAX];
    size_t n = 0;
    const char *s = wp;
    int absolute = 0;
    if (((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':') { s += 2; absolute = 1; }
    if (*s == '\\' || *s == '/') absolute = 1;
    if (!absolute && g_cwd[0]) n = snprintf(tmp, sizeof tmp, "%s/", g_cwd);
    for (; *s && n < sizeof tmp - 1; s++) tmp[n++] = (*s == '\\') ? '/' : *s;
    tmp[n] = 0;
    // normalise components
    char *comps[256]; int nc = 0;
    char *save = NULL;
    for (char *c = strtok_r(tmp, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) { if (nc) nc--; continue; }
        if (nc < 256) comps[nc++] = c;
    }
    // the virtual install dir C:\CoD is the game dir itself
    int first = 0;
    if (absolute && nc && !strcasecmp(comps[0], RT_VIRTUAL_INSTALL_DIR)) first = 1;
    snprintf(out, cap, "%s", g_game_dir);
    for (int i = first; i < nc; i++) ci_component(out, comps[i], cap);
    return out;
}

void rt_set_cwd(const char *wp) {
    char host[PATH_MAX];
    rt_map_path(wp, host, sizeof host);
    size_t gl = strlen(g_game_dir);
    if (strncmp(host, g_game_dir, gl) == 0) snprintf(g_cwd, sizeof g_cwd, "%s", host + gl + (host[gl] == '/'));
}
const char *rt_get_cwd(void) { return g_cwd; }
const char *rt_exe_path(void) { return g_exe_path; }

// ------------------------------------------------------------------ PE loading
#include <fcntl.h>
typedef struct Module { uint32_t base, size, entry; int loaded, refs; char path[PATH_MAX]; } Module;
static Module g_mods[8];

static uint32_t map_image(int mi, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(len);
    fread(d, 1, len, f); fclose(f);
    uint32_t pe = *(uint32_t *)(d + 0x3c);
    uint16_t nsec = *(uint16_t *)(d + pe + 6), optsz = *(uint16_t *)(d + pe + 20);
    uint32_t opt = pe + 24, base = *(uint32_t *)(d + opt + 28), hdrsz = *(uint32_t *)(d + opt + 60);
    uint32_t size = *(uint32_t *)(d + opt + 56);
    memset(gptr(base), 0, size);            // reload re-initialises .data/.bss
    memcpy(gptr(base), d, hdrsz);
    for (int i = 0; i < nsec; i++) {
        uint8_t *s = d + opt + optsz + 40 * i;
        uint32_t va = *(uint32_t *)(s + 12), rs = *(uint32_t *)(s + 16), rp = *(uint32_t *)(s + 20);
        if (rp + rs > (uint32_t)len) rs = (uint32_t)len - rp;
        memcpy(gptr(base + va), d + rp, rs);
    }
    g_mods[mi].base = base;
    g_mods[mi].size = size;
    g_mods[mi].entry = base + *(uint32_t *)(d + opt + 16);
    snprintf(g_mods[mi].path, sizeof g_mods[mi].path, "%s", path);
    free(d);
    for (uint32_t i = 0; i < g_import_count; i++)
        if (g_imports[i].module == (uint32_t)mi)
            g_wr32(g_imports[i].iat, rt_thunk(g_imports[i].fn, g_imports[i].name));
    g_mods[mi].loaded = 1;
    return base;
}

static int module_index(const char *name) {
    const char *b = strrchr(name, '\\'), *b2 = strrchr(name, '/');
    if (b2 > b) b = b2;
    b = b ? b + 1 : name;
    for (uint32_t i = 0; i < g_module_count; i++)
        if (!strcasecmp(g_module_files[i], b)) return (int)i;
    return -1;
}

// LoadLibrary for lifted guest DLLs: (re)map image, run CRT DllMain(DLL_PROCESS_ATTACH). 0 if not a guest module.
uint32_t rt_load_module(Cpu *cpu, const char *name) {
    int mi = module_index(name);
    if (mi <= 0) return 0;
    Module *m = &g_mods[mi];
    if (m->loaded && m->refs > 0) { m->refs++; return m->base; }
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", rt_bin_dir(), g_module_files[mi]);
    if (access(p, R_OK) != 0) {
        DIR *dir = opendir(rt_bin_dir());
        struct dirent *entry;
        while (dir && (entry = readdir(dir))) {
            if (!strcasecmp(entry->d_name, "main")) {
                snprintf(p, sizeof p, "%s/%s/%s", rt_bin_dir(), entry->d_name, g_module_files[mi]);
                break;
            }
        }
        if (dir) closedir(dir);
    }
    map_image(mi, p);
    m->refs = 1;
    LOG("LoadLibrary(%s) -> %08x, DllMain", g_module_files[mi], m->base);
    uint32_t ok = call_guest(cpu, m->entry, 0, 3, m->base, 1, 0);
    if (!ok) { LOG("DllMain failed for %s", g_module_files[mi]); m->refs = 0; return 0; }
    return m->base;
}

int rt_free_module(Cpu *cpu, uint32_t h) {
    for (uint32_t i = 1; i < g_module_count; i++) {
        Module *m = &g_mods[i];
        if (m->loaded && m->base == h) {
            if (--m->refs == 0) {
                call_guest(cpu, m->entry, 0, 3, m->base, 0, 0);   // DLL_PROCESS_DETACH
                LOG("FreeLibrary(%s)", g_module_files[i]);
            }
            return 1;
        }
    }
    return 0;
}

uint32_t rt_module_export(uint32_t h, const char *name) {
    for (uint32_t i = 0; i < g_export_count; i++)
        if (g_mods[g_exports[i].module].base == h && !strcmp(g_exports[i].name, name)) return g_exports[i].va;
    return 0;
}

const char *rt_module_name_of(uint32_t h) {   // guest module file name for an HMODULE, or NULL
    for (uint32_t i = 0; i < g_module_count; i++)
        if (g_mods[i].loaded && g_mods[i].base == h) return g_module_files[i];
    return NULL;
}

uint32_t rt_import_thunk(const char *dll, const char *name) {
    for (uint32_t i = 0; i < g_import_count; i++)
        if (!strcasecmp(g_imports[i].name, name)) return g_rd32(g_imports[i].iat);
    (void)dll;
    return 0;
}

static void segv(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    uintptr_t a = (uintptr_t)si->si_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x100000000ull)
        fprintf(stderr, "\n*** guest memory fault at guest address %08x (sig %d)\n", (uint32_t)(a - (uintptr_t)g_mem), sig);
    else
        fprintf(stderr, "\n*** host fault at %p (sig %d)\n", si->si_addr, sig);
    Cpu *c = t_cpu;
    if (c) dump(c);
    signal(sig, SIG_DFL);
    abort();
}

void rt_platform_init(int argc, char **argv);   // platform.c (SDL)

void plat_mac_bundle_init(void);

int main(int argc, char **argv) {
    plat_mac_bundle_init();
    const char *gd = getenv("COD_GAME_DIR");
    if (!gd) gd = "gamedata";
    if (!realpath(gd, g_game_dir)) { fprintf(stderr, "game dir %s not found (set COD_GAME_DIR)\n", gd); return 1; }
    const char *bin = getenv("COD_BIN_DIR");
    if (!bin) bin = g_game_dir;
    if (!realpath(bin, g_bin_dir)) { fprintf(stderr, "binary dir %s not found (set COD_BIN_DIR)\n", bin); return 1; }
    snprintf(g_exe_path, sizeof g_exe_path, "%s/%s", g_bin_dir, g_module_files[0]);
    g_trace_imports = getenv("COD_TRACE") != NULL;
    {   // guest command line: "C:\CoD\CoDSP.exe" + host args (e.g. +set r_mode 6 +devmap ...)
        size_t n = snprintf(g_cmdline_buf, sizeof g_cmdline_buf, "\"C:\\%s\\%s\"", RT_VIRTUAL_INSTALL_DIR, RT_EXE_NAME);
        for (int i = 1; i < argc && n < sizeof g_cmdline_buf - 2; i++)
            n += snprintf(g_cmdline_buf + n, sizeof g_cmdline_buf - n, " %s", argv[i]);
    }

    g_mem = mmap(NULL, 0x100000000ull + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) { perror("mmap"); return 1; }
    mprotect(g_mem, 0x10000, PROT_NONE);   // null page guard
    struct sigaction sa = {0};
    sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);

    map_image(0, g_exe_path);
    g_mods[0].refs = 1;
    rt_platform_init(argc, argv);

    static Cpu main_cpu;
    rt_new_thread_cpu(&main_cpu);
    g_main_cpu = &main_cpu;
    uint32_t entry = g_exe_entry;   // real CRT entry (the PE entry is a wrapper stub that is not lifted)
    LOG("game dir %s, entry %08x", g_game_dir, entry);
    Cpu *cpu = &main_cpu;
    cpu->esp -= 4; g_wr32(cpu->esp, 0xFFFFFFF0u);
    dispatch(cpu, entry);
    LOG("entry returned eax=%08x", cpu->eax);
    return (int)cpu->eax;
}

#include <mach/mach_time.h>
uint64_t rt_rdtsc(void) {   // ~1 GHz-ish monotonic tick count
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
