/*
 * TyrQuake software-video presentation backend for PSL1GHT.
 *
 * Quake renders 8-bit palette indices into main memory.  The PPU expands a
 * completed frame into an XRGB image in RSX local memory, then the RSX copy
 * engine scales that image into a double-buffered display surface.
 */

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "qtypes.h"

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#include "common.h"
#include "console.h"
#include "d_iface.h"
#include "d_local.h"
#include "host.h"
#include "quakedef.h"
#include "render.h"
#include "screen.h"
#include "sys.h"
#include "vid.h"
#include "zone.h"

#define PS3_HOST_SIZE       (32U * 1024U * 1024U)
#define PS3_COMMAND_SIZE    (1U * 1024U * 1024U)
#define PS3_BUFFER_COUNT    2
#define PS3_LABEL_INDEX     255
#define PS3_RENDER_WIDTH    640
#define PS3_RENDER_HEIGHT_4_3 480
#define PS3_RENDER_HEIGHT_16_9 360

typedef struct ps3_display_buffer_s {
    u32 *address;
    u32 offset;
} ps3_display_buffer_t;

unsigned short d_8to16table[256];
unsigned d_8to24table[256];
viddef_t vid;

static gcmContextData *ps3_context;
static void *ps3_host_address;
static ps3_display_buffer_t ps3_display[PS3_BUFFER_COUNT];
static u32 ps3_display_width;
static u32 ps3_display_height;
static u32 ps3_display_pitch;
static u32 ps3_current_buffer;
static qboolean ps3_first_frame = true;

static u32 *ps3_upload_buffer;
static u32 ps3_upload_offset;
static u32 ps3_upload_pitch;

static byte *vid_surfcache;
static int vid_surfcachesize;
static int vid_highhunkmark;
static qboolean vid_buffers_allocated;
static qvidmode_t ps3_mode;
static int ps3_render_height;

static u32 ps3_label_value = 1;

static void
PS3_WaitForIdle(void)
{
    vu32 *label;

    rsxSetWriteBackendLabel(ps3_context, PS3_LABEL_INDEX, ps3_label_value);
    rsxSetWaitLabel(ps3_context, PS3_LABEL_INDEX, ps3_label_value);
    ++ps3_label_value;
    rsxSetWriteBackendLabel(ps3_context, PS3_LABEL_INDEX, ps3_label_value);
    rsxFlushBuffer(ps3_context);

    label = (vu32 *)gcmGetLabelAddress(PS3_LABEL_INDEX);
    while (*label != ps3_label_value)
        usleep(30);

    ++ps3_label_value;
}

static void
PS3_WaitForFlip(void)
{
    if (ps3_first_frame) {
        gcmResetFlipStatus();
        return;
    }

    while (gcmGetFlipStatus() != 0)
        usleep(200);
    gcmResetFlipStatus();
}

static void
PS3_PresentUploadBuffer(qboolean upload_is_safe)
{
    gcmTransferScale scale;
    gcmTransferSurface destination;

    if (!upload_is_safe)
        PS3_WaitForFlip();

    memset(&scale, 0, sizeof(scale));
    scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
    scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
    scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
    scale.clipW = ps3_display_width;
    scale.clipH = ps3_display_height;
    scale.outW = ps3_display_width;
    scale.outH = ps3_display_height;
    scale.ratioX = rsxGetFixedSint32((float)vid.width /
                                    (float)ps3_display_width);
    scale.ratioY = rsxGetFixedSint32((float)vid.height /
                                    (float)ps3_display_height);
    scale.inW = vid.width;
    scale.inH = vid.height;
    scale.pitch = ps3_upload_pitch;
    scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
    scale.interp = GCM_TRANSFER_INTERPOLATOR_NEAREST;
    scale.offset = ps3_upload_offset;

    memset(&destination, 0, sizeof(destination));
    destination.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
    destination.pitch = ps3_display_pitch;
    destination.offset = ps3_display[ps3_current_buffer].offset;

    rsxSetTransferScaleMode(ps3_context, GCM_TRANSFER_LOCAL_TO_LOCAL,
                            GCM_TRANSFER_SURFACE);
    rsxSetTransferScaleSurface(ps3_context, &scale, &destination);

    if (gcmSetFlip(ps3_context, ps3_current_buffer) != 0)
        Sys_Error("VID: gcmSetFlip failed");
    rsxFlushBuffer(ps3_context);
    gcmSetWaitFlip(ps3_context);

    ps3_current_buffer ^= 1;
    ps3_first_frame = false;
}

static void
PS3_ConvertFrame(void)
{
    int x, y;
    const byte *source;
    u32 *destination;

    source = vid.buffer;
    destination = ps3_upload_buffer;
    for (y = 0; y < vid.height; ++y) {
        for (x = 0; x < vid.width; ++x)
            destination[x] = d_8to24table[source[x]];
        source += vid.rowbytes;
        destination += ps3_upload_pitch / sizeof(*destination);
    }
}

static qboolean
PS3_AllocEngineBuffers(int width, int height)
{
    int total_size;

    vid_surfcachesize = D_SurfaceCacheForRes(width, height);
    total_size = width * height * sizeof(*d_pzbuffer) + vid_surfcachesize;

    if (!VID_CheckAdequateMem(width, height)) {
        Con_SafePrintf("Not enough memory for PS3 video mode\n");
        return false;
    }

    if (vid_buffers_allocated) {
        D_FlushCaches();
        Hunk_FreeToHighMark(vid_highhunkmark);
        d_pzbuffer = NULL;
        r_warpbuffer = NULL;
        vid.buffer = vid.conbuffer = vid.direct = NULL;
    }

    vid_highhunkmark = Hunk_HighMark();
    d_pzbuffer = Hunk_HighAllocName(total_size, "video");
    vid_surfcache = (byte *)d_pzbuffer +
                    width * height * sizeof(*d_pzbuffer);
    r_warpbuffer = Hunk_HighAllocName(width * height, "warpbuf");
    vid.buffer = vid.conbuffer = vid.direct =
        Hunk_HighAllocName(width * height, "vidbuf");
    vid.rowbytes = vid.conrowbytes = width;
    vid_buffers_allocated = true;

    R_AllocSurfEdges(false);
    D_InitCaches(vid_surfcache, vid_surfcachesize);
    return true;
}

static void
PS3_FreeUploadBuffer(void)
{
    if (ps3_upload_buffer) {
        rsxFree(ps3_upload_buffer);
        ps3_upload_buffer = NULL;
        ps3_upload_offset = 0;
        ps3_upload_pitch = 0;
    }
}

static qboolean
PS3_AllocUploadBuffer(int width, int height)
{
    PS3_FreeUploadBuffer();
    ps3_upload_pitch = width * sizeof(*ps3_upload_buffer);
    ps3_upload_buffer = rsxMemalign(64, ps3_upload_pitch * height);
    if (!ps3_upload_buffer)
        return false;
    if (rsxAddressToOffset(ps3_upload_buffer, &ps3_upload_offset) != 0) {
        PS3_FreeUploadBuffer();
        return false;
    }
    return true;
}

static qboolean
PS3_InitDisplay(videoState *state)
{
    videoConfiguration configuration;
    videoResolution resolution;
    int i;

    ps3_host_address = memalign(1024 * 1024, PS3_HOST_SIZE);
    if (!ps3_host_address)
        return false;
    if (rsxInit(&ps3_context, PS3_COMMAND_SIZE, PS3_HOST_SIZE,
                ps3_host_address) != 0 || !ps3_context)
        return false;
    if (videoGetState(VIDEO_PRIMARY, 0, state) != 0 ||
        videoGetResolution(state->displayMode.resolution, &resolution) != 0)
        return false;
    /* The installed SDK macro is inverted relative to the ABI: zero is on. */
    if (state->state != 0)
        return false;

    memset(&configuration, 0, sizeof(configuration));
    configuration.resolution = state->displayMode.resolution;
    configuration.format = VIDEO_BUFFER_FORMAT_XRGB;
    configuration.pitch = resolution.width * sizeof(u32);
    configuration.aspect = state->displayMode.aspect;

    PS3_WaitForIdle();
    if (videoConfigure(VIDEO_PRIMARY, &configuration, NULL, 0) != 0)
        return false;

    ps3_display_width = resolution.width;
    ps3_display_height = resolution.height;
    ps3_display_pitch = ps3_display_width * sizeof(u32);
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    for (i = 0; i < PS3_BUFFER_COUNT; ++i) {
        ps3_display[i].address =
            rsxMemalign(64, ps3_display_pitch * ps3_display_height);
        if (!ps3_display[i].address ||
            rsxAddressToOffset(ps3_display[i].address,
                               &ps3_display[i].offset) != 0 ||
            gcmSetDisplayBuffer(i, ps3_display[i].offset, ps3_display_pitch,
                                ps3_display_width, ps3_display_height) != 0)
            return false;
    }

    gcmResetFlipStatus();
    return true;
}

void
VID_GetDesktopRect(vrect_t *rect)
{
    if (!rect)
        return;
    rect->x = 0;
    rect->y = 0;
    rect->width = ps3_display_width;
    rect->height = ps3_display_height;
    rect->pnext = NULL;
}

void
VID_Shutdown(void)
{
    int i;

    if (ps3_context)
        rsxFinish(ps3_context, 1);

    PS3_FreeUploadBuffer();
    for (i = 0; i < PS3_BUFFER_COUNT; ++i) {
        if (ps3_display[i].address) {
            rsxFree(ps3_display[i].address);
            ps3_display[i].address = NULL;
        }
    }

    ps3_context = NULL;
    free(ps3_host_address);
    ps3_host_address = NULL;
}

void
VID_ShiftPalette(const byte *palette)
{
    VID_SetPalette(palette);
}

void
VID_SetDefaultMode(void)
{
}

qboolean
window_visible(void)
{
    return true;
}

qboolean
VID_CheckAdequateMem(int width, int height)
{
    int buffer_size;

    buffer_size = width * height * sizeof(*d_pzbuffer);
    buffer_size += D_SurfaceCacheForRes(width, height);
    return (host_parms.memsize - buffer_size + SURFCACHE_SIZE_AT_320X200 +
            0x10000 * 3) >= minimum_memory;
}

void
VID_InitColormap(const byte *palette)
{
    (void)palette;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
}

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    if (mode != &ps3_mode || !ps3_context)
        return false;

    /* The PS3 exposes one fixed fullscreen mode; undo menu-side mutations. */
    ps3_mode.width = ps3_display_width;
    ps3_mode.height = ps3_display_height;
    ps3_mode.bpp = 32;
    ps3_mode.refresh = 60;
    ps3_mode.min_scale = 1;
    ps3_mode.resolution.scale = 0;
    ps3_mode.resolution.width = PS3_RENDER_WIDTH;
    ps3_mode.resolution.height = ps3_render_height;
    VID_Mode_SetupViddef(mode, &vid);
    if (!vid_buffers_allocated) {
        if (!PS3_AllocEngineBuffers(vid.width, vid.height) ||
            !PS3_AllocUploadBuffer(vid.width, vid.height))
            return false;
    }

    vid.numpages = 1;
    vid.aspect = 1.0f;
    vid.stretchblit = true;
    vid.colormap16 = NULL;
    r_pixbytes = 1;
    VID_SetPalette(palette);
    VID_InitColormap(palette);

    vid_currentmode = mode;
    vid.recalc_refdef = 1;
    SCR_CheckResize();
    Con_CheckResize();
    Cvar_SetValue("vid_fullscreen", 1);
    Cvar_SetValue("vid_width", mode->width);
    Cvar_SetValue("vid_height", mode->height);
    Cvar_SetValue("vid_bpp", mode->bpp);
    Cvar_SetValue("vid_refreshrate", mode->refresh);
    Cvar_SetValue("vid_render_resolution_scale", mode->resolution.scale);
    Cvar_SetValue("vid_render_resolution_width", mode->resolution.width);
    Cvar_SetValue("vid_render_resolution_height", mode->resolution.height);
    return true;
}

void
VID_SetPalette(const byte *palette)
{
    int i;

    if (!palette)
        return;
    for (i = 0; i < 256; ++i) {
        d_8to24table[i] = 0xff000000U | ((u32)palette[0] << 16) |
                         ((u32)palette[1] << 8) | palette[2];
        d_8to16table[i] = ((palette[0] >> 3) << 11) |
                         ((palette[1] >> 2) << 5) | (palette[2] >> 3);
        palette += 3;
    }
}

void
VID_ProcessEvents(void)
{
}

void
VID_RegisterVariables(void)
{
}

void
VID_AddCommands(void)
{
}

void
VID_Init(const byte *palette)
{
    videoState state;
    int render_height;

    if (!PS3_InitDisplay(&state))
        Sys_Error("VID: unable to initialize the PS3 display");

    render_height = (state.displayMode.aspect == VIDEO_ASPECT_4_3)
                    ? PS3_RENDER_HEIGHT_4_3 : PS3_RENDER_HEIGHT_16_9;
    ps3_render_height = render_height;

    memset(&ps3_mode, 0, sizeof(ps3_mode));
    ps3_mode.width = ps3_display_width;
    ps3_mode.height = ps3_display_height;
    ps3_mode.bpp = 32;
    ps3_mode.refresh = 60;
    ps3_mode.min_scale = 1;
    ps3_mode.resolution.width = PS3_RENDER_WIDTH;
    ps3_mode.resolution.height = render_height;
    vid_windowed_mode = ps3_mode;
    vid_modelist = &ps3_mode;
    vid_nummodes = 1;

    if (!VID_SetMode(&ps3_mode, palette))
        Sys_Error("VID: unable to allocate the %dx%d video mode",
                  PS3_RENDER_WIDTH, render_height);

    vsync_available = true;
    adaptive_vsync_available = false;
    vid_menudrawfn = VID_MenuDraw;
    vid_menukeyfn = VID_MenuKey;
    Sys_Printf("PS3 video: %ux%u display, %dx%d indexed render buffer\n",
               ps3_display_width, ps3_display_height, vid.width, vid.height);

}

void
VID_Update(vrect_t *rects)
{
    (void)rects;

    PS3_WaitForFlip();
    PS3_ConvertFrame();
    PS3_PresentUploadBuffer(true);
}

void
D_BeginDirectRect(int x, int y, const byte *bitmap, int width, int height)
{
    (void)x;
    (void)y;
    (void)bitmap;
    (void)width;
    (void)height;
}

void
D_EndDirectRect(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void
VID_LockBuffer(void)
{
}

void
VID_UnlockBuffer(void)
{
}
