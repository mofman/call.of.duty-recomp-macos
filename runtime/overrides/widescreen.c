// Hor+ field of view for widescreen modes (cgamex86.dll; cgame_mp_x86.dll in the MP build).
//
// CG_CalcViewValues (SP 0x30025c80, MP 0x30034bb0) tail-jumps into CG_CalcFov, which keeps the horizontal FOV at cg_fov and derives
// fov_y from the refdef aspect, so wider screens lose vertical view ("Vert-"). After it runs we treat the computed
// fov_x as the 4:3 value and widen it to the real aspect, keeping the 4:3 vertical FOV. zoomSensitivity (computed
// from the unwidened fov_x) is left alone so mouse feel is unchanged. COD_VERTMINUS=1 restores the original behaviour.
#include <math.h>
#include "../rt.h"
#include "gen.h"

#ifdef COD_MP
#define REFDEF_W    0x3020d338u          // cg.refdef in cgame_mp_x86.dll
#define REFDEF_H    0x3020d33cu
#define REFDEF_FOVX 0x3020d340u
#define REFDEF_FOVY 0x3020d344u
#define CALCVIEW_FN f_30034bb0
#define CALCVIEW_ORIG orig_f_30034bb0
#else
#define REFDEF_W    0x3012572cu          // cg.refdef in cgamex86.dll
#define REFDEF_H    0x30125730u
#define REFDEF_FOVX 0x30125734u
#define REFDEF_FOVY 0x30125738u
#define CALCVIEW_FN f_30025c80
#define CALCVIEW_ORIG orig_f_30025c80
#endif

void CALCVIEW_FN(Cpu *restrict cpu) {
    uint8_t *const m = g_mem; (void)m;
    static int off = -1;
    if (off < 0) off = getenv("COD_VERTMINUS") && atoi(getenv("COD_VERTMINUS"));
    CALCVIEW_ORIG(cpu);
    if (off) return;
    int32_t w = (int32_t)g_rd32(REFDEF_W), h = (int32_t)g_rd32(REFDEF_H);
    if (w <= 0 || h <= 0 || w * 3 <= h * 4) return;
    double fx = RDF32(REFDEF_FOVX) * M_PI / 360.0;   // half-angle, radians
    if (fx <= 0 || fx >= M_PI / 2) return;
    double ty = tan(fx) * 0.75;                       // tan(fov_y/2) at 4:3
    double fov_y = atan(ty) * 360.0 / M_PI;
    double fov_x = atan(ty * (double)w / (double)h) * 360.0 / M_PI;
    WRF32(REFDEF_FOVX, fov_x);
    WRF32(REFDEF_FOVY, fov_y);
    static int last_w, last_h;
    if (w != last_w || h != last_h) {
        last_w = w; last_h = h;
        LOG("Hor+ FOV for %dx%d: fov_x %.1f -> %.1f, fov_y %.1f", w, h, fx * 360.0 / M_PI, fov_x, fov_y);
    }
}
