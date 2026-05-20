/*
 * dk3dtest standalone — spinning cube rendered offscreen via deko3d,
 * then blitted to the swapchain. Proves the deko3d render path works
 * independently of the RetroArch frontend.
 */

#include <stdio.h>
#include <stdlib.h>

#include <switch.h>
#include "dk3dtest.h"

#define FB_WIDTH        1280
#define FB_HEIGHT       720
#define NUM_SC_IMAGES   2
#define NUM_CMD_SLICES  2
#define CODE_MEM_SIZE   (64 * 1024)
#define DATA_MEM_SIZE   (64 * 1024)
#define CMD_SLICE_SIZE  (256 * 1024)
#define CMD_MEM_SIZE    (NUM_CMD_SLICES * CMD_SLICE_SIZE)

static void dk3d_debug_cb(void* userData, const char* context,
                          DkResult result, const char* message)
{
    (void)userData;
    fprintf(stderr, "[deko3d] %s: result=%d (%s)\n",
            context ? context : "(no ctx)", (int)result,
            message ? message : "");
}

/* romfs shader loader */
static bool load_shader(const char* path, DkShader* out_shader,
                        DkMemBlock code_mem, uint32_t* io_code_offset)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[dk3dtest] shader open failed: %s\n", path);
        return false;
    }

    dksh_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 || hdr.magic != DKSH_MAGIC) {
        fprintf(stderr, "[dk3dtest] bad DKSH header: %s\n", path);
        fclose(fp);
        return false;
    }

    void* control = malloc(hdr.control_sz);
    if (!control) { fclose(fp); return false; }
    fseek(fp, 0, SEEK_SET);
    if (fread(control, hdr.control_sz, 1, fp) != 1) {
        free(control); fclose(fp); return false;
    }

    uint32_t off = (*io_code_offset + DK_SHADER_CODE_ALIGNMENT - 1)
                 & ~(DK_SHADER_CODE_ALIGNMENT - 1);
    if (off + hdr.code_sz > dkMemBlockGetSize(code_mem)) {
        fprintf(stderr, "[dk3dtest] code mem exhausted loading %s\n", path);
        free(control); fclose(fp); return false;
    }

    fseek(fp, hdr.control_sz, SEEK_SET);
    void* code_cpu = (uint8_t*)dkMemBlockGetCpuAddr(code_mem) + off;
    if (fread(code_cpu, hdr.code_sz, 1, fp) != 1) {
        free(control); fclose(fp); return false;
    }
    fclose(fp);

    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, code_mem, off);
    sm.control = control;
    dkShaderInitialize(out_shader, &sm);

    *io_code_offset = off + hdr.code_sz;
    return true;
}

int main(int argc, char* argv[])
{
    (void)argc; (void)argv;

    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        fprintf(stderr, "[dk3dtest] romfsInit failed: 0x%x\n", rc);
        return 1;
    }

    printf("[dk3dtest] standalone start\n");

    /* Device */
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.flags   = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginLowerLeft;
    dm.cbDebug = dk3d_debug_cb;
    DkDevice device = dkDeviceCreate(&dm);
    if (!device) { fprintf(stderr, "dkDeviceCreate failed\n"); return 1; }

    /* Queue */
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device);
    qm.flags             = DkQueueFlags_Graphics;
    qm.commandMemorySize = DK_QUEUE_MIN_CMDMEM_SIZE;
    DkQueue queue = dkQueueCreate(&qm);
    if (!queue) { fprintf(stderr, "dkQueueCreate failed\n"); return 1; }

    /* Command buffer + memory ring */
    DkMemBlockMaker mbm;
    dkMemBlockMakerDefaults(&mbm, device, CMD_MEM_SIZE);
    mbm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock cmd_mem = dkMemBlockCreate(&mbm);

    DkCmdBufMaker cbm;
    dkCmdBufMakerDefaults(&cbm, device);
    DkCmdBuf cmdbuf = dkCmdBufCreate(&cbm);

    DkFence cmd_fences[NUM_CMD_SLICES];
    memset(cmd_fences, 0, sizeof(cmd_fences));
    int cmd_slice = 0;

    /* Code memory + shaders */
    dkMemBlockMakerDefaults(&mbm, device, CODE_MEM_SIZE);
    mbm.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached
              | DkMemBlockFlags_Code;
    DkMemBlock code_mem = dkMemBlockCreate(&mbm);

    DkShader vsh, fsh, fsh_edge;
    uint32_t code_off = 0;
    if (!load_shader("romfs:/shaders/cube_vsh.dksh",      &vsh,      code_mem, &code_off))
        return 1;
    if (!load_shader("romfs:/shaders/cube_fsh.dksh",      &fsh,      code_mem, &code_off))
        return 1;
    if (!load_shader("romfs:/shaders/cube_edge_fsh.dksh", &fsh_edge, code_mem, &code_off))
        return 1;
    dkMemBlockFlushCpuCache(code_mem, 0, CODE_MEM_SIZE);

    /* Data memory (vertex, index, UBO) */
    dkMemBlockMakerDefaults(&mbm, device, DATA_MEM_SIZE);
    mbm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    DkMemBlock data_mem = dkMemBlockCreate(&mbm);

    uint8_t* data_cpu = (uint8_t*)dkMemBlockGetCpuAddr(data_mem);
    DkGpuAddr data_gpu = dkMemBlockGetGpuAddr(data_mem);

    const uint32_t off_vb        = 0;
    const uint32_t off_ib        = 256;
    const uint32_t off_eib       = 384;
    const uint32_t off_mvb       = 512;
    const uint32_t off_mib       = 896;
    const uint32_t off_ubo       = 1024;
    const uint32_t off_ubo_ident = 1280;
    memcpy(data_cpu + off_vb,  s_cube_verts,     sizeof(s_cube_verts));
    memcpy(data_cpu + off_ib,  s_cube_indices,   sizeof(s_cube_indices));
    memcpy(data_cpu + off_eib, s_cube_edges,     sizeof(s_cube_edges));
    memcpy(data_cpu + off_mvb, s_marker_verts,   sizeof(s_marker_verts));
    memcpy(data_cpu + off_mib, s_marker_indices, sizeof(s_marker_indices));
    {
        float ident[16];
        mat4_identity(ident);
        memcpy(data_cpu + off_ubo_ident, ident, sizeof(ident));
    }

    /* Image memory */
    DkImageLayoutMaker ilm;
    DkImageLayout offscreen_layout, depth_layout, swap_layout;

    dkImageLayoutMakerDefaults(&ilm, device);
    ilm.flags = DkImageFlags_UsageRender | DkImageFlags_UsageLoadStore
              | DkImageFlags_Usage2DEngine;
    ilm.format     = DkImageFormat_RGBA8_Unorm;
    ilm.dimensions[0] = RENDER_W;
    ilm.dimensions[1] = RENDER_H;
    dkImageLayoutInitialize(&offscreen_layout, &ilm);

    dkImageLayoutMakerDefaults(&ilm, device);
    ilm.flags  = DkImageFlags_UsageRender;
    ilm.format = DkImageFormat_Z24S8;
    ilm.dimensions[0] = RENDER_W;
    ilm.dimensions[1] = RENDER_H;
    dkImageLayoutInitialize(&depth_layout, &ilm);

    dkImageLayoutMakerDefaults(&ilm, device);
    ilm.flags  = DkImageFlags_UsageRender | DkImageFlags_UsagePresent
               | DkImageFlags_Usage2DEngine;
    ilm.format = DkImageFormat_RGBA8_Unorm;
    ilm.dimensions[0] = FB_WIDTH;
    ilm.dimensions[1] = FB_HEIGHT;
    dkImageLayoutInitialize(&swap_layout, &ilm);

    uint32_t off = 0;
    uint32_t off_offscreen = (off + dkImageLayoutGetAlignment(&offscreen_layout) - 1)
                          & ~(dkImageLayoutGetAlignment(&offscreen_layout) - 1);
    off = off_offscreen + dkImageLayoutGetSize(&offscreen_layout);

    uint32_t off_depth = (off + dkImageLayoutGetAlignment(&depth_layout) - 1)
                       & ~(dkImageLayoutGetAlignment(&depth_layout) - 1);
    off = off_depth + dkImageLayoutGetSize(&depth_layout);

    uint32_t off_swap[NUM_SC_IMAGES];
    for (int i = 0; i < NUM_SC_IMAGES; i++) {
        off = (off + dkImageLayoutGetAlignment(&swap_layout) - 1)
            & ~(dkImageLayoutGetAlignment(&swap_layout) - 1);
        off_swap[i] = off;
        off += dkImageLayoutGetSize(&swap_layout);
    }

    uint32_t image_mem_size = (off + DK_MEMBLOCK_ALIGNMENT - 1)
                            & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    dkMemBlockMakerDefaults(&mbm, device, image_mem_size);
    mbm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image
              | DkMemBlockFlags_ZeroFillInit;
    DkMemBlock image_mem = dkMemBlockCreate(&mbm);

    DkImage offscreen_img, depth_img, swap_imgs[NUM_SC_IMAGES];
    dkImageInitialize(&offscreen_img, &offscreen_layout, image_mem, off_offscreen);
    dkImageInitialize(&depth_img,     &depth_layout,     image_mem, off_depth);
    for (int i = 0; i < NUM_SC_IMAGES; i++)
        dkImageInitialize(&swap_imgs[i], &swap_layout, image_mem, off_swap[i]);

    /* Swapchain */
    DkImage const* swap_img_array[NUM_SC_IMAGES];
    for (int i = 0; i < NUM_SC_IMAGES; i++)
        swap_img_array[i] = &swap_imgs[i];

    DkSwapchainMaker sm2;
    dkSwapchainMakerDefaults(&sm2, device, nwindowGetDefault(),
                             swap_img_array, NUM_SC_IMAGES);
    DkSwapchain swapchain = dkSwapchainCreate(&sm2);
    nwindowSetSwapInterval(nwindowGetDefault(), 1);

    /* Main loop */
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    float angle = 0.0f;

    while (appletMainLoop()) {
        padUpdate(&pad);
        uint64_t kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_Plus) break;

        angle += 0.02f;

        float proj[16], view[16], rx[16], ry[16], tmp[16], mvp[16];
        mat4_perspective(proj, 1.0472f,
                         (float)RENDER_W / (float)RENDER_H, 0.1f, 100.0f);
        mat4_translate(view, 0.0f, 0.0f, -5.0f);
        mat4_rotate_x(rx, angle * 0.7f);
        mat4_rotate_y(ry, angle);
        mat4_mul(tmp, rx, ry);
        mat4_mul(tmp, view, tmp);
        mat4_mul(mvp, proj, tmp);
        memcpy(data_cpu + off_ubo, mvp, sizeof(mvp));

        int slot = dkQueueAcquireImage(queue, swapchain);

        dkFenceWait(&cmd_fences[cmd_slice], -1);
        dkCmdBufClear(cmdbuf);
        dkCmdBufAddMemory(cmdbuf, cmd_mem,
                          cmd_slice * CMD_SLICE_SIZE, CMD_SLICE_SIZE);

        /* Clear swapchain to black */
        DkImageView swap_view;
        dkImageViewDefaults(&swap_view, &swap_imgs[slot]);
        DkImageView const* swap_rt[] = { &swap_view };
        dkCmdBufBindRenderTargets(cmdbuf, swap_rt, 1, NULL);

        DkViewport sw_vp = { 0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT, 0.0f, 1.0f };
        DkScissor  sw_sc = { 0, 0, FB_WIDTH, FB_HEIGHT };
        dkCmdBufSetViewports(cmdbuf, 0, &sw_vp, 1);
        dkCmdBufSetScissors(cmdbuf,  0, &sw_sc, 1);
        dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

        /* Render cube into offscreen image */
        DkImageView color_view, depth_view;
        dkImageViewDefaults(&color_view, &offscreen_img);
        dkImageViewDefaults(&depth_view, &depth_img);
        DkImageView const* rt[] = { &color_view };
        dkCmdBufBindRenderTargets(cmdbuf, rt, 1, &depth_view);

        DkViewport vp = { 0.0f, 0.0f, (float)RENDER_W, (float)RENDER_H, 0.0f, 1.0f };
        DkScissor  sc = { 0, 0, RENDER_W, RENDER_H };
        dkCmdBufSetViewports(cmdbuf, 0, &vp, 1);
        dkCmdBufSetScissors(cmdbuf,  0, &sc, 1);

        dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.1f, 0.15f, 0.25f, 1.0f);
        dkCmdBufClearDepthStencil(cmdbuf, true, 1.0f, 0xff, 0);

        DkShader const* shaders[] = { &vsh, &fsh };
        dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

        DkRasterizerState raster;     dkRasterizerStateDefaults(&raster);
        DkColorState      cstate;     dkColorStateDefaults(&cstate);
        DkColorWriteState cwstate;    dkColorWriteStateDefaults(&cwstate);
        DkDepthStencilState dsstate;  dkDepthStencilStateDefaults(&dsstate);
        dkCmdBufBindRasterizerState(cmdbuf, &raster);
        dkCmdBufBindColorState(cmdbuf, &cstate);
        dkCmdBufBindColorWriteState(cmdbuf, &cwstate);
        dkCmdBufBindDepthStencilState(cmdbuf, &dsstate);

        DkVtxAttribState attribs[] = {
            { 0, 0, offsetof(vertex_t, pos),   DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
            { 0, 0, offsetof(vertex_t, color), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0 },
        };
        DkVtxBufferState buffers[] = { { sizeof(vertex_t), 0 } };
        dkCmdBufBindVtxAttribState(cmdbuf, attribs, 2);
        dkCmdBufBindVtxBufferState(cmdbuf, buffers, 1);

        dkCmdBufBindVtxBuffer(cmdbuf, 0, data_gpu + off_vb, sizeof(s_cube_verts));
        dkCmdBufBindIdxBuffer(cmdbuf, DkIdxFormat_Uint16, data_gpu + off_ib);

        dkCmdBufBindUniformBuffer(cmdbuf, DkStage_Vertex, 0,
                                  data_gpu + off_ubo, 64);
        dkCmdBufDrawIndexed(cmdbuf, DkPrimitive_Triangles, 36, 1, 0, 0, 0);

        /* Wireframe edges */
        DkShader const* edge_shaders[] = { &fsh_edge };
        dkCmdBufBindShaders(cmdbuf, DkStageFlag_Fragment, edge_shaders, 1);
        dsstate.depthCompareOp = DkCompareOp_Lequal;
        dkCmdBufBindDepthStencilState(cmdbuf, &dsstate);
        dkCmdBufBindIdxBuffer(cmdbuf, DkIdxFormat_Uint16, data_gpu + off_eib);
        dkCmdBufDrawIndexed(cmdbuf, DkPrimitive_Lines, 24, 1, 0, 0, 0);

        /* Orientation markers — identity MVP, no depth test */
        DkShader const* color_shaders[] = { &fsh };
        dkCmdBufBindShaders(cmdbuf, DkStageFlag_Fragment, color_shaders, 1);
        dsstate.depthTestEnable = false;
        dkCmdBufBindDepthStencilState(cmdbuf, &dsstate);
        dkCmdBufBindVtxBuffer(cmdbuf, 0, data_gpu + off_mvb,
                              sizeof(s_marker_verts));
        dkCmdBufBindIdxBuffer(cmdbuf, DkIdxFormat_Uint16,
                              data_gpu + off_mib);
        dkCmdBufBindUniformBuffer(cmdbuf, DkStage_Vertex, 0,
                                  data_gpu + off_ubo_ident, 64);
        dkCmdBufDrawIndexed(cmdbuf, DkPrimitive_Triangles, 24, 1, 0, 0, 0);

        /* Barrier + blit offscreen → swapchain */
        dkCmdBufBarrier(cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);

        DkImageView src_view, dst_view;
        dkImageViewDefaults(&src_view, &offscreen_img);
        dkImageViewDefaults(&dst_view, &swap_imgs[slot]);

        DkImageRect src_rect = { 0, 0, 0, RENDER_W, RENDER_H, 1 };
        const uint32_t scale = FB_HEIGHT / RENDER_H;
        const uint32_t dst_w = RENDER_W * scale;
        const uint32_t dst_h = RENDER_H * scale;
        DkImageRect dst_rect = {
            (FB_WIDTH  - dst_w) / 2,
            (FB_HEIGHT - dst_h) / 2,
            0, dst_w, dst_h, 1
        };
        dkCmdBufBlitImage(cmdbuf, &src_view, &src_rect,
                          &dst_view, &dst_rect,
                          DkBlitFlag_FilterNearest | DkBlitFlag_ModeBlit, 0);

        dkCmdBufSignalFence(cmdbuf, &cmd_fences[cmd_slice], false);

        DkCmdList list = dkCmdBufFinishList(cmdbuf);
        dkQueueSubmitCommands(queue, list);
        dkQueuePresentImage(queue, swapchain, slot);

        cmd_slice = (cmd_slice + 1) % NUM_CMD_SLICES;
    }

    /* Clean exit: present black frames before teardown */
    for (int i = 0; i < NUM_SC_IMAGES; i++) {
        int slot = dkQueueAcquireImage(queue, swapchain);
        dkCmdBufClear(cmdbuf);

        DkImageView swap_view_exit;
        dkImageViewDefaults(&swap_view_exit, &swap_imgs[slot]);
        DkImageView const* exit_rt[] = { &swap_view_exit };
        dkCmdBufBindRenderTargets(cmdbuf, exit_rt, 1, NULL);

        DkViewport ex_vp = { 0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT, 0.0f, 1.0f };
        DkScissor  ex_sc = { 0, 0, FB_WIDTH, FB_HEIGHT };
        dkCmdBufSetViewports(cmdbuf, 0, &ex_vp, 1);
        dkCmdBufSetScissors(cmdbuf,  0, &ex_sc, 1);
        dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

        DkCmdList exit_list = dkCmdBufFinishList(cmdbuf);
        dkQueueSubmitCommands(queue, exit_list);
        dkQueuePresentImage(queue, swapchain, slot);
    }

    dkQueueWaitIdle(queue);
    dkSwapchainDestroy(swapchain);
    nwindowReleaseBuffers(nwindowGetDefault());
    dkMemBlockDestroy(image_mem);
    dkMemBlockDestroy(data_mem);
    dkMemBlockDestroy(code_mem);
    dkCmdBufDestroy(cmdbuf);
    dkMemBlockDestroy(cmd_mem);
    dkQueueDestroy(queue);
    dkDeviceDestroy(device);
    romfsExit();
    return 0;
}
