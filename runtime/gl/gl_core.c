// OpenGL 1.x fixed-function state tracker. Converts every draw into indexed triangle/line/point lists of GLVtx and
// hands them to the Metal backend (gl_metal.m). Single context, main thread only (the engine runs r_smp 0).
#include "../rt.h"
#include "gl_ctx.h"
#include "gl_impl.h"
#include <math.h>
#include <string.h>
#include <strings.h>

GLContext g_gl;
static bool g_inited;
static void lighting_defaults(void);

static void ctx_init(void) {
    if (g_inited) return;
    g_inited = true;
    GLContext *c = &g_gl;
    memset(c, 0, sizeof *c);
    for (int m = 0; m < 2 + GL_MAX_UNITS; m++)
        for (int i = 0; i < 16; i++) c->mstack[m][0][i] = (i % 5 == 0) ? 1.f : 0.f;
    c->cull_face = GL_BACK; c->front_face = GL_CCW; c->depth_func = GL_LESS;
    c->blend_src = GL_ONE; c->blend_dst = GL_ZERO; c->alpha_func = GL_ALWAYS;
    c->depth_mask = true;
    for (int i = 0; i < 4; i++) c->color_mask[i] = true;
    c->polygon_mode = GL_FILL;
    c->depth_near = 0; c->depth_far = 1; c->clear_depth = 1;
    c->stencil_func = GL_ALWAYS; c->stencil_mask = c->stencil_writemask = 0xff;
    c->stencil_fail = c->stencil_zfail = c->stencil_zpass = GL_KEEP;
    c->fog_mode = GL_EXP; c->fog_density = 1; c->fog_end = 1;
    for (int u = 0; u < GL_MAX_UNITS; u++) {
        GLTexEnv *e = &c->env[u];
        e->mode = GL_MODULATE; e->combine_rgb = e->combine_alpha = GL_MODULATE;
        e->src_rgb[0] = e->src_alpha[0] = GL_TEXTURE; e->src_rgb[1] = e->src_alpha[1] = GL_PREVIOUS_ARB;
        e->src_rgb[2] = e->src_alpha[2] = GL_CONSTANT_ARB;
        e->op_rgb[0] = e->op_rgb[1] = GL_SRC_COLOR; e->op_rgb[2] = GL_SRC_ALPHA;
        for (int k = 0; k < 3; k++) e->op_alpha[k] = GL_SRC_ALPHA;
        e->rgb_scale = e->alpha_scale = 1;
        c->cur_tc[u][3] = 1;
    }
    for (int i = 0; i < 4; i++) c->cur_color[i] = 1;
    c->cur_normal[2] = 1;
    c->unpack_align = c->pack_align = 4;
    c->va.size = 4; c->va.type = GL_FLOAT; c->ca.size = 4; c->ca.type = GL_FLOAT;
    for (int i = 0; i < 3; i++) for (int k = 0; k < 256; k++) c->gamma[i][k] = k / 255.f;
    c->gamma_identity = true;
    c->swap_interval = 1;
    lighting_defaults();
    mtl_init();
}

void gl_missing(const char *name) {
    static const char *seen[512]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == name) return;
    if (n < 512) seen[n++] = name;
    LOG("GL: %s not implemented (ignored)", name);
}

#define C (&g_gl)
static void set_error(GLenum e) { if (!C->error) C->error = e; }

// ------------------------------------------------------------------ matrices
void gl_mat_mul(float *o, const float *a, const float *b) {   // o = a * b (column-major)
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[j * 4 + i] = a[0 * 4 + i] * b[j * 4 + 0] + a[1 * 4 + i] * b[j * 4 + 1] + a[2 * 4 + i] * b[j * 4 + 2] + a[3 * 4 + i] * b[j * 4 + 3];
    memcpy(o, r, sizeof r);
}
const float *gl_top(int which) { return C->mstack[which][C->mdepth[which]]; }
static float *cur_m(void) { return C->mstack[C->matrix_mode][C->mdepth[C->matrix_mode]]; }
static void mult(const float *m) { gl_mat_mul(cur_m(), cur_m(), m); }

void impl_glMatrixMode(GLenum mode) {
    ctx_init();
    if (mode == GL_MODELVIEW) C->matrix_mode = 0;
    else if (mode == GL_PROJECTION) C->matrix_mode = 1;
    else if (mode == GL_TEXTURE) C->matrix_mode = 2 + C->active_unit;
    else set_error(GL_INVALID_ENUM);
}
void impl_glLoadIdentity(void) { float *m = cur_m(); for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.f : 0.f; }
void impl_glLoadMatrixf(const GLfloat *m) { memcpy(cur_m(), m, 64); }
void impl_glLoadMatrixd(const GLdouble *m) { float *d = cur_m(); for (int i = 0; i < 16; i++) d[i] = (float)m[i]; }
void impl_glMultMatrixf(const GLfloat *m) { float t[16]; memcpy(t, m, 64); mult(t); }
void impl_glMultMatrixd(const GLdouble *m) { float t[16]; for (int i = 0; i < 16; i++) t[i] = (float)m[i]; mult(t); }
void impl_glPushMatrix(void) {
    int w = C->matrix_mode;
    if (C->mdepth[w] + 1 >= GL_MATRIX_DEPTH) { set_error(0x0503); return; }
    memcpy(C->mstack[w][C->mdepth[w] + 1], C->mstack[w][C->mdepth[w]], 64);
    C->mdepth[w]++;
}
void impl_glPopMatrix(void) { int w = C->matrix_mode; if (C->mdepth[w] > 0) C->mdepth[w]--; else set_error(0x0504); }
void impl_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    float m[16] = {0};
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n)); m[15] = 1;
    mult(m);
}
void impl_glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    float m[16] = {0};
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b)); m[10] = (float)(-(f + n) / (f - n)); m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    mult(m);
}
void impl_glTranslatef(GLfloat x, GLfloat y, GLfloat z) { float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1}; mult(m); }
void impl_glTranslated(GLdouble x, GLdouble y, GLdouble z) { impl_glTranslatef((float)x, (float)y, (float)z); }
void impl_glScalef(GLfloat x, GLfloat y, GLfloat z) { float m[16] = {x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1}; mult(m); }
void impl_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len == 0) return;
    x /= len; y /= len; z /= len;
    float c = cosf(a * (float)M_PI / 180), s = sinf(a * (float)M_PI / 180), ic = 1 - c;
    float m[16] = {x * x * ic + c, y * x * ic + z * s, x * z * ic - y * s, 0,
                   x * y * ic - z * s, y * y * ic + c, y * z * ic + x * s, 0,
                   x * z * ic + y * s, y * z * ic - x * s, z * z * ic + c, 0, 0, 0, 0, 1};
    mult(m);
}

// ------------------------------------------------------------------ enables
static bool *cap_ptr(GLenum cap) {
    switch (cap) {
    case GL_CULL_FACE: return &C->cull;
    case GL_DEPTH_TEST: return &C->depth_test;
    case GL_BLEND: return &C->blend;
    case GL_ALPHA_TEST: return &C->alpha_test;
    case GL_SCISSOR_TEST: return &C->scissor_test;
    case GL_STENCIL_TEST: return &C->stencil_test;
    case GL_FOG: return &C->fog;
    case GL_CLIP_PLANE0: return &C->clip0;
    case GL_POLYGON_OFFSET_FILL: return &C->poly_offset_fill;
    case GL_POLYGON_OFFSET_LINE: return &C->poly_offset_line;
    case GL_TEXTURE_2D: return &C->tex2d[C->active_unit];
    case GL_LIGHTING: return &C->lighting;
    case GL_COLOR_MATERIAL: return &C->color_material;
    }
    return NULL;
}
int gl_tracing(void);
static void set_light_cap(GLenum cap, bool on) { if (cap >= 0x4000 && cap < 0x4008) C->light_on[cap - 0x4000] = on; }
void impl_glEnable(GLenum cap) { ctx_init(); set_light_cap(cap, true); if (gl_tracing()) fprintf(stderr, "  glEnable %04x\n", cap); bool *p = cap_ptr(cap); if (p) *p = true; }
void impl_glDisable(GLenum cap) { set_light_cap(cap, false); ctx_init(); if (gl_tracing()) fprintf(stderr, "  glDisable %04x\n", cap); bool *p = cap_ptr(cap); if (p) *p = false; }
GLboolean impl_glIsEnabled(GLenum cap) {
    bool *p = cap_ptr(cap);
    if (p) return *p;
    if (cap >= 0x4000 && cap < 0x4008) return C->light_on[cap - 0x4000];
    switch (cap) {
    case GL_VERTEX_ARRAY: return C->va.enabled;
    case GL_COLOR_ARRAY: return C->ca.enabled;
    case GL_NORMAL_ARRAY: return C->na.enabled;
    case GL_TEXTURE_COORD_ARRAY: return C->ta[C->client_unit].enabled;
    }
    return 0;
}
static GLArray *client_array(GLenum a) {
    switch (a) {
    case GL_VERTEX_ARRAY: return &C->va;
    case GL_COLOR_ARRAY: return &C->ca;
    case GL_NORMAL_ARRAY: return &C->na;
    case GL_TEXTURE_COORD_ARRAY: return &C->ta[C->client_unit];
    }
    return NULL;
}
void impl_glEnableClientState(GLenum a) { ctx_init(); GLArray *p = client_array(a); if (p) p->enabled = true; }
void impl_glDisableClientState(GLenum a) { ctx_init(); GLArray *p = client_array(a); if (p) p->enabled = false; }

// ------------------------------------------------------------------ simple state
void impl_glAlphaFunc(GLenum f, GLclampf ref) { C->alpha_func = f; C->alpha_ref = ref; }
void impl_glBlendFunc(GLenum s, GLenum d) { C->blend_src = s; C->blend_dst = d; }
void impl_glDepthFunc(GLenum f) { C->depth_func = f; }
void impl_glDepthMask(GLboolean f) { C->depth_mask = f != 0; }
void impl_glDepthRange(GLclampd n, GLclampd f) { if (gl_tracing()) fprintf(stderr, "  glDepthRange %g %g\n", n, f); C->depth_near = n; C->depth_far = f; }
void impl_glCullFace(GLenum m) { if (gl_tracing()) fprintf(stderr, "  glCullFace %04x\n", m); C->cull_face = m; }
void impl_glFrontFace(GLenum m) { if (gl_tracing()) fprintf(stderr, "  glFrontFace %04x\n", m); C->front_face = m; }
void impl_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    C->color_mask[0] = r; C->color_mask[1] = g; C->color_mask[2] = b; C->color_mask[3] = a;
}
void impl_glPolygonMode(GLenum face, GLenum mode) { (void)face; C->polygon_mode = mode; }
void impl_glPolygonOffset(GLfloat f, GLfloat u) { C->poly_factor = f; C->poly_units = u; }
void impl_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    ctx_init();
    C->viewport[0] = x; C->viewport[1] = y; C->viewport[2] = w; C->viewport[3] = h;
}
void impl_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { C->scissor[0] = x; C->scissor[1] = y; C->scissor[2] = w; C->scissor[3] = h; }
void impl_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    C->clear_color[0] = r; C->clear_color[1] = g; C->clear_color[2] = b; C->clear_color[3] = a;
}
void impl_glClearDepth(GLclampd d) { C->clear_depth = d; }
void impl_glClearStencil(GLint s) { C->clear_stencil = s; }
void impl_glStencilFunc(GLenum f, GLint ref, GLuint mask) { C->stencil_func = f; C->stencil_ref = ref; C->stencil_mask = mask; }
void impl_glStencilMask(GLuint m) { C->stencil_writemask = m; }
void impl_glStencilOp(GLenum f, GLenum zf, GLenum zp) { C->stencil_fail = f; C->stencil_zfail = zf; C->stencil_zpass = zp; }
void impl_glShadeModel(GLenum m) { (void)m; }
void impl_glHint(GLenum t, GLenum m) { (void)t; (void)m; }
void impl_glLineWidth(GLfloat w) { (void)w; }
void impl_glPointSize(GLfloat s) { (void)s; }
void impl_glDrawBuffer(GLenum m) { (void)m; }
void impl_glReadBuffer(GLenum m) { (void)m; }
void impl_glFlush(void) {}
void impl_glFinish(void) { mtl_finish(); }
void impl_glClear(GLbitfield mask) { ctx_init(); mtl_clear(mask); }
void impl_glPixelStorei(GLenum p, GLint v) {
    if (p == GL_UNPACK_ALIGNMENT) C->unpack_align = v;
    else if (p == GL_PACK_ALIGNMENT) C->pack_align = v;
    else if (p == GL_UNPACK_ROW_LENGTH) C->unpack_row_length = v;
}
void impl_glPixelStoref(GLenum p, GLfloat v) { impl_glPixelStorei(p, (GLint)v); }
void impl_glClipPlane(GLenum plane, const GLdouble *eq) {
    if (plane != GL_CLIP_PLANE0 || !eq) return;
    // plane is transformed to eye space by the inverse of the current modelview: p_eye = p * inverse(MV)
    const float *m = gl_top(0);
    double a = eq[0], b = eq[1], c = eq[2], d = eq[3];
    // invert affine modelview (rotation+translation, possibly scaled): use general 4x4 inverse
    double inv[16], M[16];
    for (int i = 0; i < 16; i++) M[i] = m[i];
    inv[0] = M[5]*M[10]*M[15] - M[5]*M[11]*M[14] - M[9]*M[6]*M[15] + M[9]*M[7]*M[14] + M[13]*M[6]*M[11] - M[13]*M[7]*M[10];
    inv[4] = -M[4]*M[10]*M[15] + M[4]*M[11]*M[14] + M[8]*M[6]*M[15] - M[8]*M[7]*M[14] - M[12]*M[6]*M[11] + M[12]*M[7]*M[10];
    inv[8] = M[4]*M[9]*M[15] - M[4]*M[11]*M[13] - M[8]*M[5]*M[15] + M[8]*M[7]*M[13] + M[12]*M[5]*M[11] - M[12]*M[7]*M[9];
    inv[12] = -M[4]*M[9]*M[14] + M[4]*M[10]*M[13] + M[8]*M[5]*M[14] - M[8]*M[6]*M[13] - M[12]*M[5]*M[10] + M[12]*M[6]*M[9];
    inv[1] = -M[1]*M[10]*M[15] + M[1]*M[11]*M[14] + M[9]*M[2]*M[15] - M[9]*M[3]*M[14] - M[13]*M[2]*M[11] + M[13]*M[3]*M[10];
    inv[5] = M[0]*M[10]*M[15] - M[0]*M[11]*M[14] - M[8]*M[2]*M[15] + M[8]*M[3]*M[14] + M[12]*M[2]*M[11] - M[12]*M[3]*M[10];
    inv[9] = -M[0]*M[9]*M[15] + M[0]*M[11]*M[13] + M[8]*M[1]*M[15] - M[8]*M[3]*M[13] - M[12]*M[1]*M[11] + M[12]*M[3]*M[9];
    inv[13] = M[0]*M[9]*M[14] - M[0]*M[10]*M[13] - M[8]*M[1]*M[14] + M[8]*M[2]*M[13] + M[12]*M[1]*M[10] - M[12]*M[2]*M[9];
    inv[2] = M[1]*M[6]*M[15] - M[1]*M[7]*M[14] - M[5]*M[2]*M[15] + M[5]*M[3]*M[14] + M[13]*M[2]*M[7] - M[13]*M[3]*M[6];
    inv[6] = -M[0]*M[6]*M[15] + M[0]*M[7]*M[14] + M[4]*M[2]*M[15] - M[4]*M[3]*M[14] - M[12]*M[2]*M[7] + M[12]*M[3]*M[6];
    inv[10] = M[0]*M[5]*M[15] - M[0]*M[7]*M[13] - M[4]*M[1]*M[15] + M[4]*M[3]*M[13] + M[12]*M[1]*M[7] - M[12]*M[3]*M[5];
    inv[14] = -M[0]*M[5]*M[14] + M[0]*M[6]*M[13] + M[4]*M[1]*M[14] - M[4]*M[2]*M[13] - M[12]*M[1]*M[6] + M[12]*M[2]*M[5];
    inv[3] = -M[1]*M[6]*M[11] + M[1]*M[7]*M[10] + M[5]*M[2]*M[11] - M[5]*M[3]*M[10] - M[9]*M[2]*M[7] + M[9]*M[3]*M[6];
    inv[7] = M[0]*M[6]*M[11] - M[0]*M[7]*M[10] - M[4]*M[2]*M[11] + M[4]*M[3]*M[10] + M[8]*M[2]*M[7] - M[8]*M[3]*M[6];
    inv[11] = -M[0]*M[5]*M[11] + M[0]*M[7]*M[9] + M[4]*M[1]*M[11] - M[4]*M[3]*M[9] - M[8]*M[1]*M[7] + M[8]*M[3]*M[5];
    inv[15] = M[0]*M[5]*M[10] - M[0]*M[6]*M[9] - M[4]*M[1]*M[10] + M[4]*M[2]*M[9] + M[8]*M[1]*M[6] - M[8]*M[2]*M[5];
    double det = M[0] * inv[0] + M[1] * inv[4] + M[2] * inv[8] + M[3] * inv[12];
    if (det == 0) return;
    for (int i = 0; i < 16; i++) inv[i] /= det;
    // row vector p times inv (column-major: element (row r, col k) = inv[k*4+r])
    double p[4] = {a, b, c, d};
    for (int k = 0; k < 4; k++) C->clip_plane[k] = (float)(p[0] * inv[k * 4 + 0] + p[1] * inv[k * 4 + 1] + p[2] * inv[k * 4 + 2] + p[3] * inv[k * 4 + 3]);
}

// fog
void impl_glFogf(GLenum p, GLfloat v) {
    switch (p) {
    case GL_FOG_MODE: C->fog_mode = (GLenum)v; break;
    case GL_FOG_DENSITY: C->fog_density = v; break;
    case GL_FOG_START: C->fog_start = v; break;
    case GL_FOG_END: C->fog_end = v; break;
    }
}
void impl_glFogi(GLenum p, GLint v) { if (p == GL_FOG_MODE) C->fog_mode = (GLenum)v; else impl_glFogf(p, (float)v); }
void impl_glFogfv(GLenum p, const GLfloat *v) {
    if (p == GL_FOG_COLOR) memcpy(C->fog_color, v, 16); else impl_glFogf(p, v[0]);
}
void impl_glFogiv(GLenum p, const GLint *v) {
    if (p == GL_FOG_COLOR) for (int i = 0; i < 4; i++) C->fog_color[i] = v[i] / 2147483647.f;
    else impl_glFogi(p, v[0]);
}

// ------------------------------------------------------------------ texture env
static void texenv(GLenum pname, const float *fv, GLint iv, bool is_vec) {
    GLTexEnv *e = &C->env[C->active_unit];
    GLenum v = (GLenum)iv;
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: e->mode = v; break;
    case GL_TEXTURE_ENV_COLOR: if (is_vec) memcpy(e->color, fv, 16); break;
    case GL_COMBINE_RGB_ARB: e->combine_rgb = v; break;
    case GL_COMBINE_ALPHA_ARB: e->combine_alpha = v; break;
    case GL_RGB_SCALE_ARB: e->rgb_scale = fv ? fv[0] : (float)iv; break;
    case GL_ALPHA_SCALE: e->alpha_scale = fv ? fv[0] : (float)iv; break;
    default:
        if (pname >= GL_SOURCE0_RGB_ARB && pname <= GL_SOURCE0_RGB_ARB + 2) e->src_rgb[pname - GL_SOURCE0_RGB_ARB] = v;
        else if (pname >= GL_SOURCE0_ALPHA_ARB && pname <= GL_SOURCE0_ALPHA_ARB + 2) e->src_alpha[pname - GL_SOURCE0_ALPHA_ARB] = v;
        else if (pname >= GL_OPERAND0_RGB_ARB && pname <= GL_OPERAND0_RGB_ARB + 2) e->op_rgb[pname - GL_OPERAND0_RGB_ARB] = v;
        else if (pname >= GL_OPERAND0_ALPHA_ARB && pname <= GL_OPERAND0_ALPHA_ARB + 2) e->op_alpha[pname - GL_OPERAND0_ALPHA_ARB] = v;
    }
}
void impl_glTexEnvi(GLenum t, GLenum p, GLint v) { (void)t; texenv(p, NULL, v, false); }
void impl_glTexEnvf(GLenum t, GLenum p, GLfloat v) { (void)t; float f = v; texenv(p, &f, (GLint)v, false); }
void impl_glTexEnvfv(GLenum t, GLenum p, const GLfloat *v) { (void)t; texenv(p, v, (GLint)v[0], true); }
void impl_glTexEnviv(GLenum t, GLenum p, const GLint *v) {
    (void)t;
    float f[4] = {v[0] / 2147483647.f, v[1] / 2147483647.f, v[2] / 2147483647.f, v[3] / 2147483647.f};
    if (p == GL_TEXTURE_ENV_COLOR) texenv(p, f, 0, true); else texenv(p, NULL, v[0], false);
}
void impl_glTexGeni(GLenum c, GLenum p, GLint v) { (void)c; (void)p; (void)v; }
void impl_glTexGenf(GLenum c, GLenum p, GLfloat v) { (void)c; (void)p; (void)v; }
void impl_glTexGenfv(GLenum c, GLenum p, const GLfloat *v) { (void)c; (void)p; (void)v; }

// ------------------------------------------------------------------ multitexture
void impl_glActiveTextureARB(GLenum u) { ctx_init(); int i = (int)(u - GL_TEXTURE0_ARB); if (i >= 0 && i < GL_MAX_UNITS) C->active_unit = i; }
void impl_glClientActiveTextureARB(GLenum u) { ctx_init(); int i = (int)(u - GL_TEXTURE0_ARB); if (i >= 0 && i < GL_MAX_UNITS) C->client_unit = i; }
void impl_glActiveTexture(GLenum u) { impl_glActiveTextureARB(u); }
void impl_glClientActiveTexture(GLenum u) { impl_glClientActiveTextureARB(u); }

// ------------------------------------------------------------------ textures
static GLTexture *tex_get(GLuint name, bool create) {
    if (name >= GL_MAX_TEXTURES) return NULL;
    if (!C->tex[name] && create) {
        GLTexture *t = calloc(1, sizeof *t);
        t->min_filter = GL_NEAREST_MIPMAP_LINEAR; t->mag_filter = GL_LINEAR;
        t->wrap_s = t->wrap_t = GL_REPEAT; t->aniso = 1; t->max_level_loaded = -1;
        C->tex[name] = t;
    }
    return C->tex[name];
}
GLTexture *gl_bound_texture(int unit) { return tex_get(C->bound[unit], true); }
void impl_glBindTexture(GLenum target, GLuint name) {
    ctx_init();
    if (target != GL_TEXTURE_2D) { if (getenv("COD_GLDEBUG")) LOG("GLDEBUG: bind target %04x tex %u", target, name); return; }
    C->bound[C->active_unit] = name;
    tex_get(name, true);
}
void impl_glGenTextures(GLsizei n, GLuint *out) {
    static GLuint next = 40000;   // engine assigns its own small texnums; generated names stay out of the way
    for (int i = 0; i < n; i++) { while (next < GL_MAX_TEXTURES && C->tex[next]) next++; out[i] = next++; }
}
void impl_glDeleteTextures(GLsizei n, const GLuint *names) {
    for (int i = 0; i < n; i++) {
        GLuint k = names[i];
        if (k && k < GL_MAX_TEXTURES && C->tex[k]) {
            mtl_texture_release(C->tex[k]);
            free(C->tex[k]);
            C->tex[k] = NULL;
        }
    }
}
GLboolean impl_glIsTexture(GLuint n) { return n < GL_MAX_TEXTURES && C->tex[n] && C->tex[n]->mtl; }
void impl_glPrioritizeTextures(GLsizei n, const GLuint *t, const GLclampf *p) { (void)n; (void)t; (void)p; }
GLboolean impl_glAreTexturesResident(GLsizei n, const GLuint *t, GLboolean *r) { for (int i = 0; i < n; i++) r[i] = 1; (void)t; return 1; }

static void tex_param(GLenum p, float v) {
    GLTexture *t = gl_bound_texture(C->active_unit);
    if (!t) return;
    switch (p) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = (GLenum)v; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = (GLenum)v; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = (GLenum)v; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = (GLenum)v; break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: t->aniso = v < 1 ? 1 : v > 16 ? 16 : v; break;
    default: return;
    }
    t->sampler = NULL;   // backend rebuilds/looks up a cached sampler
}
void impl_glTexParameteri(GLenum target, GLenum p, GLint v) { (void)target; tex_param(p, (float)v); }
void impl_glTexParameterf(GLenum target, GLenum p, GLfloat v) {
    (void)target;
    // enum-valued params passed as float (Quake does this): exact integer conversion
    tex_param(p, v);
}
void impl_glTexParameterfv(GLenum target, GLenum p, const GLfloat *v) { (void)target; if (p != GL_TEXTURE_BORDER_COLOR) tex_param(p, v[0]); }
void impl_glTexParameteriv(GLenum target, GLenum p, const GLint *v) { (void)target; if (p != GL_TEXTURE_BORDER_COLOR) tex_param(p, (float)v[0]); }

typedef enum { BASE_RGBA, BASE_RGB, BASE_L, BASE_LA, BASE_A, BASE_I } BaseFmt;
static BaseFmt base_of_internal(GLint ifmt) {
    switch (ifmt) {
    case 1: case GL_LUMINANCE: return BASE_L;
    case 2: case GL_LUMINANCE_ALPHA: return BASE_LA;
    case 3: case GL_RGB: case 0x84ED: return BASE_RGB;                 // GL_COMPRESSED_RGB_ARB
    case 4: case GL_RGBA: case 0x84EE: return BASE_RGBA;               // GL_COMPRESSED_RGBA_ARB
    case GL_ALPHA: return BASE_A;
    case GL_INTENSITY: return BASE_I;
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: return BASE_RGB;
    }
    if (ifmt >= 0x803B && ifmt <= 0x803E) return BASE_A;       // ALPHA4..16
    if (ifmt >= 0x803F && ifmt <= 0x8042) return BASE_L;       // LUMINANCE4..16
    if (ifmt >= 0x8043 && ifmt <= 0x8048) return BASE_LA;
    if (ifmt >= 0x8049 && ifmt <= 0x804D) return BASE_I;
    if (ifmt >= 0x804F && ifmt <= 0x8054) return BASE_RGB;     // RGB4..RGB16, R3_G3_B2(0x2A10 handled below)
    if (ifmt >= 0x8055 && ifmt <= 0x805B) return BASE_RGBA;    // RGBA2..RGBA16, RGB5_A1, RGB10_A2
    if (ifmt == 0x2A10) return BASE_RGB;
    return BASE_RGBA;
}
static int comps_of(GLenum fmt) {
    switch (fmt) {
    case GL_RGBA: case GL_BGRA_EXT: return 4;
    case GL_RGB: case GL_BGR_EXT: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}
// Convert client pixels (format/type) to RGBA8 applying the internal base format.
static uint8_t *to_rgba8(int w, int h, GLenum fmt, GLenum type, const uint8_t *src, BaseFmt base) {
    if (type != GL_UNSIGNED_BYTE) { LOG("GL: texture type %04x unsupported", type); return NULL; }
    int nc = comps_of(fmt);
    int row_px = C->unpack_row_length > 0 ? C->unpack_row_length : w;
    size_t stride = (size_t)row_px * nc;
    int al = C->unpack_align;
    if (al > 1) stride = (stride + al - 1) / al * al;
    uint8_t *out = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + y * stride;
        uint8_t *d = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, s += nc, d += 4) {
            uint8_t r, g, b, a;
            switch (fmt) {
            case GL_RGBA: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
            case GL_BGRA_EXT: r = s[2]; g = s[1]; b = s[0]; a = s[3]; break;
            case GL_RGB: r = s[0]; g = s[1]; b = s[2]; a = 255; break;
            case GL_BGR_EXT: r = s[2]; g = s[1]; b = s[0]; a = 255; break;
            case GL_LUMINANCE_ALPHA: r = g = b = s[0]; a = s[1]; break;
            case GL_ALPHA: r = g = b = 255; a = s[0]; break;
            case GL_INTENSITY: r = g = b = a = s[0]; break;
            default: r = g = b = s[0]; a = 255; break;      // GL_LUMINANCE, GL_RED...
            }
            switch (base) {
            case BASE_RGB: a = 255; break;
            case BASE_L: g = b = r; a = 255; break;
            case BASE_LA: g = b = r; break;
            case BASE_A: r = g = b = 255; break;
            case BASE_I: g = b = a = r; break;
            default: break;
            }
            d[0] = r; d[1] = g; d[2] = b; d[3] = a;
        }
    }
    return out;
}
static int full_levels(int w, int h) { int n = 1; while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }

static void ensure_storage(GLTexture *t, int level, int w, int h, int kind, GLenum ifmt) {
    // derive level-0 size from this level
    int w0 = w << level, h0 = h << level;
    if (level == 0) { w0 = w; h0 = h; }
    int k = t->compressed ? (int)(t->ifmt - GL_COMPRESSED_RGB_S3TC_DXT1_EXT) : -1;
    int cur_kind = !t->compressed ? TEXK_RGBA8 : k <= 1 ? TEXK_BC1 : k == 2 ? TEXK_BC2 : TEXK_BC3;
    if (t->mtl && level == 0 && (t->w != w0 || t->h != h0 || cur_kind != kind)) { mtl_texture_release(t); }
    if (!t->mtl) {
        if (level != 0 && !t->w) { w0 = w << level; h0 = h << level; }
        t->w = w0; t->h = h0;
        t->levels = full_levels(w0, h0);
        t->max_level_loaded = -1;
        mtl_texture_storage(t, w0, h0, t->levels, kind);
    }
    if (level == 0) t->ifmt = ifmt;
}

void impl_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const GLvoid *pixels) {
    ctx_init();
    (void)border;
    if (target != GL_TEXTURE_2D || w <= 0 || h <= 0) { if (getenv("COD_GLDEBUG")) LOG("GLDEBUG: teximage target %04x", target); return; }
    GLTexture *t = gl_bound_texture(C->active_unit);
    if (level == 0) { t->compressed = false; }
    ensure_storage(t, level, w, h, TEXK_RGBA8, (GLenum)ifmt);
    BaseFmt base = base_of_internal(ifmt);
    if (level == 0) t->opaque = base == BASE_RGB || base == BASE_L;
    if (level >= t->levels) return;
    if (pixels) {
        uint8_t *rgba = to_rgba8(w, h, fmt, type, pixels, base);
        if (rgba) { mtl_texture_upload(t, level, 0, 0, w, h, rgba, (size_t)w * 4, (size_t)w * h * 4); free(rgba); }
    }
    if (level > t->max_level_loaded) t->max_level_loaded = level;
    t->sampler = NULL;
}
void impl_glTexImage1D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLint border, GLenum fmt, GLenum type, const GLvoid *px) {
    (void)target;
    impl_glTexImage2D(GL_TEXTURE_2D, level, ifmt, w, 1, border, fmt, type, px);
}
void impl_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const GLvoid *pixels) {
    if (target != GL_TEXTURE_2D || !pixels || w <= 0 || h <= 0) return;
    GLTexture *t = gl_bound_texture(C->active_unit);
    if (!t->mtl || t->compressed) return;
    uint8_t *rgba = to_rgba8(w, h, fmt, type, pixels, base_of_internal((GLint)t->ifmt));
    if (rgba) { mtl_texture_upload(t, level, x, y, w, h, rgba, (size_t)w * 4, (size_t)w * h * 4); free(rgba); }
}
void impl_glCompressedTexImage2DARB(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const GLvoid *data) {
    ctx_init();
    (void)border;
    if (target != GL_TEXTURE_2D || w <= 0 || h <= 0) return;
    int kind;
    switch (ifmt) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: kind = TEXK_BC1; break;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: kind = TEXK_BC2; break;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: kind = TEXK_BC3; break;
    default: LOG("GL: compressed format %04x unsupported", ifmt); return;
    }
    GLTexture *t = gl_bound_texture(C->active_unit);
    if (level == 0) { t->compressed = true; t->ifmt = ifmt; }
    ensure_storage(t, level, w, h, kind, ifmt);
    if (level == 0) t->opaque = ifmt == GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    if (level >= t->levels || !data) return;
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t bpb = kind == TEXK_BC1 ? 8 : 16;
    if ((size_t)size < (size_t)bw * bh * bpb) return;
    mtl_texture_upload(t, level, 0, 0, w, h, data, bw * bpb, (size_t)bw * bh * bpb);
    if (level > t->max_level_loaded) t->max_level_loaded = level;
    t->sampler = NULL;
}
void impl_glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const GLvoid *data) {
    impl_glCompressedTexImage2DARB(target, level, ifmt, w, h, border, size, data);
}
void impl_glCopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    if (target != GL_TEXTURE_2D) return;
    GLTexture *t = gl_bound_texture(C->active_unit);
    if (!t->mtl) return;
    mtl_copy_framebuffer_to_texture(t, level, xo, yo, x, y, w, h);
}
void impl_glCopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w, GLsizei h, GLint border) {
    (void)border;
    if (target != GL_TEXTURE_2D) return;
    GLTexture *t = gl_bound_texture(C->active_unit);
    t->compressed = false;
    ensure_storage(t, (int)level, w, h, TEXK_RGBA8, ifmt);
    t->opaque = base_of_internal((GLint)ifmt) == BASE_RGB;
    if ((int)level > t->max_level_loaded) t->max_level_loaded = (int)level;
    mtl_copy_framebuffer_to_texture(t, (int)level, 0, 0, x, y, w, h);
}
void impl_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, GLvoid *out) {
    if (!out || w <= 0 || h <= 0 || type != GL_UNSIGNED_BYTE) return;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    mtl_read_pixels(x, y, w, h, rgba);
    int nc = fmt == GL_RGBA || fmt == GL_BGRA_EXT ? 4 : fmt == GL_RGB || fmt == GL_BGR_EXT ? 3 : 1;
    size_t stride = (size_t)w * nc;
    if (C->pack_align > 1) stride = (stride + C->pack_align - 1) / C->pack_align * C->pack_align;
    for (int r = 0; r < h; r++) {
        uint8_t *d = (uint8_t *)out + r * stride;
        const uint8_t *s = rgba + (size_t)r * w * 4;
        for (int i = 0; i < w; i++, s += 4) {
            switch (fmt) {
            case GL_RGBA: memcpy(d, s, 4); d += 4; break;
            case GL_BGRA_EXT: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; d += 4; break;
            case GL_RGB: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; break;
            case GL_BGR_EXT: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d += 3; break;
            case GL_ALPHA: *d++ = s[3]; break;
            default: *d++ = s[0]; break;
            }
        }
    }
    free(rgba);
}

// ------------------------------------------------------------------ queries
static const char *k_extensions =
    "GL_ARB_multitexture GL_ARB_texture_env_combine GL_EXT_texture_env_combine GL_ARB_texture_env_add "
    "GL_EXT_texture_env_add GL_EXT_compiled_vertex_array GL_ARB_texture_compression "
    "GL_EXT_texture_compression_s3tc GL_S3_s3tc GL_EXT_texture_filter_anisotropic GL_EXT_texture_edge_clamp "
    "GL_SGIS_texture_edge_clamp GL_EXT_bgra GL_EXT_packed_pixels ";
uint32_t impl_glGetString(GLenum name) {
    ctx_init();
    static uint32_t s[4];
    int i = name - GL_VENDOR;
    if (i < 0 || i > 3) { set_error(GL_INVALID_ENUM); return 0; }
    if (!s[i]) {
        const char *v[4] = {"Apple", "Apple Silicon (Metal)", "1.3.0 cod1native", k_extensions};
        s[i] = gstrdup(v[i]);
    }
    return s[i];
}
GLenum impl_glGetError(void) { GLenum e = C->error; C->error = 0; return e; }
static int get_int(GLenum p, GLint *out) {
    switch (p) {
    case GL_MAX_TEXTURE_SIZE: out[0] = 4096; return 1;
    case GL_MAX_TEXTURE_UNITS_ARB: out[0] = GL_MAX_UNITS; return 1;
    case GL_VIEWPORT: memcpy(out, C->viewport, 16); return 4;
    case GL_SCISSOR_BOX: memcpy(out, C->scissor, 16); return 4;
    case GL_DEPTH_BITS: out[0] = 24; return 1;
    case GL_STENCIL_BITS: out[0] = 8; return 1;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: out[0] = 8; return 1;
    case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: out[0] = 65536; return 1;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: out[0] = 4; return 1;
    case GL_COMPRESSED_TEXTURE_FORMATS: for (int i = 0; i < 4; i++) out[i] = GL_COMPRESSED_RGB_S3TC_DXT1_EXT + i; return 4;
    case 0x0D32: out[0] = GL_MATRIX_DEPTH; return 1;  // GL_MAX_MODELVIEW_STACK_DEPTH
    case 0x0D31: out[0] = GL_MATRIX_DEPTH; return 1;  // GL_MAX_CLIP_PLANES (enough)
    case 0x0B70: out[0] = 1; return 1;
    }
    out[0] = 0;
    return 1;
}
void impl_glGetIntegerv(GLenum p, GLint *out) { ctx_init(); get_int(p, out); }
void impl_glGetFloatv(GLenum p, GLfloat *out) {
    ctx_init();
    switch (p) {
    case GL_MODELVIEW_MATRIX: memcpy(out, gl_top(0), 64); return;
    case GL_PROJECTION_MATRIX: memcpy(out, gl_top(1), 64); return;
    case GL_TEXTURE_MATRIX: memcpy(out, gl_top(2 + C->active_unit), 64); return;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: out[0] = 16; return;
    }
    GLint v[16];
    int n = get_int(p, v);
    for (int i = 0; i < n; i++) out[i] = (float)v[i];
}
void impl_glGetDoublev(GLenum p, GLdouble *out) { float f[16]; impl_glGetFloatv(p, f); for (int i = 0; i < 16; i++) out[i] = f[i]; }
void impl_glGetBooleanv(GLenum p, GLboolean *out) { GLint v[16]; int n = get_int(p, v); for (int i = 0; i < n; i++) out[i] = v[i] != 0; }

// ------------------------------------------------------------------ vertex fetch & primitive assembly
static inline float fetch1(const uint8_t *p, GLenum type, int i) {
    switch (type) {
    case GL_FLOAT: { float f; memcpy(&f, p + 4 * i, 4); return f; }
    case GL_DOUBLE: { double d; memcpy(&d, p + 8 * i, 8); return (float)d; }
    case GL_SHORT: { int16_t s; memcpy(&s, p + 2 * i, 2); return s; }
    case GL_INT: { int32_t s; memcpy(&s, p + 4 * i, 4); return (float)s; }
    case GL_UNSIGNED_BYTE: return p[i];
    case GL_BYTE: return (int8_t)p[i];
    case GL_UNSIGNED_SHORT: { uint16_t s; memcpy(&s, p + 2 * i, 2); return s; }
    }
    return 0;
}
static int type_size(GLenum t) {
    switch (t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; case GL_DOUBLE: return 8; }
    return 4;
}
static inline const uint8_t *elem(const GLArray *a, uint32_t i) {
    int st = a->stride ? a->stride : a->size * type_size(a->type);
    return a->ptr + (size_t)st * i;
}
static void fetch_vertex(uint32_t i, GLVtx *v) {
    GLContext *c = C;
    if (c->va.enabled && c->va.ptr) {
        const uint8_t *p = elem(&c->va, i);
        v->pos[0] = fetch1(p, c->va.type, 0); v->pos[1] = fetch1(p, c->va.type, 1);
        v->pos[2] = c->va.size > 2 ? fetch1(p, c->va.type, 2) : 0.f;
        v->pos[3] = c->va.size > 3 ? fetch1(p, c->va.type, 3) : 1.f;
    } else { v->pos[0] = v->pos[1] = v->pos[2] = 0; v->pos[3] = 1; }
    if (c->ca.enabled && c->ca.ptr) {
        const uint8_t *p = elem(&c->ca, i);
        if (c->ca.type == GL_UNSIGNED_BYTE) {
            v->col[0] = p[0]; v->col[1] = p[1]; v->col[2] = p[2]; v->col[3] = c->ca.size > 3 ? p[3] : 255;
        } else {
            for (int k = 0; k < 4; k++) {
                float f = k < c->ca.size ? fetch1(p, c->ca.type, k) : 1.f;
                f = f < 0 ? 0 : f > 1 ? 1 : f;
                v->col[k] = (uint8_t)(f * 255.f + 0.5f);
            }
        }
    } else {
        for (int k = 0; k < 4; k++) { float f = c->cur_color[k]; f = f < 0 ? 0 : f > 1 ? 1 : f; v->col[k] = (uint8_t)(f * 255.f + 0.5f); }
    }
    if (c->na.enabled && c->na.ptr) {
        const uint8_t *p = elem(&c->na, i);
        float sc = c->na.type == GL_BYTE ? 1.f / 127.f : c->na.type == GL_SHORT ? 1.f / 32767.f : 1.f;
        for (int k = 0; k < 3; k++) v->nrm[k] = fetch1(p, c->na.type, k) * sc;
    } else memcpy(v->nrm, c->cur_normal, 12);
    for (int u = 0; u < GL_MAX_UNITS; u++) {
        const GLArray *a = &c->ta[u];
        if (a->enabled && a->ptr) {
            const uint8_t *p = elem(a, i);
            v->tc[u][0] = fetch1(p, a->type, 0); v->tc[u][1] = a->size > 1 ? fetch1(p, a->type, 1) : 0.f;
        } else { v->tc[u][0] = c->cur_tc[u][0]; v->tc[u][1] = c->cur_tc[u][1]; }
    }
}

// index list builder for a GL primitive over n vertices numbered base..base+n-1 (sequential) or explicit list
static uint32_t *g_idx; static size_t g_idx_cap;
static uint32_t *idx_reserve(size_t n) {
    if (n > g_idx_cap) { g_idx_cap = n * 2 + 1024; g_idx = realloc(g_idx, g_idx_cap * 4); }
    return g_idx;
}
// in: vertex numbers `src` (count n, may be NULL meaning 0..n-1). Out: indices into the same space, prim kind.
static uint32_t assemble(GLenum mode, const uint32_t *src, uint32_t n, int *prim) {
#define S(k) (src ? src[k] : (uint32_t)(k))
    uint32_t m = 0, *o;
    bool lines = C->polygon_mode == GL_LINE;
    switch (mode) {
    case GL_POINTS:
        o = idx_reserve(n); for (uint32_t i = 0; i < n; i++) o[m++] = S(i); *prim = PRIM_POINTS; return m;
    case GL_LINES:
        o = idx_reserve(n); for (uint32_t i = 0; i + 1 < n; i += 2) { o[m++] = S(i); o[m++] = S(i + 1); } *prim = PRIM_LINES; return m;
    case GL_LINE_STRIP: case GL_LINE_LOOP:
        o = idx_reserve(2 * n + 2);
        for (uint32_t i = 0; i + 1 < n; i++) { o[m++] = S(i); o[m++] = S(i + 1); }
        if (mode == GL_LINE_LOOP && n > 2) { o[m++] = S(n - 1); o[m++] = S(0); }
        *prim = PRIM_LINES; return m;
    }
    o = idx_reserve(3 * n + 6);
    switch (mode) {
    case GL_TRIANGLES: for (uint32_t i = 0; i + 2 < n; i += 3) { o[m++] = S(i); o[m++] = S(i + 1); o[m++] = S(i + 2); } break;
    case GL_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) { o[m++] = S(i + 1); o[m++] = S(i); o[m++] = S(i + 2); }
            else { o[m++] = S(i); o[m++] = S(i + 1); o[m++] = S(i + 2); }
        }
        break;
    case GL_TRIANGLE_FAN: case GL_POLYGON:
        for (uint32_t i = 1; i + 1 < n; i++) { o[m++] = S(0); o[m++] = S(i); o[m++] = S(i + 1); }
        break;
    case GL_QUADS:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            o[m++] = S(i); o[m++] = S(i + 1); o[m++] = S(i + 2);
            o[m++] = S(i); o[m++] = S(i + 2); o[m++] = S(i + 3);
        }
        break;
    case GL_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            o[m++] = S(i); o[m++] = S(i + 1); o[m++] = S(i + 3);
            o[m++] = S(i); o[m++] = S(i + 3); o[m++] = S(i + 2);
        }
        break;
    default: *prim = PRIM_TRIANGLES; return 0;
    }
#undef S
    *prim = PRIM_TRIANGLES;
    if (lines && m) {   // wireframe: triangle edges
        uint32_t t = m;
        uint32_t *tri = malloc(t * 4);
        memcpy(tri, o, t * 4);
        o = idx_reserve(2 * t);
        m = 0;
        for (uint32_t i = 0; i + 2 < t; i += 3) {
            o[m++] = tri[i]; o[m++] = tri[i + 1]; o[m++] = tri[i + 1]; o[m++] = tri[i + 2]; o[m++] = tri[i + 2]; o[m++] = tri[i];
        }
        free(tri);
        *prim = PRIM_LINES;
    }
    return m;
}

// ------------------------------------------------------------------ widescreen HUD pillarbox
// The engine draws all 2D (HUD, menus, console, loading screens, cinematics) on a 640x480 virtual canvas scaled to the
// full mode, so on a wide mode it stretches. A 2D draw is recognised by its state (identity modelview, projection =
// glOrtho(0,W,H,0) over the whole W x H mode viewport) and its x coordinates are squeezed into a centred 4:3 column.
//  - frames without a 3D scene (menus, loading, cinematics): everything is pillarboxed; the side bars are cleared to
//    black at swap.
//  - frames with a 3D scene (in game): wide axis-aligned quads touching the left/right screen edge keep their edge
//    vertices there, so full-screen fades, damage flashes and edge masks (scopes) still cover the whole width.
// COD_HUD_STRETCH=1 disables it.
int plat_mode_w(void);
int plat_mode_h(void);
static bool g_frame_3d, g_frame_boxed;
static bool hud_is_2d(float *scale, float *bias, float *W) {
    static int off = -1;
    if (off < 0) off = getenv("COD_HUD_STRETCH") && atoi(getenv("COD_HUD_STRETCH"));
    if (off) return false;
    int w = plat_mode_w(), h = plat_mode_h();
    if (w * 3 <= h * 4 + 3) return false;   // 4:3 or narrower
    const GLContext *c = C;
    const float *mv = gl_top(0), *p = gl_top(1);
    if (p[11] != 0.f) { g_frame_3d = true; return false; }   // perspective projection: the world / a 3D view
    if (c->viewport[0] != 0 || c->viewport[1] != 0 || c->viewport[2] != w || c->viewport[3] != h) return false;
    for (int i = 0; i < 16; i++) if (fabsf(mv[i] - ((i % 5 == 0) ? 1.f : 0.f)) > 1e-5f) return false;
    if (fabsf(p[0] * w - 2.f) > 1e-3f || fabsf(p[5] * h + 2.f) > 1e-3f || fabsf(p[12] + 1.f) > 1e-4f ||
        fabsf(p[13] - 1.f) > 1e-4f || p[1] != 0 || p[4] != 0 || p[3] != 0 || p[7] != 0 || p[15] != 1.f)
        return false;
    float cw = h * 4.f / 3.f;
    *scale = cw / w; *bias = (w - cw) * 0.5f; *W = (float)w;
    return true;
}
static void hud_squeeze(GLVtx *v, uint32_t n) {
    float s, b, W;
    if (!hud_is_2d(&s, &b, &W)) return;
    bool anchor = g_frame_3d && n % 4 == 0;
    if (!g_frame_3d) g_frame_boxed = true;
    for (uint32_t q = 0; q < n; q += 4) {
        uint32_t cnt = anchor ? 4 : n;
        bool keep_edges = false;
        if (anchor) {
            // only whole axis-aligned quads that are wide and touch a screen edge (fades, flashes, scope masks) keep
            // their edge vertices; rotated or small quads (compass, markers) are always squeezed
            float lo = v[q].pos[0], hi = lo;
            for (uint32_t k = 1; k < 4; k++) { float x = v[q + k].pos[0]; if (x < lo) lo = x; if (x > hi) hi = x; }
            bool aligned = true;
            for (uint32_t k = 0; k < 4; k++) {
                float x = v[q + k].pos[0];
                if (fabsf(x - lo) > 0.01f && fabsf(x - hi) > 0.01f) aligned = false;
            }
            keep_edges = aligned && hi - lo >= 0.1f * W && (fabsf(lo) <= 0.5f || fabsf(hi - W) <= 0.5f);
        }
        for (uint32_t i = q; i < q + cnt; i++) {
            float x = v[i].pos[0];
            if (keep_edges && (fabsf(x) <= 0.5f || fabsf(x - W) <= 0.5f)) continue;
            v[i].pos[0] = b + x * s;
        }
        if (!anchor) break;
    }
}
// at swap: black side bars for pillarboxed 2D-only frames (nothing else draws there), then reset per-frame state
static void hud_end_frame(void) {
    if (g_frame_boxed && !g_frame_3d) {
        GLContext *c = C;
        int w = plat_mode_w(), h = plat_mode_h();
        float cw = h * 4.f / 3.f, bias = (w - cw) * 0.5f;
        int l = (int)floorf(bias), r = (int)ceilf(bias + cw);   // column = [bias, bias + cw)
        bool st = c->scissor_test, cm[4];
        GLint sc[4]; GLfloat cc[4];
        memcpy(sc, c->scissor, sizeof sc); memcpy(cc, c->clear_color, sizeof cc); memcpy(cm, c->color_mask, sizeof cm);
        c->scissor_test = true;
        for (int i = 0; i < 4; i++) { c->clear_color[i] = i == 3 ? 1.f : 0.f; c->color_mask[i] = true; }
        c->scissor[1] = 0; c->scissor[3] = h;
        if (l > 0) { c->scissor[0] = 0; c->scissor[2] = l; mtl_clear(GL_COLOR_BUFFER_BIT); }
        if (r < w) { c->scissor[0] = r; c->scissor[2] = w - r; mtl_clear(GL_COLOR_BUFFER_BIT); }
        c->scissor_test = st;
        memcpy(c->scissor, sc, sizeof sc); memcpy(c->clear_color, cc, sizeof cc); memcpy(c->color_mask, cm, sizeof cm);
    }
    g_frame_3d = g_frame_boxed = false;
}

static GLVtx *g_vtx; static size_t g_vtx_cap;
static GLVtx *vtx_reserve(size_t n) {
    if (n > g_vtx_cap) { g_vtx_cap = n * 2 + 1024; g_vtx = realloc(g_vtx, g_vtx_cap * sizeof(GLVtx)); }
    return g_vtx;
}

// draw from client arrays: vertices [lo, hi] referenced through `elements` (or sequential first..first+count)
static void draw_arrays_range(GLenum mode, uint32_t lo, uint32_t hi, const uint32_t *elements, uint32_t count) {
    if (hi < lo || count == 0) return;
    uint32_t nv = hi - lo + 1;
    GLVtx *v = vtx_reserve(nv);
    for (uint32_t i = 0; i < nv; i++) fetch_vertex(lo + i, &v[i]);
    // rebase element numbers to the fetched range
    uint32_t *rebased = NULL;
    if (elements) {
        rebased = malloc(count * 4);
        for (uint32_t i = 0; i < count; i++) rebased[i] = elements[i] - lo;
    }
    int prim;
    uint32_t ni = assemble(mode, rebased, elements ? count : nv, &prim);
    free(rebased);
    hud_squeeze(v, nv);
    if (ni) mtl_draw(prim, v, nv, g_idx, ni);
}

void impl_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { ctx_init(); C->va = (GLArray){C->va.enabled, size, type, stride, p}; }
void impl_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { ctx_init(); C->ca = (GLArray){C->ca.enabled, size, type, stride, p}; }
void impl_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p) { ctx_init(); C->na = (GLArray){C->na.enabled, 3, type, stride, p}; }
void impl_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) {
    ctx_init();
    GLArray *a = &C->ta[C->client_unit];
    *a = (GLArray){a->enabled, size, type, stride, p};
}
void impl_glLockArraysEXT(GLint first, GLsizei count) { C->lock_first = first; C->lock_count = count; }
void impl_glUnlockArraysEXT(void) { C->lock_count = 0; }

void impl_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    ctx_init();
    if (count <= 0) return;
    draw_arrays_range(mode, (uint32_t)first, (uint32_t)(first + count - 1), NULL, (uint32_t)count);
}
static void draw_elements(GLenum mode, GLsizei count, GLenum type, const GLvoid *ind, int lo_hint, int hi_hint) {
    ctx_init();
    if (count <= 0 || !ind) return;
    uint32_t *el = malloc((size_t)count * 4);
    uint32_t lo = 0xffffffffu, hi = 0;
    for (int i = 0; i < count; i++) {
        uint32_t e;
        if (type == GL_UNSIGNED_INT) memcpy(&e, (const uint8_t *)ind + 4 * i, 4);
        else if (type == GL_UNSIGNED_SHORT) { uint16_t s; memcpy(&s, (const uint8_t *)ind + 2 * i, 2); e = s; }
        else e = ((const uint8_t *)ind)[i];
        el[i] = e;
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    (void)lo_hint; (void)hi_hint;
    draw_arrays_range(mode, lo, hi, el, (uint32_t)count);
    free(el);
}
void impl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *ind) { draw_elements(mode, count, type, ind, -1, -1); }
void impl_glDrawRangeElements(GLenum mode, GLuint s, GLuint e, GLsizei count, GLenum type, const GLvoid *ind) { draw_elements(mode, count, type, ind, (int)s, (int)e); }
void impl_glDrawRangeElementsEXT(GLenum mode, GLuint s, GLuint e, GLsizei count, GLenum type, const GLvoid *ind) { draw_elements(mode, count, type, ind, (int)s, (int)e); }

// ------------------------------------------------------------------ immediate mode
static GLVtx *g_imm; static size_t g_imm_n, g_imm_cap;
void impl_glBegin(GLenum mode) { ctx_init(); C->in_begin = true; C->begin_mode = mode; g_imm_n = 0; }
static void emit(float x, float y, float z, float w) {
    if (g_imm_n >= g_imm_cap) { g_imm_cap = g_imm_cap * 2 + 256; g_imm = realloc(g_imm, g_imm_cap * sizeof(GLVtx)); }
    GLVtx *v = &g_imm[g_imm_n++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = w;
    for (int k = 0; k < 4; k++) { float f = C->cur_color[k]; f = f < 0 ? 0 : f > 1 ? 1 : f; v->col[k] = (uint8_t)(f * 255.f + 0.5f); }
    for (int u = 0; u < GL_MAX_UNITS; u++) { v->tc[u][0] = C->cur_tc[u][0]; v->tc[u][1] = C->cur_tc[u][1]; }
    memcpy(v->nrm, C->cur_normal, 12);
}
void impl_glEnd(void) {
    C->in_begin = false;
    int prim;
    uint32_t ni = assemble(C->begin_mode, NULL, (uint32_t)g_imm_n, &prim);
    hud_squeeze(g_imm, (uint32_t)g_imm_n);
    if (ni) mtl_draw(prim, g_imm, (uint32_t)g_imm_n, g_idx, ni);
}
void impl_glVertex2f(GLfloat x, GLfloat y) { emit(x, y, 0, 1); }
void impl_glVertex2i(GLint x, GLint y) { emit((float)x, (float)y, 0, 1); }
void impl_glVertex2fv(const GLfloat *v) { emit(v[0], v[1], 0, 1); }
void impl_glVertex2d(GLdouble x, GLdouble y) { emit((float)x, (float)y, 0, 1); }
void impl_glVertex3f(GLfloat x, GLfloat y, GLfloat z) { emit(x, y, z, 1); }
void impl_glVertex3fv(const GLfloat *v) { emit(v[0], v[1], v[2], 1); }
void impl_glVertex3d(GLdouble x, GLdouble y, GLdouble z) { emit((float)x, (float)y, (float)z, 1); }
void impl_glVertex3dv(const GLdouble *v) { emit((float)v[0], (float)v[1], (float)v[2], 1); }
void impl_glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { emit(x, y, z, w); }
void impl_glVertex4fv(const GLfloat *v) { emit(v[0], v[1], v[2], v[3]); }
void impl_glArrayElement(GLint i) {
    GLVtx v;
    fetch_vertex((uint32_t)i, &v);
    if (g_imm_n >= g_imm_cap) { g_imm_cap = g_imm_cap * 2 + 256; g_imm = realloc(g_imm, g_imm_cap * sizeof(GLVtx)); }
    g_imm[g_imm_n++] = v;
}
static void set_color(float r, float g, float b, float a) { C->cur_color[0] = r; C->cur_color[1] = g; C->cur_color[2] = b; C->cur_color[3] = a; }
void impl_glColor3f(GLfloat r, GLfloat g, GLfloat b) { set_color(r, g, b, 1); }
void impl_glColor3fv(const GLfloat *v) { set_color(v[0], v[1], v[2], 1); }
void impl_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { set_color(r, g, b, a); }
void impl_glColor4fv(const GLfloat *v) { set_color(v[0], v[1], v[2], v[3]); }
void impl_glColor3ub(GLubyte r, GLubyte g, GLubyte b) { set_color(r / 255.f, g / 255.f, b / 255.f, 1); }
void impl_glColor3ubv(const GLubyte *v) { set_color(v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, 1); }
void impl_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { set_color(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }
void impl_glColor4ubv(const GLubyte *v) { set_color(v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, v[3] / 255.f); }
void impl_glColor3d(GLdouble r, GLdouble g, GLdouble b) { set_color((float)r, (float)g, (float)b, 1); }
void impl_glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) { set_color((float)r, (float)g, (float)b, (float)a); }
void impl_glTexCoord2f(GLfloat s, GLfloat t) { C->cur_tc[0][0] = s; C->cur_tc[0][1] = t; }
void impl_glTexCoord2fv(const GLfloat *v) { C->cur_tc[0][0] = v[0]; C->cur_tc[0][1] = v[1]; }
void impl_glTexCoord2d(GLdouble s, GLdouble t) { impl_glTexCoord2f((float)s, (float)t); }
void impl_glTexCoord2i(GLint s, GLint t) { impl_glTexCoord2f((float)s, (float)t); }
void impl_glMultiTexCoord2fARB(GLenum u, GLfloat s, GLfloat t) {
    int i = (int)(u - GL_TEXTURE0_ARB);
    if (i >= 0 && i < GL_MAX_UNITS) { C->cur_tc[i][0] = s; C->cur_tc[i][1] = t; }
}
void impl_glMultiTexCoord2fvARB(GLenum u, const GLfloat *v) { impl_glMultiTexCoord2fARB(u, v[0], v[1]); }
void impl_glNormal3f(GLfloat x, GLfloat y, GLfloat z) { C->cur_normal[0] = x; C->cur_normal[1] = y; C->cur_normal[2] = z; }
void impl_glNormal3fv(const GLfloat *v) { impl_glNormal3f(v[0], v[1], v[2]); }

// ------------------------------------------------------------------ misc no-ops the engine may call
void impl_glPushAttrib(GLbitfield m) { if (gl_tracing()) fprintf(stderr, "  glPushAttrib %x\n", m); }
void impl_glPopAttrib(void) { if (gl_tracing()) fprintf(stderr, "  glPopAttrib\n"); }
void impl_glPushClientAttrib(GLbitfield m) { (void)m; }
void impl_glPopClientAttrib(void) {}
void impl_glDeleteLists(GLuint l, GLsizei n) { (void)l; (void)n; }
GLuint impl_glGenLists(GLsizei n) { (void)n; return 0; }
void impl_glCallList(GLuint l) { (void)l; }
// ------------------------------------------------------------------ lighting
static void xform_mv(const float *m, const float *in, float *out) {   // out = M * in (4-vector)
    for (int r = 0; r < 4; r++) out[r] = m[r] * in[0] + m[4 + r] * in[1] + m[8 + r] * in[2] + m[12 + r] * in[3];
}
static void light_param(GLenum l, GLenum p, const float *v) {
    int i = (int)(l - 0x4000);
    if (i < 0 || i >= 8) return;
    struct GLLight *L = &C->light[i];
    const float *mv = gl_top(0);
    switch (p) {
    case 0x1200: memcpy(L->amb, v, 16); break;
    case 0x1201: memcpy(L->dif, v, 16); break;
    case 0x1202: memcpy(L->spec, v, 16); break;
    case 0x1203: xform_mv(mv, v, L->pos); break;
    case 0x1204: { float d[4] = {v[0], v[1], v[2], 0}, o[4]; xform_mv(mv, d, o); memcpy(L->spot_dir, o, 12); break; }
    case 0x1205: L->spot_exp = v[0]; break;
    case 0x1206: L->spot_cut = v[0]; break;
    case 0x1207: L->att[0] = v[0]; break;
    case 0x1208: L->att[1] = v[0]; break;
    case 0x1209: L->att[2] = v[0]; break;
    }
}
static void lighting_defaults(void) {
    for (int i = 0; i < 8; i++) {
        struct GLLight *L = &C->light[i];
        memset(L, 0, sizeof *L);
        L->amb[3] = 1;
        if (i == 0) { for (int k = 0; k < 4; k++) L->dif[k] = L->spec[k] = 1; }
        else { L->dif[3] = L->spec[3] = 1; }
        L->pos[2] = 1; L->spot_dir[2] = -1; L->spot_cut = 180; L->att[0] = 1;
    }
    for (int k = 0; k < 3; k++) { C->model_ambient[k] = 0.2f; C->mat_amb[k] = 0.2f; C->mat_dif[k] = 0.8f; }
    C->model_ambient[3] = C->mat_amb[3] = C->mat_dif[3] = C->mat_spec[3] = 1;
    C->color_material_mode = 0x1602;
}
void impl_glLightf(GLenum l, GLenum p, GLfloat v) { light_param(l, p, &v); }
void impl_glLighti(GLenum l, GLenum p, GLint v) { float f = (float)v; light_param(l, p, &f); }
void impl_glLightfv(GLenum l, GLenum p, const GLfloat *v) { light_param(l, p, v); }
void impl_glLightModelfv(GLenum p, const GLfloat *v) { if (p == 0x0B53) memcpy(C->model_ambient, v, 16); }
void impl_glLightModelf(GLenum p, GLfloat v) { (void)p; (void)v; }
void impl_glLightModeli(GLenum p, GLint v) { (void)p; (void)v; }
static void material(GLenum p, const float *v) {
    switch (p) {
    case 0x1200: memcpy(C->mat_amb, v, 16); break;
    case 0x1201: memcpy(C->mat_dif, v, 16); break;
    case 0x1202: memcpy(C->mat_spec, v, 16); break;
    case 0x1600: memcpy(C->mat_emi, v, 16); break;
    case 0x1601: C->mat_shin = v[0]; break;
    case 0x1602: memcpy(C->mat_amb, v, 16); memcpy(C->mat_dif, v, 16); break;
    }
}
void impl_glMaterialfv(GLenum f, GLenum p, const GLfloat *v) { if (f != GL_BACK) material(p, v); }
void impl_glMaterialf(GLenum f, GLenum p, GLfloat v) { if (f != GL_BACK) material(p, &v); }
void impl_glColorMaterial(GLenum f, GLenum m) { (void)f; C->color_material_mode = m; }
void impl_glRasterPos2f(GLfloat x, GLfloat y) { (void)x; (void)y; }
void impl_glLineStipple(GLint f, GLushort p) { (void)f; (void)p; }

// ------------------------------------------------------------------ gamma (GDI SetDeviceGammaRamp)
void gl_set_gamma_ramp(const uint16_t *r) {
    ctx_init();
    bool ident = true;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) {
            C->gamma[c][i] = r[c * 256 + i] / 65535.f;
            if (r[c * 256 + i] / 257 != i) ident = false;
        }
    C->gamma_identity = ident;
}
void gl_swap_buffers(void) {
    ctx_init();
    hud_end_frame();
    mtl_present();
}

// ------------------------------------------------------------------ opengl32 module: wgl + entry table
extern const struct GlEntry { const char *name; GuestFn fn; } g_gl_entries[];
extern const uint32_t g_gl_entry_count;

#define WGL(name) static void wgl_##name(Cpu *cpu)
static uint32_t g_hglrc;
WGL(wglCreateContext) { ctx_init(); RET(0x60001, 1); }
WGL(wglDeleteContext) { if (ARG(0) == g_hglrc) g_hglrc = 0; RET(1, 1); }
WGL(wglMakeCurrent) { g_hglrc = ARG(1); if (g_hglrc) ctx_init(); RET(1, 2); }
WGL(wglGetCurrentContext) { RET(g_hglrc, 0); }
WGL(wglGetCurrentDC) { RET(g_hglrc ? 0x20001 : 0, 0); }
WGL(wglShareLists) { RET(1, 2); }
WGL(wglCopyContext) { RET(1, 3); }
WGL(wglChoosePixelFormat) { RET(1, 2); }
WGL(wglGetPixelFormat) { RET(1, 1); }
WGL(wglSetPixelFormat) { RET(1, 3); }
WGL(wglDescribePixelFormat) {
    extern void imp_GDI32_DescribePixelFormat(Cpu *);
    imp_GDI32_DescribePixelFormat(cpu);
}
WGL(wglSwapBuffers) { gl_swap_buffers(); RET(1, 1); }
WGL(wglSwapLayerBuffers) { gl_swap_buffers(); RET(1, 2); }
WGL(wglCreateLayerContext) { RET(0, 2); }
WGL(wglDescribeLayerPlane) { RET(0, 5); }
WGL(wglGetLayerPaletteEntries) { RET(0, 5); }
WGL(wglSetLayerPaletteEntries) { RET(0, 5); }
WGL(wglRealizeLayerPalette) { RET(0, 3); }
WGL(wglUseFontBitmaps) { RET(0, 4); }
WGL(wglUseFontOutlines) { RET(0, 8); }
WGL(wglSwapIntervalEXT) { g_gl.swap_interval = (int)ARG(0); RET(1, 1); }
WGL(wglGetSwapIntervalEXT) { RET((uint32_t)g_gl.swap_interval, 0); }
WGL(wglGetExtensionsStringEXT) {
    static uint32_t s;
    if (!s) s = gstrdup("WGL_EXT_swap_control WGL_EXT_extensions_string");
    RET(s, 0);
}
WGL(wglGetExtensionsStringARB) {
    static uint32_t s;
    if (!s) s = gstrdup("WGL_EXT_swap_control WGL_EXT_extensions_string");
    RET(s, 1);
}
uint32_t gl_proc_address(const char *name);
WGL(wglGetProcAddress) {
    const char *n = ARGP(0);
    uint32_t t = gl_proc_address(n);
    LOG("wglGetProcAddress(%s) -> %08x", n, t);
    RET(t, 1);
}

static const struct { const char *name; GuestFn fn; } g_wgl[] = {
#define E(n) {#n, wgl_##n}
    E(wglCreateContext), E(wglDeleteContext), E(wglMakeCurrent), E(wglGetCurrentContext), E(wglGetCurrentDC),
    E(wglShareLists), E(wglCopyContext), E(wglChoosePixelFormat), E(wglGetPixelFormat), E(wglSetPixelFormat),
    E(wglDescribePixelFormat), E(wglSwapBuffers), E(wglSwapLayerBuffers), E(wglCreateLayerContext),
    E(wglDescribeLayerPlane), E(wglGetLayerPaletteEntries), E(wglSetLayerPaletteEntries), E(wglRealizeLayerPalette),
    E(wglUseFontBitmaps), E(wglUseFontOutlines), E(wglSwapIntervalEXT), E(wglGetSwapIntervalEXT),
    E(wglGetExtensionsStringEXT), E(wglGetExtensionsStringARB), E(wglGetProcAddress),
#undef E
};

uint32_t gl_proc_address(const char *name) {
    for (size_t i = 0; i < sizeof g_wgl / sizeof *g_wgl; i++)
        if (!strcmp(g_wgl[i].name, name)) return rt_thunk(g_wgl[i].fn, g_wgl[i].name);
    for (uint32_t i = 0; i < g_gl_entry_count; i++)
        if (!strcmp(g_gl_entries[i].name, name)) return rt_thunk(g_gl_entries[i].fn, g_gl_entries[i].name);
    return 0;
}
