// CRT setjmp/longjmp for lifted code (Quake-style Com_Error -> longjmp to the frame loop).
//
// The lifter wraps every call to _setjmp3 in a host _setjmp() that lives in the calling lifted function's C frame
// (rt_jmpbuf_for maps the guest jmp_buf address to a host jmp_buf). The original _setjmp3 still runs and records
// ebp/ebx/edi/esi/esp/SEH registration in the guest jmp_buf; longjmp (overridden here) restores those guest
// registers and host-longjmps back into the lifted caller, which continues after the call with eax = value.
#include "../rt.h"
#include "gen.h"

#define MAX_JB 64
static struct { uint32_t guest; jmp_buf host; } g_jb[MAX_JB];
static int g_njb;

jmp_buf *rt_jmpbuf_for(uint32_t guest) {
    for (int i = 0; i < g_njb; i++) if (g_jb[i].guest == guest) return &g_jb[i].host;
    if (g_njb >= MAX_JB) { fprintf(stderr, "too many setjmp buffers\n"); abort(); }
    g_jb[g_njb].guest = guest;
    return &g_jb[g_njb++].host;
}

// void __cdecl longjmp(jmp_buf env, int value)
#ifdef COD_MP
#define LONGJMP_VA 0x55fc80          // CoDMP.exe
#define LONGJMP_FN f_0055fc80
#else
#define LONGJMP_VA 0x51ca3c          // CoDSP.exe
#define LONGJMP_FN f_0051ca3c
#endif
void LONGJMP_FN(Cpu *restrict cpu) {
    uint32_t jb = ARG(0), val = ARG(1);
    jmp_buf *h = NULL;
    for (int i = 0; i < g_njb; i++) if (g_jb[i].guest == jb) h = &g_jb[i].host;
    if (!h) trap(cpu, LONGJMP_VA, "longjmp to a jmp_buf without a recorded host setjmp");
    cpu->ebp = g_rd32(jb);
    cpu->ebx = g_rd32(jb + 4);
    cpu->edi = g_rd32(jb + 8);
    cpu->esi = g_rd32(jb + 12);
    cpu->esp = g_rd32(jb + 16) + 4;           // past _setjmp3's return address (as the CRT does)
    g_wr32(cpu->fs_base, g_rd32(jb + 0x18));  // SEH registration at setjmp time
    cpu->eax = val ? val : 1;
    _longjmp(*h, 1);
}
