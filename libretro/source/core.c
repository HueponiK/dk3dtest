/*
 * dk3dtest libretro core — renders a deko3d spinning cube into an offscreen
 * DkImage and hands it to the RetroArch frontend via
 * retro_hw_render_interface_deko3d.
 */

#include <stdio.h>
#include <stdlib.h>

#include "dk3dtest.h"
#include <libretro.h>
#include <libretro_deko3d.h>

#define FB_FPS          60.0
#define MAX_SLOTS       4
#define CODE_MEM_SIZE   (64 * 1024)
#define DATA_MEM_SIZE   (64 * 1024)
#define CMD_SLICE_SIZE  (256 * 1024)

/* Embedded DKSH shader blobs (from build/<name>_dksh.o) */
extern const uint8_t cube_vsh_dksh[];
extern const uint8_t cube_vsh_dksh_end[];
extern const uint8_t cube_fsh_dksh[];
extern const uint8_t cube_fsh_dksh_end[];
extern const uint8_t cube_edge_fsh_dksh[];
extern const uint8_t cube_edge_fsh_dksh_end[];

/* ------------------------------------------------------------------ */
/* GPU state                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    DkImage      img;
    DkMemBlock   img_mem;
    DkImage      depth;
    DkMemBlock   depth_mem;
    DkCmdBuf     cmdbuf;
    DkMemBlock   cmd_mem;
    DkFence      acquire;
    DkFence      release;
    bool         release_valid;
} slot_t;

typedef struct {
    bool         initialized;
    DkDevice     device;
    DkQueue      queue;
    DkMemBlock   code_mem;
    DkMemBlock   data_mem;
    DkGpuAddr    data_gpu;
    uint8_t*     data_cpu;
    DkShader     vsh, fsh, fsh_edge;
    uint32_t     num_slots;
    slot_t       slot[MAX_SLOTS];
    float        angle;
    uint32_t     ubo_offs[MAX_SLOTS];
    uint32_t     off_vb, off_ib, off_eib, off_mvb, off_mib, off_ubo_ident;
} gpu_t;

static gpu_t g_gpu;

/* ------------------------------------------------------------------ */
/* libretro callbacks                                                 */
/* ------------------------------------------------------------------ */
static retro_log_printf_t         log_cb;
static retro_environment_t        environ_cb;
#define LOG(level, ...) do { \
    if (log_cb) log_cb((level), __VA_ARGS__); \
    else        fprintf(stderr,  __VA_ARGS__); \
} while (0)

static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static struct retro_hw_render_callback hw_render;

/* ------------------------------------------------------------------ */
/* GPU init / teardown                                                */
/* ------------------------------------------------------------------ */
static bool gpu_init(const struct retro_hw_render_interface_deko3d* iface) {
    memset(&g_gpu, 0, sizeof(g_gpu));
    g_gpu.device = iface->device;

    uint32_t mask = iface->get_sync_index_mask(iface->handle);
    uint32_t n = 0;
    for (uint32_t b = 0; b < 32; b++) if (mask & (1u << b)) n++;
    if (n == 0 || n > MAX_SLOTS) n = 2;
    g_gpu.num_slots = n;
    LOG(RETRO_LOG_INFO, "[dk3dtest] gpu_init: sync mask=0x%x slots=%u\n", mask, n);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_gpu.device);
    qm.flags             = DkQueueFlags_Graphics;
    qm.commandMemorySize = DK_QUEUE_MIN_CMDMEM_SIZE;
    g_gpu.queue = dkQueueCreate(&qm);
    if (!g_gpu.queue) { LOG(RETRO_LOG_ERROR, "[dk3dtest] dkQueueCreate failed\n"); return false; }

    DkMemBlockMaker mbm;

    dkMemBlockMakerDefaults(&mbm, g_gpu.device, CODE_MEM_SIZE);
    mbm.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached
              | DkMemBlockFlags_Code;
    g_gpu.code_mem = dkMemBlockCreate(&mbm);

    uint32_t code_off = 0;
    if (!load_shader_blob(cube_vsh_dksh,      cube_vsh_dksh_end      - cube_vsh_dksh,
                          &g_gpu.vsh,      g_gpu.code_mem, &code_off)) return false;
    if (!load_shader_blob(cube_fsh_dksh,      cube_fsh_dksh_end      - cube_fsh_dksh,
                          &g_gpu.fsh,      g_gpu.code_mem, &code_off)) return false;
    if (!load_shader_blob(cube_edge_fsh_dksh, cube_edge_fsh_dksh_end - cube_edge_fsh_dksh,
                          &g_gpu.fsh_edge, g_gpu.code_mem, &code_off)) return false;
    dkMemBlockFlushCpuCache(g_gpu.code_mem, 0, CODE_MEM_SIZE);

    dkMemBlockMakerDefaults(&mbm, g_gpu.device, DATA_MEM_SIZE);
    mbm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_gpu.data_mem = dkMemBlockCreate(&mbm);
    g_gpu.data_cpu = (uint8_t*)dkMemBlockGetCpuAddr(g_gpu.data_mem);
    g_gpu.data_gpu = dkMemBlockGetGpuAddr(g_gpu.data_mem);

    /* Layout: all UBO binds must be 256-aligned. */
    g_gpu.off_vb        = 0;
    g_gpu.off_ib        = 256;
    g_gpu.off_eib       = 384;
    g_gpu.off_mvb       = 512;
    g_gpu.off_mib       = 896;
    g_gpu.off_ubo_ident = 1024;
    for (uint32_t i = 0; i < g_gpu.num_slots; i++)
        g_gpu.ubo_offs[i] = 1280 + i * 256;

    memcpy(g_gpu.data_cpu + g_gpu.off_vb,    s_cube_verts,     sizeof(s_cube_verts));
    memcpy(g_gpu.data_cpu + g_gpu.off_ib,    s_cube_indices,   sizeof(s_cube_indices));
    memcpy(g_gpu.data_cpu + g_gpu.off_eib,   s_cube_edges,     sizeof(s_cube_edges));
    memcpy(g_gpu.data_cpu + g_gpu.off_mvb,   s_marker_verts,   sizeof(s_marker_verts));
    memcpy(g_gpu.data_cpu + g_gpu.off_mib,   s_marker_indices, sizeof(s_marker_indices));
    {
        float ident[16];
        mat4_identity(ident);
        memcpy(g_gpu.data_cpu + g_gpu.off_ubo_ident, ident, sizeof(ident));
    }

    DkImageLayoutMaker ilm;
    DkImageLayout color_layout, depth_layout;

    dkImageLayoutMakerDefaults(&ilm, g_gpu.device);
    ilm.flags = DkImageFlags_UsageRender | DkImageFlags_UsageLoadStore
              | DkImageFlags_Usage2DEngine;
    ilm.format = DkImageFormat_RGBA8_Unorm;
    ilm.dimensions[0] = RENDER_W;
    ilm.dimensions[1] = RENDER_H;
    dkImageLayoutInitialize(&color_layout, &ilm);

    dkImageLayoutMakerDefaults(&ilm, g_gpu.device);
    ilm.flags  = DkImageFlags_UsageRender;
    ilm.format = DkImageFormat_Z24S8;
    ilm.dimensions[0] = RENDER_W;
    ilm.dimensions[1] = RENDER_H;
    dkImageLayoutInitialize(&depth_layout, &ilm);

    uint32_t color_sz   = dkImageLayoutGetSize(&color_layout);
    uint32_t color_alig = dkImageLayoutGetAlignment(&color_layout);
    uint32_t depth_sz   = dkImageLayoutGetSize(&depth_layout);
    uint32_t depth_alig = dkImageLayoutGetAlignment(&depth_layout);

    for (uint32_t i = 0; i < g_gpu.num_slots; i++) {
        slot_t* s = &g_gpu.slot[i];

        uint32_t mb_sz = (color_sz + color_alig - 1) & ~(color_alig - 1);
        dkMemBlockMakerDefaults(&mbm, g_gpu.device, mb_sz);
        mbm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image
                  | DkMemBlockFlags_ZeroFillInit;
        s->img_mem = dkMemBlockCreate(&mbm);
        dkImageInitialize(&s->img, &color_layout, s->img_mem, 0);

        mb_sz = (depth_sz + depth_alig - 1) & ~(depth_alig - 1);
        dkMemBlockMakerDefaults(&mbm, g_gpu.device, mb_sz);
        mbm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image
                  | DkMemBlockFlags_ZeroFillInit;
        s->depth_mem = dkMemBlockCreate(&mbm);
        dkImageInitialize(&s->depth, &depth_layout, s->depth_mem, 0);

        dkMemBlockMakerDefaults(&mbm, g_gpu.device, CMD_SLICE_SIZE);
        mbm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        s->cmd_mem = dkMemBlockCreate(&mbm);

        DkCmdBufMaker cbm;
        dkCmdBufMakerDefaults(&cbm, g_gpu.device);
        s->cmdbuf = dkCmdBufCreate(&cbm);

        memset(&s->acquire, 0, sizeof(s->acquire));
        memset(&s->release, 0, sizeof(s->release));
        s->release_valid = false;
    }

    g_gpu.initialized = true;
    LOG(RETRO_LOG_INFO, "[dk3dtest] gpu_init done\n");
    return true;
}

static void gpu_destroy(void) {
    if (!g_gpu.initialized) return;
    if (g_gpu.queue) dkQueueWaitIdle(g_gpu.queue);

    for (uint32_t i = 0; i < g_gpu.num_slots; i++) {
        slot_t* s = &g_gpu.slot[i];
        if (s->cmdbuf)    dkCmdBufDestroy(s->cmdbuf);
        if (s->cmd_mem)   dkMemBlockDestroy(s->cmd_mem);
        if (s->depth_mem) dkMemBlockDestroy(s->depth_mem);
        if (s->img_mem)   dkMemBlockDestroy(s->img_mem);
    }
    if (g_gpu.data_mem) dkMemBlockDestroy(g_gpu.data_mem);
    if (g_gpu.code_mem) dkMemBlockDestroy(g_gpu.code_mem);
    if (g_gpu.queue)    dkQueueDestroy(g_gpu.queue);
    memset(&g_gpu, 0, sizeof(g_gpu));
}

/* ------------------------------------------------------------------ */
/* Record + submit one frame                                          */
/* ------------------------------------------------------------------ */
static void render_frame(uint32_t i) {
    slot_t* s = &g_gpu.slot[i];

    if (s->release_valid) {
        dkFenceWait(&s->release, -1);
        s->release_valid = false;
    }

    g_gpu.angle += 0.02f;
    float proj[16], view[16], rx[16], ry[16], tmp[16], mvp[16];
    mat4_perspective(proj, 1.0472f, (float)RENDER_W / (float)RENDER_H, 0.1f, 100.0f);
    mat4_translate(view, 0.0f, 0.0f, -5.0f);
    mat4_rotate_x(rx, g_gpu.angle * 0.7f);
    mat4_rotate_y(ry, g_gpu.angle);
    mat4_mul(tmp, rx, ry);
    mat4_mul(tmp, view, tmp);
    mat4_mul(mvp, proj, tmp);
    memcpy(g_gpu.data_cpu + g_gpu.ubo_offs[i], mvp, sizeof(mvp));

    dkCmdBufClear(s->cmdbuf);
    dkCmdBufAddMemory(s->cmdbuf, s->cmd_mem, 0, CMD_SLICE_SIZE);

    DkImageView color_view, depth_view;
    dkImageViewDefaults(&color_view, &s->img);
    dkImageViewDefaults(&depth_view, &s->depth);
    DkImageView const* rt[] = { &color_view };
    dkCmdBufBindRenderTargets(s->cmdbuf, rt, 1, &depth_view);

    DkViewport vp = { 0.0f, 0.0f, (float)RENDER_W, (float)RENDER_H, 0.0f, 1.0f };
    DkScissor  sc = { 0, 0, RENDER_W, RENDER_H };
    dkCmdBufSetViewports(s->cmdbuf, 0, &vp, 1);
    dkCmdBufSetScissors (s->cmdbuf, 0, &sc, 1);

    dkCmdBufClearColorFloat(s->cmdbuf, 0, DkColorMask_RGBA,
                            0.1f, 0.15f, 0.25f, 1.0f);
    dkCmdBufClearDepthStencil(s->cmdbuf, true, 1.0f, 0xff, 0);

    DkShader const* shaders[] = { &g_gpu.vsh, &g_gpu.fsh };
    dkCmdBufBindShaders(s->cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

    DkRasterizerState   raster;   dkRasterizerStateDefaults(&raster);
    DkColorState        cstate;   dkColorStateDefaults(&cstate);
    DkColorWriteState   cwstate;  dkColorWriteStateDefaults(&cwstate);
    DkDepthStencilState dsstate;  dkDepthStencilStateDefaults(&dsstate);
    dkCmdBufBindRasterizerState   (s->cmdbuf, &raster);
    dkCmdBufBindColorState        (s->cmdbuf, &cstate);
    dkCmdBufBindColorWriteState   (s->cmdbuf, &cwstate);
    dkCmdBufBindDepthStencilState (s->cmdbuf, &dsstate);

    DkVtxAttribState attribs[] = {
        { 0, 0, offsetof(vertex_t, pos),   DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
        { 0, 0, offsetof(vertex_t, color), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
    };
    DkVtxBufferState buffers[] = { { sizeof(vertex_t), 0 } };
    dkCmdBufBindVtxAttribState(s->cmdbuf, attribs, 2);
    dkCmdBufBindVtxBufferState(s->cmdbuf, buffers, 1);

    dkCmdBufBindVtxBuffer(s->cmdbuf, 0, g_gpu.data_gpu + g_gpu.off_vb,
                          sizeof(s_cube_verts));
    dkCmdBufBindIdxBuffer(s->cmdbuf, DkIdxFormat_Uint16,
                          g_gpu.data_gpu + g_gpu.off_ib);
    dkCmdBufBindUniformBuffer(s->cmdbuf, DkStage_Vertex, 0,
                              g_gpu.data_gpu + g_gpu.ubo_offs[i], 64);
    dkCmdBufDrawIndexed(s->cmdbuf, DkPrimitive_Triangles, 36, 1, 0, 0, 0);

    /* Wireframe edges */
    DkShader const* edge_shaders[] = { &g_gpu.fsh_edge };
    dkCmdBufBindShaders(s->cmdbuf, DkStageFlag_Fragment, edge_shaders, 1);
    dsstate.depthCompareOp = DkCompareOp_Lequal;
    dkCmdBufBindDepthStencilState(s->cmdbuf, &dsstate);
    dkCmdBufBindIdxBuffer(s->cmdbuf, DkIdxFormat_Uint16,
                          g_gpu.data_gpu + g_gpu.off_eib);
    dkCmdBufDrawIndexed(s->cmdbuf, DkPrimitive_Lines, 24, 1, 0, 0, 0);

    /* Orientation markers — identity MVP, no depth test */
    DkShader const* color_shaders[] = { &g_gpu.fsh };
    dkCmdBufBindShaders(s->cmdbuf, DkStageFlag_Fragment, color_shaders, 1);
    dsstate.depthTestEnable = false;
    dkCmdBufBindDepthStencilState(s->cmdbuf, &dsstate);
    dkCmdBufBindVtxBuffer(s->cmdbuf, 0, g_gpu.data_gpu + g_gpu.off_mvb,
                          sizeof(s_marker_verts));
    dkCmdBufBindIdxBuffer(s->cmdbuf, DkIdxFormat_Uint16,
                          g_gpu.data_gpu + g_gpu.off_mib);
    dkCmdBufBindUniformBuffer(s->cmdbuf, DkStage_Vertex, 0,
                              g_gpu.data_gpu + g_gpu.off_ubo_ident, 64);
    dkCmdBufDrawIndexed(s->cmdbuf, DkPrimitive_Triangles, 24, 1, 0, 0, 0);

    dkCmdBufBarrier(s->cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);
    dkCmdBufSignalFence(s->cmdbuf, &s->acquire, true);

    DkCmdList list = dkCmdBufFinishList(s->cmdbuf);
    dkQueueSubmitCommands(g_gpu.queue, list);
    dkQueueFlush(g_gpu.queue);
}

/* ------------------------------------------------------------------ */
/* HW context lifecycle                                               */
/* ------------------------------------------------------------------ */
static void context_reset(void) {
    const struct retro_hw_render_interface_deko3d* iface = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE,
                    (void*)&iface) || !iface) {
        LOG(RETRO_LOG_ERROR, "[dk3dtest] context_reset: GET_HW_RENDER_INTERFACE failed\n");
        return;
    }
    if (iface->interface_type    != RETRO_HW_RENDER_INTERFACE_DEKO3D
     || iface->interface_version != RETRO_HW_RENDER_INTERFACE_DEKO3D_VERSION) {
        LOG(RETRO_LOG_ERROR, "[dk3dtest] context_reset: iface type/version mismatch\n");
        return;
    }
    gpu_init(iface);
}
static void context_destroy(void) { gpu_destroy(); }

/* ================================================================== */
/*  libretro API                                                       */
/* ================================================================== */

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "dk3dtest";
    info->library_version  = "0.1";
    info->need_fullpath    = false;
    info->valid_extensions = NULL;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = RENDER_W;
    info->geometry.base_height  = RENDER_H;
    info->geometry.max_width    = RENDER_W;
    info->geometry.max_height   = RENDER_H;
    info->geometry.aspect_ratio = (float)RENDER_W / (float)RENDER_H;
    info->timing.fps            = FB_FPS;
    info->timing.sample_rate    = 48000.0;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    bool no_rom = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

    struct retro_log_callback lc = { 0 };
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lc) && lc.log)
        log_cb = lc.log;
}

void retro_set_video_refresh(retro_video_refresh_t cb)          { video_cb       = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)            { audio_cb       = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb){ audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)                { input_poll_cb  = cb; }
void retro_set_input_state(retro_input_state_t cb)              { input_state_cb = cb; }

void retro_init(void)   { }
void retro_deinit(void) { }
void retro_reset(void)  { }
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }

bool retro_load_game(const struct retro_game_info *info) {
    (void)info;
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    memset(&hw_render, 0, sizeof(hw_render));
    hw_render.context_type    = RETRO_HW_CONTEXT_DEKO3D;
    hw_render.context_reset   = context_reset;
    hw_render.context_destroy = context_destroy;
    hw_render.bottom_left_origin = true;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
        return false;
    return true;
}

bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) {
    (void)t; (void)i; (void)n; return false;
}
void retro_unload_game(void) { }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

size_t retro_serialize_size(void) { return 0; }
bool   retro_serialize(void *d, size_t s)         { (void)d; (void)s; return false; }
bool   retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }

void   retro_cheat_reset(void) { }
void   retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }

void  *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

/* ------------------------------------------------------------------ */
/* retro_run                                                          */
/* ------------------------------------------------------------------ */
void retro_run(void) {
    if (input_poll_cb) input_poll_cb();

    if (!g_gpu.initialized) {
        if (video_cb) video_cb(NULL, RENDER_W, RENDER_H, 0);
        return;
    }

    const struct retro_hw_render_interface_deko3d* iface = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void*)&iface)
        || !iface) {
        if (video_cb) video_cb(NULL, RENDER_W, RENDER_H, 0);
        return;
    }

    uint32_t sync_idx = iface->get_sync_index(iface->handle);
    if (sync_idx >= g_gpu.num_slots) sync_idx = sync_idx % g_gpu.num_slots;

    render_frame(sync_idx);

    slot_t* s = &g_gpu.slot[sync_idx];
    struct retro_deko3d_image img = {
        .image                = &s->img,
        .width                = RENDER_W,
        .height               = RENDER_H,
        .display_aspect_ratio = (float)RENDER_W / (float)RENDER_H,
    };
    iface->set_image(iface->handle, &img, &s->acquire, &s->release);
    s->release_valid = true;

    if (video_cb) video_cb(RETRO_HW_FRAME_BUFFER_VALID, RENDER_W, RENDER_H, 0);

    if (audio_batch_cb) {
        static int16_t silence[800 * 2];
        audio_batch_cb(silence, 800);
    }
}
