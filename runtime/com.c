// Minimal COM object support: guest-visible objects whose vtables point at host thunks.
#include "rt.h"
#include <string.h>

void com_unimpl(Cpu *cpu) {
    fprintf(stderr, "\n*** UNIMPLEMENTED COM METHOD %s (this=%08x, caller %08x)\n", t_thunk_name ? t_thunk_name : "?", ARG(0), g_rd32(cpu->esp));
    trap(cpu, g_rd32(cpu->esp), "unimplemented COM method");
}

static void com_addref(Cpu *cpu) { uint32_t o = ARG(0); uint32_t r = g_rd32(o + 4) + 1; g_wr32(o + 4, r); RET(r, 1); }
static void com_release(Cpu *cpu) { uint32_t o = ARG(0); uint32_t r = g_rd32(o + 4); if (r) r--; g_wr32(o + 4, r); RET(r, 1); }
static void com_qi(Cpu *cpu) {
    // default: return self for any IID (interfaces we serve are binary-compatible prefixes)
    uint32_t o = ARG(0), out = ARG(2);
    g_wr32(out, o);
    g_wr32(o + 4, g_rd32(o + 4) + 1);
    RET(S_OK, 3);
}

uint32_t com_new(const char *iface, const ComMethod *methods, int n, void *host) {
    uint32_t vt = galloc(4 * n);
    for (int i = 0; i < n; i++) {
        GuestFn fn = methods[i].fn;
        char *nm = malloc(strlen(iface) + strlen(methods[i].name) + 3);
        sprintf(nm, "%s::%s", iface, methods[i].name);
        if (!fn) {
            if (i == 0) fn = com_qi; else if (i == 1) fn = com_addref; else if (i == 2) fn = com_release; else fn = com_unimpl;
        }
        g_wr32(vt + 4 * i, rt_thunk(fn, nm));
    }
    uint32_t o = galloc(64);
    g_wr32(o, vt);
    g_wr32(o + 4, 1);
    uint64_t h = (uint64_t)(uintptr_t)host;
    g_wr32(o + 8, (uint32_t)h);
    g_wr32(o + 12, (uint32_t)(h >> 32));
    return o;
}

void *com_host(uint32_t o) {
    return (void *)(uintptr_t)(((uint64_t)g_rd32(o + 12) << 32) | g_rd32(o + 8));
}
