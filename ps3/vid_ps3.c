/* Minimal link-only video backend. Real RSX presentation is Task 8. */

#include "d_iface.h"
#include "quakedef.h"
#include "sys.h"
#include "vid.h"

unsigned short d_8to16table[256];
unsigned d_8to24table[256];
viddef_t vid;

void
VID_GetDesktopRect(vrect_t *rect)
{
    if (rect) {
        rect->x = 0;
        rect->y = 0;
        rect->width = 0;
        rect->height = 0;
        rect->pnext = NULL;
    }
}

void
VID_Shutdown(void)
{
}

void
VID_ShiftPalette(const byte *palette)
{
    (void)palette;
}

void
VID_SetDefaultMode(void)
{
}

qboolean
window_visible(void)
{
    return false;
}

qboolean
VID_CheckAdequateMem(int width, int height)
{
    (void)width;
    (void)height;
    return false;
}

void
VID_InitColormap(const byte *palette)
{
    (void)palette;
}

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    (void)mode;
    (void)palette;
    return false;
}

void
VID_SetPalette(const byte *palette)
{
    (void)palette;
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
    (void)palette;
    Sys_Error("VID_Init: PS3 video backend is still a stub");
}

void
VID_Update(vrect_t *rects)
{
    (void)rects;
}

void
D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
    (void)x;
    (void)y;
    (void)pbitmap;
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
