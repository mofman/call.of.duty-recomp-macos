// Intro videos on wide screens (CoDSP.exe only: not built into the MP target).
//
// SCR_DrawScreenField (0x410410) queues a full-screen black StretchPic "to clear the sides" whenever
// vidWidth*480 > vidHeight*640 and cls.state != 2. Cinematic frames are drawn immediately by RE_StretchRaw, while
// StretchPics are queued until the end of the frame, so on any wide mode that black quad lands on top of the intro
// videos and they play as a black screen. The side bars are handled by the GL layer's pillarbox instead, so the
// clear is dropped: while SCR_DrawScreenField runs, the first RE_StretchPic (0x4d4330) is skipped if it is exactly
// that clear (0,0,vidWidth,vidHeight, zero texcoords, cls.whiteShader).
#include "../rt.h"
#include "gen.h"

#define CLS_VIDWIDTH    0x143942cu
#define CLS_VIDHEIGHT   0x1439430u
#define CLS_WHITESHADER 0x1439448u

static int g_skip_side_clear;

void f_00410410(Cpu *restrict cpu) {
    g_skip_side_clear = 1;
    orig_f_00410410(cpu);
    g_skip_side_clear = 0;
}

// void RE_StretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t shader)
void f_004d4330(Cpu *restrict cpu) {
    if (g_skip_side_clear) {
        g_skip_side_clear = 0;   // only the first StretchPic of the screen field can be the clear
        uint8_t *const m = g_mem; (void)m;
        float x = (float)RDF32(cpu->esp + 4), y = (float)RDF32(cpu->esp + 8);
        float w = (float)RDF32(cpu->esp + 12), h = (float)RDF32(cpu->esp + 16);
        int zero_st = ARG(4) == 0 && ARG(5) == 0 && ARG(6) == 0 && ARG(7) == 0;
        if (x == 0 && y == 0 && zero_st && ARG(8) == g_rd32(CLS_WHITESHADER) &&
            w == (float)(int32_t)g_rd32(CLS_VIDWIDTH) && h == (float)(int32_t)g_rd32(CLS_VIDHEIGHT)) {
            cpu->esp += 4;       // skip: cdecl, caller pops the arguments
            return;
        }
    }
    orig_f_004d4330(cpu);
}
