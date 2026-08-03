/* Minimal link-only input backend. Real libpad polling is Task 9. */

#include "input.h"
#include "quakedef.h"

cvar_t in_snd_block = { "in_snd_block", "0" };
cvar_t _windowed_mouse = { "_windowed_mouse", "0", CVAR_CONFIG };

static qboolean ps3_input_focus = true;

void
IN_SetFocus(qboolean focus)
{
    ps3_input_focus = focus;
}

qboolean
IN_HaveFocus(void)
{
    return ps3_input_focus;
}

void
IN_AddCommands(void)
{
}

void
IN_RegisterVariables(void)
{
    Cvar_RegisterVariable(&in_snd_block);
    Cvar_RegisterVariable(&_windowed_mouse);
}

void
IN_Init(void)
{
}

void
IN_Shutdown(void)
{
}

void
IN_Accumulate(void)
{
}

void
IN_UpdateClipCursor(void)
{
}

void
IN_Move(usercmd_t *cmd)
{
    (void)cmd;
}

void
IN_Commands(void)
{
}
