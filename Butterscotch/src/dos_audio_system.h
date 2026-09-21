#ifndef _BS_DOS_AUDIO_SYSTEM_H_
#define _BS_DOS_AUDIO_SYSTEM_H_

#include "common.h"
#include "audio_system.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define DOS_AUDIO_MAX_INSTANCES 4
#define DOS_AUDIO_INSTANCE_BASE 100000
#define DOS_AUDIO_DMA_SIZE 32768
#define DOS_AUDIO_DMA_CHANNEL 1

typedef struct {
    uint32_t magic;
    bool     active;
    int32_t  soundIndex;
    int32_t  instanceId;
    int32_t  priority;
    bool     loop;

    int16_t* pcm;        /* full decoded stream, mono 16-bit */
    uint32_t pcmLen;     /* number of samples */
    uint32_t pcmPos;     /* current playback position */
    double   pcmPosD;    /* fractional position for resampling */
    int      srcRate;    /* source sample rate of pcm[] */
    bool     ownsPcm;

    float    currentGain;
    float    targetGain;
    float    startGain;
    float    fadeTimeRemaining;
    float    fadeTotalTime;
} DosSoundInstance;

typedef struct {
    AudioSystem      base;
    FileSystem*      fileSystem;

    DosSoundInstance instances[DOS_AUDIO_MAX_INSTANCES];
    int32_t          nextInstanceCounter;

    int              sbBase;

    uint16_t         dmaSel;
    uint16_t         dmaRmSeg;
    uint8_t*         dmaBuffer;
    unsigned long    dmaPhysAddr;
    bool             dmaAlloc;

    uint32_t         writePos;
    int              sampleRate;

    float            masterGain;
    bool             paused;
    bool             suspended;
} DosAudioSystem;

DosAudioSystem* DosAudioSystem_create(DataWin* dataWin);

#endif
