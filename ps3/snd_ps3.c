/* TyrQuake software mixer output through PSL1GHT's libaudio. */

#include "sound.h"

#include <stdint.h>
#include <string.h>

#include <audio/audio.h>
#include <sys/mutex.h>
#include <sys/thread.h>

#include "console.h"

#define PS3_AUDIO_CHANNELS       2
#define PS3_AUDIO_BLOCKS         AUDIO_BLOCK_16
#define PS3_AUDIO_FRAMES_BLOCK   AUDIO_BLOCK_SAMPLES
#define PS3_AUDIO_SAMPLES_BLOCK  (PS3_AUDIO_FRAMES_BLOCK * PS3_AUDIO_CHANNELS)
#define PS3_AUDIO_SAMPLES        (PS3_AUDIO_BLOCKS * PS3_AUDIO_SAMPLES_BLOCK)
#define PS3_AUDIO_RATE           48000
#define PS3_AUDIO_THREAD_STACK   (16 * 1024)
#define PS3_AUDIO_THREAD_PRIO    1000
#define PS3_AUDIO_TIMEOUT        ((s32)0x8001000b)

static dma_t ps3_dma;
static s16 ps3_mix_buffer[PS3_AUDIO_SAMPLES] __attribute__((aligned(128)));
static audioPortConfig ps3_audio_config;
static u32 ps3_audio_port;
static sys_event_queue_t ps3_audio_queue;
static sys_ipc_key_t ps3_audio_queue_key;
static sys_mutex_t ps3_audio_mutex;
static sys_ppu_thread_t ps3_audio_thread;

static qboolean ps3_audio_initialized;
static qboolean ps3_audio_port_open;
static qboolean ps3_audio_queue_created;
static qboolean ps3_audio_queue_set;
static qboolean ps3_audio_mutex_created;
static qboolean ps3_audio_thread_created;
static qboolean ps3_audio_port_started;
static qboolean ps3_audio_buffer_locked;
static volatile qboolean ps3_audio_thread_running;
static volatile s32 ps3_audio_async_error;
static qboolean ps3_audio_async_reported;

static f32 *
PS3_AudioOutput(void)
{
    return (f32 *)(uintptr_t)ps3_audio_config.audioDataStart;
}

static void
PS3_ClearAudioBuffers(void)
{
    size_t output_samples;

    memset(ps3_mix_buffer, 0, sizeof(ps3_mix_buffer));
    if (!ps3_audio_port_open || !ps3_audio_config.audioDataStart)
        return;

    output_samples = ps3_audio_config.channelCount *
                     ps3_audio_config.numBlocks * AUDIO_BLOCK_SAMPLES;
    memset(PS3_AudioOutput(), 0, output_samples * sizeof(f32));
}

static void
PS3_AudioThread(void *unused)
{
    const f32 scale = 1.0f / 32768.0f;
    sys_event_t event;

    (void)unused;
    while (ps3_audio_thread_running) {
        u64 current;
        u32 block;
        s32 ret;
        int i;
        const s16 *source;
        f32 *output;

        ret = sysEventQueueReceive(ps3_audio_queue, &event, 20 * 1000);
        if (!ps3_audio_thread_running)
            break;
        if (ret == PS3_AUDIO_TIMEOUT)
            continue;
        if (ret != 0) {
            ps3_audio_async_error = ret;
            break;
        }

        ret = sysMutexLock(ps3_audio_mutex, 0);
        if (ret != 0) {
            ps3_audio_async_error = ret;
            break;
        }
        current = *(volatile u64 *)(uintptr_t)ps3_audio_config.readIndex;
        block = (u32)((current + 1) % ps3_audio_config.numBlocks);
        source = ps3_mix_buffer + block * PS3_AUDIO_SAMPLES_BLOCK;
        output = PS3_AudioOutput() + block * PS3_AUDIO_SAMPLES_BLOCK;
        for (i = 0; i < PS3_AUDIO_SAMPLES_BLOCK; ++i)
            output[i] = source[i] * scale;
        ret = sysMutexUnlock(ps3_audio_mutex);
        if (ret != 0) {
            ps3_audio_async_error = ret;
            break;
        }
    }

    ps3_audio_thread_running = false;
    sysThreadExit(0);
}

static void
PS3_AudioCleanup(void)
{
    u64 thread_result;

    ps3_audio_thread_running = false;
    if (ps3_audio_port_started) {
        audioPortStop(ps3_audio_port);
        ps3_audio_port_started = false;
    }
    if (ps3_audio_thread_created) {
        sysThreadJoin(ps3_audio_thread, &thread_result);
        ps3_audio_thread_created = false;
    }
    if (ps3_audio_queue_set) {
        audioRemoveNotifyEventQueue(ps3_audio_queue_key);
        ps3_audio_queue_set = false;
    }
    if (ps3_audio_port_open) {
        audioPortClose(ps3_audio_port);
        ps3_audio_port_open = false;
    }
    if (ps3_audio_queue_created) {
        sysEventQueueDestroy(ps3_audio_queue, 0);
        ps3_audio_queue_created = false;
    }
    if (ps3_audio_mutex_created) {
        sysMutexDestroy(ps3_audio_mutex);
        ps3_audio_mutex_created = false;
    }
    if (ps3_audio_initialized) {
        audioQuit();
        ps3_audio_initialized = false;
    }

    ps3_audio_buffer_locked = false;
    ps3_audio_async_error = 0;
    ps3_audio_async_reported = false;
    memset(&ps3_audio_config, 0, sizeof(ps3_audio_config));
}

qboolean
SNDDMA_Init(void)
{
    audioPortParam params;
    sys_mutex_attr_t mutex_attr;
    s32 ret;

    shm = NULL;
    PS3_AudioCleanup();

    ret = audioInit();
    if (ret != 0) {
        Con_Printf("PS3 audio: audioInit failed: %08x\n", (unsigned)ret);
        return false;
    }
    ps3_audio_initialized = true;

    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = PS3_AUDIO_BLOCKS;
    params.attrib = AUDIO_PORT_INITLEVEL;
    params.level = 1.0f;
    ret = audioPortOpen(&params, &ps3_audio_port);
    if (ret != 0) {
        Con_Printf("PS3 audio: audioPortOpen failed: %08x\n", (unsigned)ret);
        goto fail;
    }
    ps3_audio_port_open = true;

    ret = audioGetPortConfig(ps3_audio_port, &ps3_audio_config);
    if (ret != 0) {
        Con_Printf("PS3 audio: audioGetPortConfig failed: %08x\n",
                   (unsigned)ret);
        goto fail;
    }
    if (ps3_audio_config.channelCount != PS3_AUDIO_CHANNELS ||
        ps3_audio_config.numBlocks != PS3_AUDIO_BLOCKS ||
        !ps3_audio_config.readIndex || !ps3_audio_config.audioDataStart) {
        Con_Printf("PS3 audio: unsupported port geometry\n");
        goto fail;
    }

    ret = audioCreateNotifyEventQueue(&ps3_audio_queue,
                                      &ps3_audio_queue_key);
    if (ret != 0) {
        Con_Printf("PS3 audio: audioCreateNotifyEventQueue failed: %08x\n",
                   (unsigned)ret);
        goto fail;
    }
    ps3_audio_queue_created = true;

    ret = audioSetNotifyEventQueue(ps3_audio_queue_key);
    if (ret != 0) {
        Con_Printf("PS3 audio: audioSetNotifyEventQueue failed: %08x\n",
                   (unsigned)ret);
        goto fail;
    }
    ps3_audio_queue_set = true;

    ret = sysEventQueueDrain(ps3_audio_queue);
    if (ret != 0) {
        Con_Printf("PS3 audio: sysEventQueueDrain failed: %08x\n",
                   (unsigned)ret);
        goto fail;
    }

    sysMutexAttrInitialize(mutex_attr);
    mutex_attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO_INHERIT;
    memcpy(mutex_attr.name, "quakeaud", sizeof(mutex_attr.name));
    ret = sysMutexCreate(&ps3_audio_mutex, &mutex_attr);
    if (ret != 0) {
        Con_Printf("PS3 audio: sysMutexCreate failed: %08x\n", (unsigned)ret);
        goto fail;
    }
    ps3_audio_mutex_created = true;
    PS3_ClearAudioBuffers();

    ps3_audio_thread_running = true;
    ret = sysThreadCreate(&ps3_audio_thread, PS3_AudioThread, NULL,
                          PS3_AUDIO_THREAD_PRIO, PS3_AUDIO_THREAD_STACK,
                          THREAD_JOINABLE, "quake_audio");
    if (ret != 0) {
        ps3_audio_thread_running = false;
        Con_Printf("PS3 audio: sysThreadCreate failed: %08x\n", (unsigned)ret);
        goto fail;
    }
    ps3_audio_thread_created = true;

    ret = audioPortStart(ps3_audio_port);
    if (ret != 0) {
        Con_Printf("PS3 audio: audioPortStart failed: %08x\n", (unsigned)ret);
        goto fail;
    }
    ps3_audio_port_started = true;

    memset(&ps3_dma, 0, sizeof(ps3_dma));
    ps3_dma.channels = PS3_AUDIO_CHANNELS;
    ps3_dma.samples = PS3_AUDIO_SAMPLES;
    ps3_dma.submission_chunk = 1;
    ps3_dma.samplebits = 16;
    ps3_dma.speed = PS3_AUDIO_RATE;
    ps3_dma.buffer = (byte *)ps3_mix_buffer;
    shm = &ps3_dma;
    snd_blocked = 0;

    Con_Printf("PS3 audio: %d Hz stereo, %d blocks/%d frames\n",
               PS3_AUDIO_RATE, PS3_AUDIO_BLOCKS,
               PS3_AUDIO_BLOCKS * PS3_AUDIO_FRAMES_BLOCK);
    return true;

fail:
    PS3_AudioCleanup();
    return false;
}

int
SNDDMA_GetDMAPos(void)
{
    u64 current;

    if (!ps3_audio_port_started || !ps3_audio_config.readIndex)
        return 0;
    current = *(volatile u64 *)(uintptr_t)ps3_audio_config.readIndex;
    ps3_dma.samplepos = (int)(current % PS3_AUDIO_BLOCKS) *
                        PS3_AUDIO_SAMPLES_BLOCK;
    return ps3_dma.samplepos;
}

void
SNDDMA_Shutdown(void)
{
    PS3_AudioCleanup();
}

void
SNDDMA_Submit(void)
{
}

int
SNDDMA_LockBuffer(void)
{
    s32 ret;

    if (ps3_audio_async_error) {
        if (!ps3_audio_async_reported) {
            ps3_audio_async_reported = true;
            Con_Printf("PS3 audio: asynchronous failure: %08x\n",
                       (unsigned)ps3_audio_async_error);
        }
        return ps3_audio_async_error;
    }
    if (!ps3_audio_mutex_created)
        return -1;

    ret = sysMutexLock(ps3_audio_mutex, 0);
    if (ret == 0)
        ps3_audio_buffer_locked = true;
    return ret;
}

void
SNDDMA_UnlockBuffer(void)
{
    s32 ret;

    if (!ps3_audio_buffer_locked)
        return;
    ps3_audio_buffer_locked = false;
    ret = sysMutexUnlock(ps3_audio_mutex);
    if (ret != 0 && !ps3_audio_async_error)
        ps3_audio_async_error = ret;
}

void
S_BlockSound(void)
{
    s32 ret;

    snd_blocked++;
    if (snd_blocked != 1 || !ps3_audio_initialized)
        return;

    if (ps3_audio_port_started) {
        ret = audioPortStop(ps3_audio_port);
        if (ret != 0) {
            ps3_audio_async_error = ret;
            return;
        }
        ps3_audio_port_started = false;
    }

    ret = sysMutexLock(ps3_audio_mutex, 0);
    if (ret != 0) {
        ps3_audio_async_error = ret;
        return;
    }
    PS3_ClearAudioBuffers();
    ret = sysMutexUnlock(ps3_audio_mutex);
    if (ret != 0)
        ps3_audio_async_error = ret;
}

void
S_UnblockSound(void)
{
    s32 ret;

    if (snd_blocked <= 0)
        return;
    snd_blocked--;
    if (snd_blocked != 0 || !ps3_audio_initialized ||
        ps3_audio_port_started || ps3_audio_async_error)
        return;

    ret = audioPortStart(ps3_audio_port);
    if (ret != 0)
        ps3_audio_async_error = ret;
    else
        ps3_audio_port_started = true;
}
