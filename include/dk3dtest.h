#ifndef DK3DTEST_H
#define DK3DTEST_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <deko3d.h>

#define RENDER_W  320
#define RENDER_H  240

typedef struct {
    float pos[3];
    float color[3];
} vertex_t;

static const vertex_t s_cube_verts[8] = {
    {{-1,-1,-1}, {0,0,0}},
    {{ 1,-1,-1}, {1,0,0}},
    {{ 1, 1,-1}, {1,1,0}},
    {{-1, 1,-1}, {0,1,0}},
    {{-1,-1, 1}, {0,0,1}},
    {{ 1,-1, 1}, {1,0,1}},
    {{ 1, 1, 1}, {1,1,1}},
    {{-1, 1, 1}, {0,1,1}},
};

static const uint16_t s_cube_indices[36] = {
    0,1,2, 0,2,3,
    4,6,5, 4,7,6,
    0,3,7, 0,7,4,
    1,5,6, 1,6,2,
    0,4,5, 0,5,1,
    3,2,6, 3,6,7,
};

static const uint16_t s_cube_edges[24] = {
    0,1, 1,2, 2,3, 3,0,
    4,5, 5,6, 6,7, 7,4,
    0,4, 1,5, 2,6, 3,7,
};

#define MARKER_T 0.05f
static const vertex_t s_marker_verts[16] = {
    /* top — blue */
    {{-1.0f, +1.0f - MARKER_T, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{+1.0f, +1.0f - MARKER_T, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{+1.0f, +1.0f,            0.0f}, {0.0f, 0.0f, 1.0f}},
    {{-1.0f, +1.0f,            0.0f}, {0.0f, 0.0f, 1.0f}},
    /* bottom — red */
    {{-1.0f, -1.0f,            0.0f}, {1.0f, 0.0f, 0.0f}},
    {{+1.0f, -1.0f,            0.0f}, {1.0f, 0.0f, 0.0f}},
    {{+1.0f, -1.0f + MARKER_T, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{-1.0f, -1.0f + MARKER_T, 0.0f}, {1.0f, 0.0f, 0.0f}},
    /* left — purple */
    {{-1.0f,            -1.0f, 0.0f}, {0.6f, 0.0f, 0.8f}},
    {{-1.0f + MARKER_T, -1.0f, 0.0f}, {0.6f, 0.0f, 0.8f}},
    {{-1.0f + MARKER_T, +1.0f, 0.0f}, {0.6f, 0.0f, 0.8f}},
    {{-1.0f,            +1.0f, 0.0f}, {0.6f, 0.0f, 0.8f}},
    /* right — orange */
    {{+1.0f - MARKER_T, -1.0f, 0.0f}, {1.0f, 0.5f, 0.0f}},
    {{+1.0f,            -1.0f, 0.0f}, {1.0f, 0.5f, 0.0f}},
    {{+1.0f,            +1.0f, 0.0f}, {1.0f, 0.5f, 0.0f}},
    {{+1.0f - MARKER_T, +1.0f, 0.0f}, {1.0f, 0.5f, 0.0f}},
};
static const uint16_t s_marker_indices[24] = {
     0, 1, 2,  0, 2, 3,
     4, 5, 6,  4, 6, 7,
     8, 9,10,  8,10,11,
    12,13,14, 12,14,15,
};

/* ------------------------------------------------------------------ */
/* Math helpers                                                       */
/* ------------------------------------------------------------------ */
static inline void mat4_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void mat4_mul(float r[16], const float a[16], const float b[16]) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i*4+j] = a[0*4+j]*b[i*4+0] + a[1*4+j]*b[i*4+1]
                     + a[2*4+j]*b[i*4+2] + a[3*4+j]*b[i*4+3];
    memcpy(r, t, sizeof(t));
}

static inline void mat4_perspective(float m[16], float fovy, float aspect,
                                    float zn, float zf) {
    float f = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = zf / (zn - zf);
    m[11] = -1.0f;
    m[14] = (zn * zf) / (zn - zf);
}

static inline void mat4_translate(float m[16], float x, float y, float z) {
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

static inline void mat4_rotate_y(float m[16], float a) {
    float c = cosf(a), s = sinf(a);
    mat4_identity(m);
    m[0] = c;  m[2] = s;
    m[8] = -s; m[10] = c;
}

static inline void mat4_rotate_x(float m[16], float a) {
    float c = cosf(a), s = sinf(a);
    mat4_identity(m);
    m[5] = c;  m[6] = s;
    m[9] = -s; m[10] = c;
}

/* ------------------------------------------------------------------ */
/* DKSH header                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t magic;
    uint32_t header_sz;
    uint32_t control_sz;
    uint32_t code_sz;
    uint32_t programs_off;
    uint32_t num_programs;
} dksh_header_t;

#define DKSH_MAGIC 0x48534B44u

static inline bool load_shader_blob(const uint8_t* blob, size_t blob_sz,
                                    DkShader* out, DkMemBlock code_mem,
                                    uint32_t* io_off) {
    if (blob_sz < sizeof(dksh_header_t)) return false;
    const dksh_header_t* hdr = (const dksh_header_t*)blob;
    if (hdr->magic != DKSH_MAGIC) return false;

    const void* control = blob;

    uint32_t off = (*io_off + DK_SHADER_CODE_ALIGNMENT - 1)
                 & ~(DK_SHADER_CODE_ALIGNMENT - 1);
    if (off + hdr->code_sz > dkMemBlockGetSize(code_mem)) return false;

    void* code_cpu = (uint8_t*)dkMemBlockGetCpuAddr(code_mem) + off;
    memcpy(code_cpu, blob + hdr->control_sz, hdr->code_sz);

    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, code_mem, off);
    sm.control = control;
    dkShaderInitialize(out, &sm);

    *io_off = off + hdr->code_sz;
    return true;
}

#endif /* DK3DTEST_H */
