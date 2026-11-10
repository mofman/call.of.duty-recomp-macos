// Fixed-function OpenGL 1.x state shared between the state tracker (gl_core.c) and the Metal backend (gl_metal.m).
#pragma once
#include "gl_types.h"
#include <stdbool.h>

#define GL_MAX_UNITS 2
#define GL_MAX_TEXTURES 65536
#define GL_MATRIX_DEPTH 32

// ---- enums (subset used by the engine) ------------------------------------------------------------------------
enum {
    GL_NO_ERROR = 0, GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE = 0x0501, GL_INVALID_OPERATION = 0x0502,
    GL_POINTS = 0, GL_LINES = 1, GL_LINE_LOOP = 2, GL_LINE_STRIP = 3, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5,
    GL_TRIANGLE_FAN = 6, GL_QUADS = 7, GL_QUAD_STRIP = 8, GL_POLYGON = 9,
    GL_NEVER = 0x0200, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS,
    GL_ZERO = 0, GL_ONE = 1, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE,
    GL_FRONT = 0x0404, GL_BACK = 0x0405, GL_FRONT_AND_BACK = 0x0408, GL_CW = 0x0900, GL_CCW = 0x0901,
    GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_STENCIL_TEST = 0x0B90, GL_ALPHA_TEST = 0x0BC0,
    GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11, GL_TEXTURE_2D = 0x0DE1, GL_FOG = 0x0B60, GL_LIGHTING = 0x0B50,
    GL_POLYGON_OFFSET_FILL = 0x8037, GL_POLYGON_OFFSET_LINE = 0x2A02, GL_CLIP_PLANE0 = 0x3000, GL_DITHER = 0x0BD0,
    GL_TEXTURE_GEN_S = 0x0C60, GL_TEXTURE_GEN_T = 0x0C61, GL_NORMALIZE = 0x0BA1, GL_COLOR_MATERIAL = 0x0B57,
    GL_LINE_SMOOTH = 0x0B20, GL_POLYGON_SMOOTH = 0x0B41, GL_MULTISAMPLE = 0x809D, GL_POINT_SMOOTH = 0x0B10,
    GL_POLYGON_STIPPLE = 0x0B42, GL_LINE_STIPPLE = 0x0B24,
    GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078,
    GL_BYTE = 0x1400, GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT, GL_INT, GL_UNSIGNED_INT, GL_FLOAT,
    GL_DOUBLE = 0x140A,
    GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701, GL_TEXTURE = 0x1702,
    GL_TEXTURE0_ARB = 0x84C0,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100, GL_STENCIL_BUFFER_BIT = 0x0400,
    GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909, GL_LUMINANCE_ALPHA = 0x190A,
    GL_INTENSITY = 0x8049, GL_BGR_EXT = 0x80E0, GL_BGRA_EXT = 0x80E1, GL_DEPTH_COMPONENT = 0x1902,
    GL_COMPRESSED_RGB_S3TC_DXT1_EXT = 0x83F0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1,
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST = 0x2701,
    GL_NEAREST_MIPMAP_LINEAR = 0x2702, GL_LINEAR_MIPMAP_LINEAR = 0x2703,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE, GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF,
    GL_CLAMP = 0x2900, GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F, GL_TEXTURE_BORDER_COLOR = 0x1004,
    GL_TEXTURE_ENV = 0x2300, GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_ENV_COLOR = 0x2201,
    GL_MODULATE = 0x2100, GL_DECAL = 0x2101, GL_ADD = 0x0104, GL_REPLACE = 0x1E01, GL_BLEND_ENV = 0x0BE2,
    GL_COMBINE_ARB = 0x8570, GL_COMBINE_RGB_ARB = 0x8571, GL_COMBINE_ALPHA_ARB = 0x8572,
    GL_RGB_SCALE_ARB = 0x8573, GL_ADD_SIGNED_ARB = 0x8574, GL_INTERPOLATE_ARB = 0x8575, GL_SUBTRACT_ARB = 0x84E7,
    GL_CONSTANT_ARB = 0x8576, GL_PRIMARY_COLOR_ARB = 0x8577, GL_PREVIOUS_ARB = 0x8578,
    GL_SOURCE0_RGB_ARB = 0x8580, GL_SOURCE0_ALPHA_ARB = 0x8588, GL_OPERAND0_RGB_ARB = 0x8590,
    GL_OPERAND0_ALPHA_ARB = 0x8598, GL_ALPHA_SCALE = 0x0D1C,
    GL_FOG_MODE = 0x0B65, GL_FOG_DENSITY = 0x0B62, GL_FOG_START = 0x0B63, GL_FOG_END = 0x0B64, GL_FOG_COLOR = 0x0B66,
    GL_EXP = 0x0800, GL_EXP2 = 0x0801, GL_FOG_HINT = 0x0C54, GL_FOG_DISTANCE_MODE_NV = 0x855A,
    GL_KEEP = 0x1E00, GL_INCR = 0x1E02, GL_DECR = 0x1E03, GL_INVERT = 0x150A, GL_INCR_WRAP = 0x8507, GL_DECR_WRAP = 0x8508,
    GL_POINT = 0x1B00, GL_LINE = 0x1B01, GL_FILL = 0x1B02,
    GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ROW_LENGTH = 0x0CF2,
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_EXTENSIONS = 0x1F03,
    GL_MAX_TEXTURE_SIZE = 0x0D33, GL_MAX_TEXTURE_UNITS_ARB = 0x84E2, GL_VIEWPORT = 0x0BA2, GL_DEPTH_BITS = 0x0D56,
    GL_STENCIL_BITS = 0x0D57, GL_RED_BITS = 0x0D52, GL_GREEN_BITS = 0x0D53, GL_BLUE_BITS = 0x0D54, GL_ALPHA_BITS = 0x0D55,
    GL_MODELVIEW_MATRIX = 0x0BA6, GL_PROJECTION_MATRIX = 0x0BA7, GL_TEXTURE_MATRIX = 0x0BA8,
    GL_MAX_ELEMENTS_VERTICES = 0x80E8, GL_MAX_ELEMENTS_INDICES = 0x80E9, GL_SCISSOR_BOX = 0x0C10,
    GL_NUM_COMPRESSED_TEXTURE_FORMATS = 0x86A2, GL_COMPRESSED_TEXTURE_FORMATS = 0x86A3,
};

// ---- client arrays -------------------------------------------------------------------------------------------
typedef struct GLArray { bool enabled; GLint size; GLenum type; GLsizei stride; const uint8_t *ptr; } GLArray;

// ---- texture env per unit ------------------------------------------------------------------------------------
typedef struct GLTexEnv {
    GLenum mode;                         // MODULATE / REPLACE / DECAL / ADD / BLEND / COMBINE
    GLenum combine_rgb, combine_alpha;
    GLenum src_rgb[3], src_alpha[3], op_rgb[3], op_alpha[3];
    float rgb_scale, alpha_scale;
    float color[4];
} GLTexEnv;

// ---- textures ------------------------------------------------------------------------------------------------
typedef struct GLTexture {
    void *mtl;                           // id<MTLTexture> (retained, bridged)
    int w, h, levels, max_level_loaded;
    GLenum ifmt;                         // GL internal format given at level 0
    bool compressed, opaque;             // opaque: alpha forced to 1 (RGB formats)
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    float aniso;
    void *sampler;                       // cached id<MTLSamplerState> (invalidated on parameter change)
} GLTexture;

typedef struct GLContext {
    // matrices: 0 modelview, 1 projection, 2.. texture units
    float mstack[2 + GL_MAX_UNITS][GL_MATRIX_DEPTH][16];
    int mdepth[2 + GL_MAX_UNITS];
    int matrix_mode;                     // index into mstack
    // raster state
    bool cull, depth_test, blend, alpha_test, scissor_test, stencil_test, fog, clip0, poly_offset_fill, poly_offset_line;
    bool tex2d[GL_MAX_UNITS];
    // fixed-function lighting (GL 1.x): positions/directions stored in eye space at specification time
    bool lighting, color_material, light_on[8];
    struct GLLight { float amb[4], dif[4], spec[4], pos[4], spot_dir[3], spot_exp, spot_cut, att[3]; } light[8];
    float model_ambient[4];
    float mat_amb[4], mat_dif[4], mat_spec[4], mat_emi[4], mat_shin;
    GLenum color_material_mode;
    GLenum cull_face, front_face, depth_func, blend_src, blend_dst, alpha_func;
    float alpha_ref;
    bool depth_mask;
    bool color_mask[4];
    float poly_factor, poly_units;
    GLenum polygon_mode;
    int viewport[4], scissor[4];
    double depth_near, depth_far;
    float clear_color[4]; double clear_depth; int clear_stencil;
    // stencil
    GLenum stencil_func; int stencil_ref; uint32_t stencil_mask, stencil_writemask;
    GLenum stencil_fail, stencil_zfail, stencil_zpass;
    // fog
    GLenum fog_mode; float fog_density, fog_start, fog_end, fog_color[4];
    // clip plane 0 in eye space
    float clip_plane[4];
    // texture units
    int active_unit, client_unit;
    GLuint bound[GL_MAX_UNITS];
    GLTexEnv env[GL_MAX_UNITS];
    // current vertex attributes
    float cur_color[4], cur_tc[GL_MAX_UNITS][4], cur_normal[3];
    // client arrays
    GLArray va, ca, na, ta[GL_MAX_UNITS];
    int lock_first, lock_count;
    // pixel store
    int unpack_align, pack_align, unpack_row_length;
    GLenum error;
    // immediate mode
    bool in_begin;
    GLenum begin_mode;
    // textures
    GLTexture *tex[GL_MAX_TEXTURES];
    // framebuffer size (pixels) of the current mode
    int fb_w, fb_h;
    float gamma[3][256];
    bool gamma_identity;
    int swap_interval;
} GLContext;

extern GLContext g_gl;

// Vertex as consumed by the Metal shaders.
typedef struct GLVtx { float pos[4]; float tc[GL_MAX_UNITS][2]; uint8_t col[4]; float nrm[3]; } GLVtx;

// primitive kinds after conversion
enum { PRIM_TRIANGLES, PRIM_TRISTRIP, PRIM_LINES, PRIM_LINESTRIP, PRIM_POINTS };

// ---- backend (gl_metal.m) --------------------------------------------------------------------------------------
void mtl_init(void);
void mtl_begin_frame_if_needed(void);
void mtl_draw(int prim, const GLVtx *v, uint32_t nv, const uint32_t *idx, uint32_t ni);
void mtl_clear(GLbitfield mask);
void mtl_texture_storage(GLTexture *t, int w, int h, int levels, GLenum fmt_kind);
void mtl_texture_upload(GLTexture *t, int level, int x, int y, int w, int h, const void *data, size_t bytes_per_row, size_t bytes);
void mtl_texture_release(GLTexture *t);
void mtl_copy_framebuffer_to_texture(GLTexture *t, int level, int dx, int dy, int sx, int sy, int w, int h);
void mtl_read_pixels(int x, int y, int w, int h, uint8_t *rgba_out);   // bottom-up rows like GL
void mtl_present(void);
void mtl_finish(void);
void mtl_resize_framebuffer(int w, int h);

// texture storage kinds for mtl_texture_storage
enum { TEXK_RGBA8, TEXK_BC1, TEXK_BC2, TEXK_BC3 };

// helpers in gl_core.c
void gl_mat_mul(float *out, const float *a, const float *b);
const float *gl_top(int which);
