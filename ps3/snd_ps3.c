/* Minimal link-only sound backend. Real libaudio submission is Task 10. */

#include "sound.h"

qboolean
SNDDMA_Init(void)
{
    return false;
}

int
SNDDMA_GetDMAPos(void)
{
    return 0;
}

void
SNDDMA_Shutdown(void)
{
}

void
SNDDMA_Submit(void)
{
}

int
SNDDMA_LockBuffer(void)
{
    return 0;
}

void
SNDDMA_UnlockBuffer(void)
{
}

void
S_BlockSound(void)
{
    snd_blocked++;
}

void
S_UnblockSound(void)
{
    if (snd_blocked > 0)
        snd_blocked--;
}
