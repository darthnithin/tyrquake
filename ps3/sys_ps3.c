/*
 * TyrQuake system backend for PSL1GHT.
 *
 * RPCS3 maps a directly booted SELF's directory to /app_home. Packaged
 * homebrew passes an executable path under /dev_hdd0/game instead.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "qtypes.h"

#include <sys/process.h>
#include <sys/systime.h>
#include <sys/tty.h>

#include "buildinfo.h"
#include "common.h"
#include "host.h"
#include "quakedef.h"
#include "sys.h"
#include "zone.h"

#define PS3_DEVELOPMENT_BASEDIR "/app_home"
#define PS3_DEFAULT_HEAP_SIZE ((size_t)64 << 20)

SYS_PROCESS_PARAM(1001, 0x100000);

qboolean isDedicated = false;

static qboolean ps3_clock_initialized = false;
static int64_t ps3_clock_base_us;
static char ps3_basedir[MAX_OSPATH];

static void
PS3_TtyWrite(int channel, const char *text)
{
    const char *cursor = text;
    u32 remaining = (u32)strlen(text);

    while (remaining > 0) {
        u32 written = 0;

        if (sysTtyWrite(channel, cursor, remaining, &written) != 0 ||
            written == 0)
            break;

        cursor += written;
        remaining -= written;
    }
}

static const char *
PS3_BaseDirectory(const char *executable)
{
    char *separator;

    if (!executable || executable[0] != '/' ||
        strlen(executable) >= sizeof(ps3_basedir))
        return PS3_DEVELOPMENT_BASEDIR;

    qstrncpy(ps3_basedir, executable, sizeof(ps3_basedir));
    separator = strrchr(ps3_basedir, '/');
    if (!separator)
        return PS3_DEVELOPMENT_BASEDIR;

    if (separator == ps3_basedir)
        separator[1] = '\0';
    else
        *separator = '\0';

    return ps3_basedir;
}

static size_t
PS3_EngineHeapSize(void)
{
    if (COM_CheckParm("-mem") || COM_CheckParm("-heapsize"))
        return Memory_GetSize();

    return PS3_DEFAULT_HEAP_SIZE;
}

const char *
Sys_UserDataDirectory(void)
{
    /* Save files live in the active game directory on this platform. */
    return NULL;
}

void
Sys_Printf(const char *fmt, ...)
{
    va_list args;
    char text[MAX_PRINTMSG];

    va_start(args, fmt);
    qvsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    PS3_TtyWrite(1, text);
}

void
Sys_Quit(void)
{
    Host_Shutdown();
    exit(0);
}

void
Sys_RegisterVariables(void)
{
}

void
Sys_Init(void)
{
    ps3_clock_base_us = sysGetSystemTime();
    ps3_clock_initialized = true;
}

void
Sys_Error(const char *error, ...)
{
    va_list args;
    char detail[MAX_PRINTMSG];
    char text[MAX_PRINTMSG];

    va_start(args, error);
    qvsnprintf(detail, sizeof(detail), error, args);
    va_end(args);

    qsnprintf(text, sizeof(text), "Sys_Error: %s\n", detail);
    PS3_TtyWrite(2, text);

    Host_Shutdown();
    exit(1);
}

int64_t
Sys_FileTime(const char *path)
{
    struct stat info;

    if (stat(path, &info) == -1)
        return -1;

    return info.st_mtime;
}

void
Sys_mkdir(const char *path)
{
    if (mkdir(path, 0777) == -1 && errno != EEXIST)
        Sys_Error("mkdir %s failed (errno %d)", path, errno);
}

double
Sys_DoubleTime(void)
{
    int64_t now = sysGetSystemTime();

    if (!ps3_clock_initialized) {
        ps3_clock_base_us = now;
        ps3_clock_initialized = true;
    }

    return (now - ps3_clock_base_us) / 1000000.0;
}

char *
Sys_ConsoleInput(void)
{
    return NULL;
}

void
Sys_Sleep(void)
{
    sysUsleep(1000);
}

void
Sys_DebugLog(const char *file, const char *fmt, ...)
{
    va_list args;
    char text[MAX_PRINTMSG];
    FILE *stream;

    va_start(args, fmt);
    qvsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    stream = fopen(file, "ab");
    if (!stream)
        return;

    fwrite(text, 1, strlen(text), stream);
    fclose(stream);
}

void
Sys_HighFPPrecision(void)
{
}

void
Sys_LowFPPrecision(void)
{
}

void
Sys_SetFPCW(void)
{
}

void
Sys_MakeCodeWriteable(void *start_addr, void *end_addr)
{
    (void)start_addr;
    (void)end_addr;
}

void
Sys_MakeCodeUnwriteable(void *start_addr, void *end_addr)
{
    (void)start_addr;
    (void)end_addr;
}

void
Sys_SendKeyEvents(void)
{
}

int
main(int argc, char **argv)
{
    double frame_time;
    double oldtime;
    double newtime;
    quakeparms_t parms;

    memset(&parms, 0, sizeof(parms));

    COM_InitArgv(argc, (const char **)argv);
    isDedicated = (COM_CheckParm("-dedicated") != 0);

    parms.argc = com_argc;
    parms.argv = com_argv;
    parms.basedir = PS3_BaseDirectory(argc > 0 ? argv[0] : NULL);
    parms.memsize = (int)PS3_EngineHeapSize();
    parms.membase = malloc(parms.memsize);
    if (!parms.membase)
        Sys_Error("Allocation of %d byte heap failed", parms.memsize);

    Sys_Init();
    Sys_Printf("Quake -- TyrQuake Version %s\n", build_version);
    Sys_Printf("PS3: data path %s, heap %.1f MiB\n", parms.basedir,
               parms.memsize / (1024.0 * 1024.0));

    Host_Init(&parms, NULL);

    oldtime = Sys_DoubleTime() - 0.1;
    while (1) {
        newtime = Sys_DoubleTime();
        frame_time = newtime - oldtime;

        if (cls.state == ca_dedicated) {
            if (frame_time < sys_ticrate.value) {
                Sys_Sleep();
                continue;
            }
            frame_time = sys_ticrate.value;
        }

        if (frame_time > sys_ticrate.value * 2)
            oldtime = newtime;
        else
            oldtime += frame_time;

        Host_Frame(frame_time);
    }
}
