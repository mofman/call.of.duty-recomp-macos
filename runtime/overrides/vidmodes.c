// Extended r_mode table (widescreen + native display resolution).
//
// The exe keeps a Quake 3 style r_vidModes[13] table ({desc, w, h, pixelAspect}) with its count right after it;
// only R_GetModeInfo and R_ModeList_f read them (CoDSP.exe: table 0x564a28, 0x4a5610 / 0x4a5680;
// CoDMP.exe: table 0x5b21c0, 0x4bbd60 / 0x4bbdd0 - byte-identical code). Both are replaced here by a host
// table that keeps modes 0-12 unchanged (existing configs stay valid) and appends widescreen modes from 13 on.
#include "../rt.h"
#include "../platform.h"
#include "gen.h"

int plat_desktop_w(void);
int plat_desktop_h(void);
int plat_mac_fullscreen_size(int *w, int *h);   // runtime/mac.m: below the notch

typedef struct { const char *desc; int w, h; } VidMode;
static const VidMode k_modes[] = {
    {"Mode  0: 320x240", 320, 240},           {"Mode  1: 400x300", 400, 300},
    {"Mode  2: 512x384", 512, 384},           {"Mode  3: 640x480", 640, 480},
    {"Mode  4: 800x600", 800, 600},           {"Mode  5: 960x720", 960, 720},
    {"Mode  6: 1024x768", 1024, 768},         {"Mode  7: 1152x864", 1152, 864},
    {"Mode  8: 1280x1024", 1280, 1024},       {"Mode  9: 1600x1200", 1600, 1200},
    {"Mode 10: 2048x1536", 2048, 1536},       {"Mode 11: 856x480 (wide)", 856, 480},
    {"Mode 12: 1920x1200 (wide)", 1920, 1200},
    {"Mode 13: 1280x720 (16:9)", 1280, 720},  {"Mode 14: 1366x768 (16:9)", 1366, 768},
    {"Mode 15: 1600x900 (16:9)", 1600, 900},  {"Mode 16: 1920x1080 (16:9)", 1920, 1080},
    {"Mode 17: 2560x1440 (16:9)", 2560, 1440}, {"Mode 18: 3840x2160 (16:9)", 3840, 2160},
    {"Mode 19: 1280x800 (16:10)", 1280, 800}, {"Mode 20: 1440x900 (16:10)", 1440, 900},
    {"Mode 21: 1680x1050 (16:10)", 1680, 1050}, {"Mode 22: 2560x1600 (16:10)", 2560, 1600},
    {"Mode 23: native display", 0, 0},        // filled from the desktop size
};
#define NMODES ((int)(sizeof k_modes / sizeof *k_modes))

#ifdef COD_MP
#define GETMODEINFO_FN   f_004bbd60
#define GETMODEINFO_ORIG orig_f_004bbd60
#define MODELIST_FN      f_004bbdd0
#else
#define GETMODEINFO_FN   f_004a5610
#define GETMODEINFO_ORIG orig_f_004a5610
#define MODELIST_FN      f_004a5680
#endif

int rt_vidmode_count(void) { return NMODES; }
int rt_vidmode_size(int i, int *w, int *h) {
    if (i < 0 || i >= NMODES) return 0;
    *w = k_modes[i].w; *h = k_modes[i].h;
    if (!*w && !plat_mac_fullscreen_size(w, h)) { *w = plat_desktop_w(); *h = plat_desktop_h(); }
    return 1;
}

// qboolean R_GetModeInfo(int *width /*[esp+4]*/, int *height /*edx*/, float *windowAspect /*ecx*/, int mode /*eax*/)
void GETMODEINFO_FN(Cpu *restrict cpu) {
    uint8_t *const m = g_mem; (void)m;
    int32_t mode = (int32_t)cpu->eax;
    if (mode == -1) { GETMODEINFO_ORIG(cpu); return; }   // r_customwidth / r_customheight
    int w, h;
    if (!rt_vidmode_size(mode, &w, &h)) { cpu->eax = 0; cpu->esp += 4; return; }
    g_wr32(ARG(0), (uint32_t)w);
    g_wr32(cpu->edx, (uint32_t)h);
    WRF32(cpu->ecx, (double)w / (double)h);
    cpu->eax = 1;
    cpu->esp += 4;
}

// R_ModeList_f: console "modelist"
void MODELIST_FN(Cpu *restrict cpu) {
    printf("\n");
    for (int i = 0; i < NMODES; i++) {
        int w, h;
        rt_vidmode_size(i, &w, &h);
        if (k_modes[i].w) printf("%s\n", k_modes[i].desc);
        else printf("%s (%dx%d)\n", k_modes[i].desc, w, h);
    }
    printf("\n");
    fflush(stdout);
    cpu->esp += 4;
}
