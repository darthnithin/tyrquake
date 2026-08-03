/* DualShock/Sixaxis input backend for PSL1GHT. */

#include <math.h>
#include <string.h>

#include "qtypes.h"

#include <io/pad.h>

#include "client.h"
#include "host.h"
#include "input.h"
#include "keys.h"
#include "menu.h"
#include "quakedef.h"
#include "sys.h"

#define PS3_STICK_CENTER 128.0f
#define PS3_STICK_RANGE  127.0f
#define PS3_STICK_DEADZONE 0.18f
#define PS3_MENU_STICK_THRESHOLD 0.55f

typedef enum ps3_button_e {
    PS3_BUTTON_LEFT,
    PS3_BUTTON_RIGHT,
    PS3_BUTTON_UP,
    PS3_BUTTON_DOWN,
    PS3_BUTTON_START,
    PS3_BUTTON_SELECT,
    PS3_BUTTON_CROSS,
    PS3_BUTTON_SQUARE,
    PS3_BUTTON_TRIANGLE,
    PS3_BUTTON_R1,
    PS3_BUTTON_R2,
    PS3_BUTTON_L2,
    PS3_BUTTON_L3,
    PS3_BUTTON_R3,
    PS3_BUTTON_COUNT
} ps3_button_t;

cvar_t in_snd_block = { "in_snd_block", "0" };
cvar_t _windowed_mouse = { "_windowed_mouse", "0", CVAR_CONFIG };

static qboolean ps3_input_focus = true;
static qboolean ps3_pad_initialized;
static qboolean ps3_waiting_for_pad;
static qboolean ps3_waiting_for_neutral;
static qboolean ps3_connection_unreported;
static int ps3_pad_port = -1;
static padData ps3_pad_data;
static qboolean ps3_button_down[PS3_BUTTON_COUNT];
static knum_t ps3_button_key[PS3_BUTTON_COUNT];

static void
PS3_ResetPadData(void)
{
    memset(&ps3_pad_data, 0, sizeof(ps3_pad_data));
    ps3_pad_data.ANA_L_H = PS3_STICK_CENTER;
    ps3_pad_data.ANA_L_V = PS3_STICK_CENTER;
    ps3_pad_data.ANA_R_H = PS3_STICK_CENTER;
    ps3_pad_data.ANA_R_V = PS3_STICK_CENTER;
}

static float
PS3_NormalizeAxis(unsigned value)
{
    float axis = ((float)value - PS3_STICK_CENTER) / PS3_STICK_RANGE;
    float magnitude;

    axis = qclamp(axis, -1.0f, 1.0f);
    magnitude = fabsf(axis);
    if (magnitude <= PS3_STICK_DEADZONE)
        return 0.0f;

    magnitude = (magnitude - PS3_STICK_DEADZONE) /
                (1.0f - PS3_STICK_DEADZONE);
    return (axis < 0.0f) ? -magnitude : magnitude;
}

static qboolean
PS3_PadIsNeutral(void)
{
    if (ps3_pad_data.BTN_LEFT || ps3_pad_data.BTN_RIGHT ||
        ps3_pad_data.BTN_UP || ps3_pad_data.BTN_DOWN ||
        ps3_pad_data.BTN_START || ps3_pad_data.BTN_SELECT ||
        ps3_pad_data.BTN_CROSS || ps3_pad_data.BTN_SQUARE ||
        ps3_pad_data.BTN_CIRCLE || ps3_pad_data.BTN_TRIANGLE ||
        ps3_pad_data.BTN_R1 || ps3_pad_data.BTN_R2 ||
        ps3_pad_data.BTN_L1 || ps3_pad_data.BTN_L2 ||
        ps3_pad_data.BTN_L3 || ps3_pad_data.BTN_R3)
        return false;

    return PS3_NormalizeAxis(ps3_pad_data.ANA_L_H) == 0.0f &&
           PS3_NormalizeAxis(ps3_pad_data.ANA_L_V) == 0.0f &&
           PS3_NormalizeAxis(ps3_pad_data.ANA_R_H) == 0.0f &&
           PS3_NormalizeAxis(ps3_pad_data.ANA_R_V) == 0.0f;
}

static void
PS3_UpdateButton(ps3_button_t button, qboolean pressed, knum_t key)
{
    if (pressed && !ps3_button_down[button]) {
        ps3_button_down[button] = true;
        ps3_button_key[button] = key;
        Key_Event(key, true);
    } else if (!pressed && ps3_button_down[button]) {
        Key_Event(ps3_button_key[button], false);
        ps3_button_down[button] = false;
        ps3_button_key[button] = K_UNKNOWN;
    }
}

static void
PS3_ReleaseButtons(void)
{
    int i;

    for (i = 0; i < PS3_BUTTON_COUNT; ++i) {
        if (ps3_button_down[i])
            Key_Event(ps3_button_key[i], false);
        ps3_button_down[i] = false;
        ps3_button_key[i] = K_UNKNOWN;
    }
}

static void
PS3_UpdateButtons(void)
{
    float left_x = PS3_NormalizeAxis(ps3_pad_data.ANA_L_H);
    float left_y = PS3_NormalizeAxis(ps3_pad_data.ANA_L_V);
    qboolean menu = key_dest == key_menu;
    qboolean confirm_quit = false;

#ifdef NQ_HACK
    confirm_quit = menu && m_state == m_quit;
#endif

    PS3_UpdateButton(PS3_BUTTON_LEFT,
                     ps3_pad_data.BTN_LEFT ||
                     (menu && left_x < -PS3_MENU_STICK_THRESHOLD),
                     K_LEFTARROW);
    PS3_UpdateButton(PS3_BUTTON_RIGHT,
                     ps3_pad_data.BTN_RIGHT ||
                     (menu && left_x > PS3_MENU_STICK_THRESHOLD),
                     K_RIGHTARROW);
    PS3_UpdateButton(PS3_BUTTON_UP,
                     ps3_pad_data.BTN_UP ||
                     (menu && left_y < -PS3_MENU_STICK_THRESHOLD),
                     K_UPARROW);
    PS3_UpdateButton(PS3_BUTTON_DOWN,
                     ps3_pad_data.BTN_DOWN ||
                     (menu && left_y > PS3_MENU_STICK_THRESHOLD),
                     K_DOWNARROW);

    /* These use Quake's stock default.cfg keyboard/mouse bindings. */
    PS3_UpdateButton(PS3_BUTTON_START,
                     ps3_pad_data.BTN_START || ps3_pad_data.BTN_CIRCLE,
                     K_ESCAPE);
    PS3_UpdateButton(PS3_BUTTON_SELECT, ps3_pad_data.BTN_SELECT, K_TAB);
    PS3_UpdateButton(PS3_BUTTON_CROSS, ps3_pad_data.BTN_CROSS,
                     (key_count < 0 || confirm_quit) ? K_y : K_ENTER);
    PS3_UpdateButton(PS3_BUTTON_SQUARE, ps3_pad_data.BTN_SQUARE, K_SPACE);
    PS3_UpdateButton(PS3_BUTTON_TRIANGLE, ps3_pad_data.BTN_TRIANGLE,
                     K_SLASH);
    PS3_UpdateButton(PS3_BUTTON_R1, ps3_pad_data.BTN_R1, K_MOUSE1);
    PS3_UpdateButton(PS3_BUTTON_R2, ps3_pad_data.BTN_R2, K_LCTRL);
    PS3_UpdateButton(PS3_BUTTON_L2, ps3_pad_data.BTN_L2, K_LALT);
    PS3_UpdateButton(PS3_BUTTON_L3, ps3_pad_data.BTN_L3, K_END);
    PS3_UpdateButton(PS3_BUTTON_R3, ps3_pad_data.BTN_R3, K_BACKSLASH);
}

static void
PS3_HandleDisconnect(void)
{
    qboolean had_pad = ps3_pad_port >= 0;

    PS3_ReleaseButtons();
    PS3_ResetPadData();
    ps3_pad_port = -1;
    ps3_waiting_for_neutral = true;
    ps3_connection_unreported = false;

    if (!ps3_waiting_for_pad) {
        ps3_waiting_for_pad = true;
        if (had_pad) {
            Sys_Printf("PS3 input: controller disconnected; waiting for a pad\n");
            if (key_dest == key_game) {
                Key_Event(K_ESCAPE, true);
                Key_Event(K_ESCAPE, false);
            }
        } else {
            Sys_Printf("PS3 input: waiting for a controller\n");
        }
    }
}

static void
PS3_PollPad(void)
{
    padInfo2 info;
    padData data;
    int port;

    if (!ps3_pad_initialized || ioPadGetInfo2(&info) != PAD_OK) {
        PS3_HandleDisconnect();
        return;
    }

    if (info.info & 1U) {
        if (ps3_pad_port >= 0)
            ioPadClearBuf(ps3_pad_port);
        PS3_HandleDisconnect();
        return;
    }

    if (ps3_pad_port >= 0) {
        unsigned status = info.port_status[ps3_pad_port];

        if (!(status & 1U) || (status & 2U)) {
            if (status & 1U)
                ioPadClearBuf(ps3_pad_port);
            PS3_HandleDisconnect();
            return;
        }
        port = ps3_pad_port;
    } else {
        for (port = 0; port < MAX_PORT_NUM; ++port) {
            if (info.port_status[port] & 1U)
                break;
        }
    }
    if (port == MAX_PORT_NUM) {
        PS3_HandleDisconnect();
        return;
    }

    if (port != ps3_pad_port) {
        PS3_ReleaseButtons();
        PS3_ResetPadData();
        ps3_pad_port = port;
        ioPadClearBuf(port);
        ps3_waiting_for_neutral = true;
        ps3_connection_unreported = true;
    }

    memset(&data, 0, sizeof(data));
    if (ioPadGetData(port, &data) != PAD_OK) {
        PS3_HandleDisconnect();
        return;
    }

    /* len==0 means unchanged, not released; retain the cached snapshot. */
    if (data.len > 0)
        ps3_pad_data = data;

    if (ps3_waiting_for_neutral) {
        if (!PS3_PadIsNeutral())
            return;
        ps3_waiting_for_neutral = false;
    }

    if (ps3_connection_unreported) {
        ps3_connection_unreported = false;
        ps3_waiting_for_pad = false;
        Sys_Printf("PS3 input: controller connected on port %d\n", port + 1);
    }
    PS3_UpdateButtons();
}

void
IN_SetFocus(qboolean focus)
{
    ps3_input_focus = focus;
    if (!focus) {
        PS3_ReleaseButtons();
        PS3_ResetPadData();
        if (ps3_pad_port >= 0)
            ioPadClearBuf(ps3_pad_port);
        ps3_pad_port = -1;
        ps3_waiting_for_pad = true;
        ps3_waiting_for_neutral = true;
        ps3_connection_unreported = false;
    }
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
    if (ioPadInit(MAX_PORT_NUM) != PAD_OK)
        Sys_Error("IN_Init: ioPadInit failed");

    ps3_pad_initialized = true;
    ps3_input_focus = true;
    ps3_waiting_for_pad = false;
    ps3_waiting_for_neutral = true;
    ps3_connection_unreported = false;
    ps3_pad_port = -1;
    memset(ps3_button_down, 0, sizeof(ps3_button_down));
    memset(ps3_button_key, 0, sizeof(ps3_button_key));
    PS3_ResetPadData();
    Sys_Printf("PS3 input: libpad initialized\n");
}

void
IN_Shutdown(void)
{
    PS3_ReleaseButtons();
    if (ps3_pad_initialized) {
        ioPadEnd();
        ps3_pad_initialized = false;
    }
    PS3_ResetPadData();
    ps3_pad_port = -1;
    ps3_waiting_for_pad = false;
    ps3_waiting_for_neutral = true;
    ps3_connection_unreported = false;
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
    float left_x, left_y, right_x, right_y;
    float move_scale, look_scale, pitch_sign;

    if (!cmd || !ps3_input_focus || !ps3_pad_initialized ||
        ps3_pad_port < 0 || ps3_waiting_for_neutral ||
        key_dest != key_game)
        return;

    left_x = PS3_NormalizeAxis(ps3_pad_data.ANA_L_H);
    left_y = PS3_NormalizeAxis(ps3_pad_data.ANA_L_V);
    right_x = PS3_NormalizeAxis(ps3_pad_data.ANA_R_H);
    right_y = PS3_NormalizeAxis(ps3_pad_data.ANA_R_V);

    move_scale = ((((in_speed.state & 1) != 0) || ps3_pad_data.BTN_L1) ^
                  ((int)cl_run.value != 0))
                 ? cl_movespeedkey.value : 1.0f;
    cmd->sidemove += left_x * move_scale * cl_sidespeed.value;
    cmd->forwardmove -= left_y * move_scale *
                        ((left_y < 0.0f) ? cl_forwardspeed.value
                                        : cl_backspeed.value);

    look_scale = sensitivity.value / 3.0f;
    pitch_sign = (m_pitch.value < 0.0f) ? -1.0f : 1.0f;
    if (right_x != 0.0f)
        cl.viewangles[YAW] -= right_x * host_frametime *
                              cl_yawspeed.value * look_scale;
    if (right_y != 0.0f) {
        cl.viewangles[PITCH] += right_y * host_frametime *
                                cl_pitchspeed.value * look_scale * pitch_sign;
    }
    if (right_x != 0.0f || right_y != 0.0f)
        V_StopPitchDrift();
    cl.viewangles[YAW] = anglemod(cl.viewangles[YAW]);
    cl.viewangles[PITCH] = qclamp(cl.viewangles[PITCH],
                                 cl_minpitch.value, cl_maxpitch.value);
}

void
IN_Commands(void)
{
}

void
IN_PS3_Poll(void)
{
    if (ps3_pad_initialized && ps3_input_focus)
        PS3_PollPad();
}
