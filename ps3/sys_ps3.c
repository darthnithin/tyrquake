/*
 * Minimal PS3 system backend used to prove the TyrQuake engine cross-links.
 * Hardware-aware timing, paths, and logging are implemented in Task 6.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "quakedef.h"
#include "sys.h"

qboolean isDedicated = false;

const char *
Sys_UserDataDirectory(void)
{
    return NULL;
}

void
Sys_Printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

void
Sys_Quit(void)
{
    exit(0);
}

void
Sys_RegisterVariables(void)
{
}

void
Sys_Init(void)
{
}

void
Sys_Error(const char *error, ...)
{
    va_list args;

    fputs("Sys_Error: ", stderr);
    va_start(args, error);
    vfprintf(stderr, error, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
    exit(1);
}

int64_t
Sys_FileTime(const char *path)
{
    (void)path;
    return -1;
}

void
Sys_mkdir(const char *path)
{
    (void)path;
}

double
Sys_DoubleTime(void)
{
    return 0.0;
}

char *
Sys_ConsoleInput(void)
{
    return NULL;
}

void
Sys_Sleep(void)
{
}

void
Sys_DebugLog(const char *file, const char *fmt, ...)
{
    (void)file;
    (void)fmt;
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
    (void)argc;
    (void)argv;

    Sys_Printf("TyrQuake PS3 stub backend linked successfully\n");
    return 0;
}
