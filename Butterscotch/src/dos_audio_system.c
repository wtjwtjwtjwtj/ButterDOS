#include "dos_audio_system.h"
#include "data_win.h"
#include "file_system.h"
#include "utils.h"
#include "log.h"
#include "gettime.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pc.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/nearptr.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#include "stb_ds.h"

#define DOS_AUDIO_MAGIC 0x4F4E5455u

static DosAudioSystem* g_audio_singleton = NULL;

/* ---- SB DSP ---- */

static int sbReset(int base) {
    int i; unsigned char s = 0;
    outportb(base + 6, 1);
    for (i = 0; i < 100; i++) (void)inportb(base + 6);
    outportb(base + 6, 0);
    for (i = 0; i < 0xFFFF; i++) {
        s = inportb(base + 0xE);
        if (s & 0x80) break;
    }
    if (!(s & 0x80)) return 0;
    return inportb(base + 0xA) == 0xAA;
}

static void sbWrite(int base, unsigned char v) {
    int i;
    for (i = 0; i < 0xFFFF; i++)
        if (!(inportb(base + 0xC) & 0x80)) break;
    outportb(base + 0xC, v);
}

static int sbDetect(void) {
    static const int bases[] = { 0x220, 0x240, 0x260, 0x280 };
    int i;
    for (i = 0; i < 4; i++)
        if (sbReset(bases[i])) return bases[i];
    return 0;
}


static unsigned int g_sb_tc_rate = 22050;

static void sbSetRate(int base, unsigned int rate) {
    unsigned char tc;
    if (rate < 4000) rate = 4000;
    if (rate > 44100) rate = 44100;
    tc = (unsigned char)(256 - (1000000 / rate));
    g_sb_tc_rate = 1000000u / (256 - tc);
    logInfo("SB setrate: req=%u tc=0x%02X actual=%u Hz\n",
            rate, tc, g_sb_tc_rate);
    sbWrite(base, 0x40);
    sbWrite(base, tc);
}

static void sbStartDMA(int base, unsigned long len) {
    unsigned long l = len - 1;
    sbWrite(base, 0x48);
    sbWrite(base, l & 0xFF);
    sbWrite(base, (l >> 8) & 0xFF);
    sbWrite(base, 0x1C);
    logInfo("SB: startDMA block=%lu\n", l);
}

static void sbStopDMA(int base) { sbWrite(base, 0xD0); }
static void sbSpeaker(int base, int on) { sbWrite(base, on ? 0xD1 : 0xD3); }

/* ---- 8237 DMA ---- */

static unsigned int dmaReadAddr(int channel) {
    unsigned int v;
    if (channel != 1) return 0;
    outportb(0x0C, 0x00);
    v  = inportb(0x02);
    v |= inportb(0x02) << 8;
    return v;
}

static void dmaProgram(unsigned long physAddr, unsigned long len, int channel) {
    unsigned char pagePort, addrPort, countPort, modePort, modeVal, maskBit;
    unsigned int page = (physAddr >> 16) & 0xFF;
    unsigned int offset = physAddr & 0xFFFF;
    unsigned int count = len - 1;

    if (channel == 1) {
        pagePort = 0x83; addrPort = 0x02; countPort = 0x03;
        maskBit = 0x01; modeVal = 0x59; /* single | incr | autoinit | read | ch1 */
    } else return;

    modePort = 0x0B;
    outportb(0x0A, maskBit | 0x04);
    outportb(0x0C, 0x00);
    outportb(modePort, modeVal);
    outportb(pagePort, page);
    outportb(addrPort, offset & 0xFF);
    outportb(addrPort, (offset >> 8) & 0xFF);
    outportb(countPort, count & 0xFF);
    outportb(countPort, (count >> 8) & 0xFF);
    outportb(0x0A, maskBit);
}

/* ---- Full-file decode to mono 16-bit PCM ---- */

static int16_t* load_ogg_full(const char* name, uint32_t* outLen, int* outRate) {
    int channels = 0, sampleRate = 0;
    short* tmp = NULL;
    int n = stb_vorbis_decode_filename((char*)name, &channels, &sampleRate, &tmp);
    if (n <= 0 || !tmp) {
        logError("load_ogg: '%s' decode failed\n", name);
        return NULL;
    }
    *outRate = sampleRate;
    if (channels == 1) {
        *outLen = (uint32_t)n;
        return (int16_t*)tmp;
    }
    {
        uint32_t frames = (uint32_t)n;
        uint32_t k;
        int16_t* out = (int16_t*)malloc(frames * sizeof(int16_t));
        if (!out) { free(tmp); return NULL; }
        for (k = 0; k < frames; k++) {
            int32_t l = tmp[k * channels + 0];
            int32_t r = (channels >= 2) ? tmp[k * channels + 1] : l;
            out[k] = (int16_t)((l + r) / 2);
        }
        free(tmp);
        *outLen = frames;
        return out;
    }
}


static uint32_t rd32le_local(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint16_t rd16le_local(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1]<<8));
}

static int16_t* load_wav_full(const char* name, uint32_t* outLen, int* outRate) {
    FILE* f = fopen(name, "rb");
    uint8_t hdr[12], chdr[8];
    uint32_t dataOff = 0, dataLen = 0;
    int channels = 0, bits = 0, rate = 0;
    uint8_t* raw;
    int16_t* out;
    int bps, frameBytes;
    uint32_t frames, i;

    if (!f) return NULL;
    if (fread(hdr,1,12,f) != 12 || memcmp(hdr,"RIFF",4) || memcmp(hdr+8,"WAVE",4)) {
        fclose(f); return NULL;
    }
    while (fread(chdr,1,8,f) == 8) {
        uint32_t sz = rd32le_local(chdr+4);
        if (!memcmp(chdr,"fmt ",4)) {
            uint8_t fmt[16];
            if (sz < 16 || fread(fmt,1,16,f) != 16) break;
            channels = rd16le_local(fmt+2);
            rate     = (int)rd32le_local(fmt+4);
            bits     = rd16le_local(fmt+14);
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(chdr,"data",4)) {
            dataOff = (uint32_t)ftell(f);
            dataLen = sz;
            break;
        } else {
            fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
    if (!dataOff || !dataLen || !rate || channels < 1 || channels > 2 ||
        (bits != 8 && bits != 16)) {
        fclose(f); return NULL;
    }
    bps = bits / 8;
    frameBytes = bps * channels;
    frames = dataLen / frameBytes;
    raw = (uint8_t*)malloc(dataLen);
    if (!raw) { fclose(f); return NULL; }
    fseek(f, dataOff, SEEK_SET);
    if (fread(raw, 1, dataLen, f) != dataLen) { free(raw); fclose(f); return NULL; }
    fclose(f);

    out = (int16_t*)malloc((size_t)frames * sizeof(int16_t));
    if (!out) { free(raw); return NULL; }
    for (i = 0; i < frames; i++) {
        uint8_t* p = raw + (size_t)i * frameBytes;
        int32_t sm = 0;
        if (bits == 16) {
            int16_t l = (int16_t)rd16le_local(p);
            int16_t r = (channels == 2) ? (int16_t)rd16le_local(p+2) : l;
            sm = (l + r) / 2;
        } else {
            int32_t l = ((int32_t)p[0] - 128) * 256;
            int32_t r = (channels == 2) ? (((int32_t)p[1] - 128) * 256) : l;
            sm = (l + r) / 2;
        }
        out[i] = (int16_t)sm;
    }
    free(raw);
    *outLen = frames;
    *outRate = rate;
    return out;
}


/* Decode an OGG stream that lives in memory (WAD AUDO entry).
   Returns a malloc'd mono int16_t array, or NULL. */
static int16_t* load_ogg_memory(const uint8_t* data, uint32_t size,
                                uint32_t* outLen, int* outRate) {
    int channels = 0, sampleRate = 0;
    short* tmp = NULL;
    int n = stb_vorbis_decode_memory(data, (int)size,
                                     &channels, &sampleRate, &tmp);
    if (n <= 0 || !tmp) {
        logError("load_ogg_memory: decode failed (size=%u)\n",
                 (unsigned)size);
        return NULL;
    }
    *outRate = sampleRate;
    if (channels == 1) {
        *outLen = (uint32_t)n;
        return (int16_t*)tmp;
    }
    {
        uint32_t frames = (uint32_t)n;
        uint32_t k;
        int16_t* out = (int16_t*)malloc(frames * sizeof(int16_t));
        if (!out) { free(tmp); return NULL; }
        for (k = 0; k < frames; k++) {
            int32_t l = tmp[k * channels + 0];
            int32_t r = (channels >= 2) ? tmp[k * channels + 1] : l;
            out[k] = (int16_t)((l + r) / 2);
        }
        free(tmp);
        *outLen = frames;
        return out;
    }
}

static int load_external(DosSoundInstance* inst, const char* name) {
    FILE* f;
    uint8_t magic[4];
    uint32_t pcmLen = 0;
    int16_t* pcm = NULL;
    int      srcRate = 0;

    f = fopen(name, "rb");
    if (!f) {
        logError("load_external: fopen('%s') failed\n", name);
        return -1;
    }
    if (fread(magic, 1, 4, f) != 4) {
        logError("load_external: '%s' too short\n", name);
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Detect format by content, not by extension. This lets a file
       named .ogg actually contain WAV data (the resample tool does
       exactly that - overwrites the .ogg in place with WAV bytes). */
    if (!memcmp(magic, "RIFF", 4)) {
        pcm = load_wav_full(name, &pcmLen, &srcRate);
    } else if (!memcmp(magic, "OggS", 4)) {
        pcm = load_ogg_full(name, &pcmLen, &srcRate);
    } else {
        logError("load_external: '%s' unknown format (magic %02X %02X %02X %02X)\n",
                 name, magic[0], magic[1], magic[2], magic[3]);
        return -1;
    }
    if (!pcm) {
        logError("load_external: '%s' decode failed\n", name);
        return -1;
    }

    inst->pcm = pcm;
    inst->pcmLen = pcmLen;
    inst->pcmPos = 0;
    inst->pcmPosD = 0.0;
    inst->srcRate = srcRate;
    inst->ownsPcm = true;
    return 0;
}


static void inst_close(DosSoundInstance* inst) {
    inst->magic = 0;
    if (inst->pcm && inst->ownsPcm) free(inst->pcm);
    inst->pcm = NULL;
    inst->pcmLen = 0;
    inst->pcmPos = 0;
    inst->ownsPcm = false;
}

/* ---- Instance helpers ---- */

static DosSoundInstance* allocInstance(DosAudioSystem* d) {
    int i;
    for (i = 0; i < DOS_AUDIO_MAX_INSTANCES; i++)
        if (!d->instances[i].active) return &d->instances[i];
    {
        DosSoundInstance* best = NULL;
        for (i = 0; i < DOS_AUDIO_MAX_INSTANCES; i++) {
            DosSoundInstance* inst = &d->instances[i];
            if (best == NULL || inst->priority < best->priority) best = inst;
        }
        if (best) { inst_close(best); best->active = false; }
        return best;
    }
}

static DosSoundInstance* findInstance(DosAudioSystem* d, int32_t id) {
    int i;
    for (i = 0; i < DOS_AUDIO_MAX_INSTANCES; i++)
        if (d->instances[i].active && d->instances[i].instanceId == id)
            return &d->instances[i];
    return NULL;
}

static int32_t resolveInstance(DosAudioSystem* d, int32_t soi) {
    int i;
    if (soi >= DOS_AUDIO_INSTANCE_BASE) return soi;
    for (i = 0; i < DOS_AUDIO_MAX_INSTANCES; i++)
        if (d->instances[i].active && d->instances[i].soundIndex == soi)
            return d->instances[i].instanceId;
    return -1;
}

/* ---- Mixer (called by platform tick, not the vtable) ---- */

static void dosMixInternal(DosAudioSystem* d, float dt) {
    uint32_t dmaPos, targetPos, toWrite, i;
    int j;
    static int dbg = 0;
    (void)dt;

    if (!d->dmaAlloc || !d->sbBase) return;
    if (d->paused || d->suspended) return;

    /* Update per-instance gain fades (approximate per-call). */
    for (j = 0; j < DOS_AUDIO_MAX_INSTANCES; j++) {
        DosSoundInstance* inst = &d->instances[j];
        if (!inst->active) continue;
        if (inst->magic != DOS_AUDIO_MAGIC) { inst->active = false; continue; }
        if (inst->fadeTimeRemaining > 0.0f) {
            float step = 1.0f / 30.0f;
            if (step > inst->fadeTimeRemaining) step = inst->fadeTimeRemaining;
            inst->fadeTimeRemaining -= step;
            if (inst->fadeTotalTime > 0.0f) {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain
                    + (inst->targetGain - inst->startGain) * t;
            } else inst->currentGain = inst->targetGain;
        } else inst->currentGain = inst->targetGain;
    }

    /* Ask the DMA controller where the DSP is currently reading.
       We'll write up to half a buffer ahead of it. This self-syncs to
       the DSP's actual playback rate, independent of frame rate. */
    {
        unsigned int cur  = dmaReadAddr(DOS_AUDIO_DMA_CHANNEL) & 0xFFFF;
        unsigned int base = (unsigned int)(d->dmaPhysAddr & 0xFFFF);
        unsigned int delta = (cur >= base) ? (cur - base)
                                           : (cur + 0x10000u - base);
        dmaPos = delta % DOS_AUDIO_DMA_SIZE;
    }

    targetPos = (dmaPos + DOS_AUDIO_DMA_SIZE / 2) % DOS_AUDIO_DMA_SIZE;
    toWrite = (targetPos + DOS_AUDIO_DMA_SIZE - d->writePos) % DOS_AUDIO_DMA_SIZE;
    if (toWrite > DOS_AUDIO_DMA_SIZE / 2) toWrite = DOS_AUDIO_DMA_SIZE / 2;

    if (dbg < 3) {
        int act = 0, k;
        for (k = 0; k < DOS_AUDIO_MAX_INSTANCES; k++)
            if (d->instances[k].active) act++;
        logInfo("mix: dmaPos=%u writePos=%u toWrite=%u active=%d\n",
                (unsigned)dmaPos, (unsigned)d->writePos,
                (unsigned)toWrite, act);
        dbg++;
    }

    for (i = 0; i < toWrite; i++) {
        int32_t mix = 0;
        for (j = 0; j < DOS_AUDIO_MAX_INSTANCES; j++) {
            DosSoundInstance* inst = &d->instances[j];
            if (!inst->active) continue;
            if (inst->magic != DOS_AUDIO_MAGIC) { inst->active = false; continue; }
            if (!inst->pcm || inst->pcmLen == 0) { inst->active = false; continue; }
            if (inst->pcmPosD >= (double)inst->pcmLen) {
                if (inst->loop) inst->pcmPosD = 0.0;
                else { inst->active = false; continue; }
            }
            {
                uint32_t pi = (uint32_t)inst->pcmPosD;
                if (pi >= inst->pcmLen) pi = inst->pcmLen - 1;
                mix += (int32_t)(inst->pcm[pi] * inst->currentGain);
                if (inst->srcRate > 0 && inst->srcRate != d->sampleRate) {
                    inst->pcmPosD += (double)inst->srcRate / (double)d->sampleRate;
                } else {
                    inst->pcmPosD += 1.0;
                }
            }
        }
        if (mix > 32767)  mix = 32767;
        if (mix < -32768) mix = -32768;
        d->dmaBuffer[d->writePos] = (uint8_t)(((mix + 32768) >> 8) & 0xFF);
        d->writePos = (d->writePos + 1) % DOS_AUDIO_DMA_SIZE;
    }
}


void DosAudioSystem_platformTick(float dt) {
    if (g_audio_singleton) dosMixInternal(g_audio_singleton, dt);
}

/* ---- Vtable ---- */

static uint32_t dma_buffer_pos(DosAudioSystem* d) {
    unsigned int cur  = dmaReadAddr(DOS_AUDIO_DMA_CHANNEL) & 0xFFFF;
    unsigned int base = (unsigned int)(d->dmaPhysAddr & 0xFFFF);
    unsigned int delta = (cur >= base) ? (cur - base)
                                       : (cur + 0x10000 - base);
    return delta % DOS_AUDIO_DMA_SIZE;
}

static uint32_t measure_sb_rate(DosAudioSystem* d) {
    uint32_t sz = DOS_AUDIO_DMA_SIZE;
    uint32_t target = 4096;
    uint32_t p0, p1;
    uint64_t t0, t1;
    uint32_t rate;

    p0 = dma_buffer_pos(d);
    t0 = nowNanos();
    for (;;) {
        p1 = dma_buffer_pos(d);
        if (((p1 + sz - p0) % sz) >= target) break;
        if (nowNanos() - t0 > 3000000000ULL) {
            logInfo("SB rate measure: timeout, defaulting 22050\n");
            return 22050;
        }
    }
    t1 = nowNanos();
    if (t1 == t0) return 22050;
    rate = (uint32_t)(((uint64_t)target * 1000000000ULL) / (t1 - t0));
    logInfo("SB measured rate: %u Hz (p0=%u p1=%u dt=%lu ns)\n",
            rate, (unsigned)p0, (unsigned)p1, (unsigned long)(t1 - t0));
    return rate;
}

static void dosInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    DosAudioSystem* d = (DosAudioSystem*)audio;
    _go32_dpmi_seginfo info;
    int i;

    (void)fileSystem;
    logInfo("DOS audio: init\n");

    arrput(audio->audioGroups, dataWin);
    d->fileSystem = fileSystem;
    d->masterGain = 0.8f;

    for (i = 0; i < DOS_AUDIO_MAX_INSTANCES; i++) {
        d->instances[i].active = false;
        d->instances[i].pcm = NULL;
    }

    if (__djgpp_nearptr_enable() == 0) {
        logError("DOS audio: nearptr failed\n");
        return;
    }

    /* Auto-probe standard SB ports. No config file needed. */
    d->sbBase = sbDetect();
    if (!d->sbBase) { logError("DOS audio: no SB detected\n"); return; }
    logInfo("DOS audio: SB at 0x%03X\n", d->sbBase);

    /* Query DSP version */
    {
        unsigned char maj = 0, min = 0, st = 0;
        int i;
        sbWrite(d->sbBase, 0xE1);
        for (i = 0; i < 0xFFFF; i++) { st = inportb(d->sbBase + 0xE); if (st & 0x80) break; }
        if (st & 0x80) maj = inportb(d->sbBase + 0xA);
        for (i = 0; i < 0xFFFF; i++) { st = inportb(d->sbBase + 0xE); if (st & 0x80) break; }
        if (st & 0x80) min = inportb(d->sbBase + 0xA);
        logInfo("DOS audio: DSP version %d.%02d\n", maj, min);
    }

    d->sampleRate = 22050;

    info.size = (DOS_AUDIO_DMA_SIZE + 0x10000) / 16;
    if (_go32_dpmi_allocate_dos_memory(&info) != 0) {
        logError("DOS audio: DMA alloc failed\n");
        return;
    }
    d->dmaSel = info.pm_selector;
    d->dmaRmSeg = info.rm_segment;
    d->dmaAlloc = true;
    {
        unsigned long base = (unsigned long)info.rm_segment * 16UL;
        unsigned long off  = base & 0xFFFFUL;
        unsigned long pad  = (off + DOS_AUDIO_DMA_SIZE > 0x10000UL)
                           ? (0x10000UL - off) : 0;
        d->dmaPhysAddr = base + pad;
    }
    d->dmaBuffer = (uint8_t*)(__djgpp_conventional_base + d->dmaPhysAddr);
    memset(d->dmaBuffer, 0x80, DOS_AUDIO_DMA_SIZE);
    logInfo("DOS audio: DMA phys=0x%05lX\n", d->dmaPhysAddr);

    dmaProgram(d->dmaPhysAddr, DOS_AUDIO_DMA_SIZE, DOS_AUDIO_DMA_CHANNEL);

    d->sampleRate = 22050;
    sbSetRate(d->sbBase, d->sampleRate);
    sbSpeaker(d->sbBase, 1);
    sbStartDMA(d->sbBase, DOS_AUDIO_DMA_SIZE);

    d->writePos = DOS_AUDIO_DMA_SIZE / 2;
    {
        uint32_t measured = measure_sb_rate(d);
        logInfo("SB measured=%u Hz, tc-derived=%u Hz\n",
                measured, g_sb_tc_rate);
        d->sampleRate = (int)g_sb_tc_rate;
        logInfo("DOS audio: mixer locked to %d Hz\n", d->sampleRate);
    }
    d->nextInstanceCounter = DOS_AUDIO_INSTANCE_BASE;
    g_audio_singleton = d;
    logInfo("DOS audio: ready\n");
}

static void dosDestroy(AudioSystem* audio) {
    DosAudioSystem* d = (DosAudioSystem*)audio;
    int i;
    if (d->sbBase) { sbStopDMA(d->sbBase); sbSpeaker(d->sbBase, 0); }
    for (i = 0; i < DOS_AUDIO_MAX_INSTANCES; i++) {
        inst_close(&d->instances[i]);
        d->instances[i].active = false;
    }
    arrfree(audio->audioGroups);
    if (d->dmaAlloc) {
        _go32_dpmi_seginfo info;
        info.pm_selector = d->dmaSel;
        info.rm_segment = d->dmaRmSeg;
        _go32_dpmi_free_dos_memory(&info);
        d->dmaAlloc = false;
    }
    g_audio_singleton = NULL;
    free(d);
}

static void dosUpdateNoop(AudioSystem* a, float dt) { (void)a; (void)dt; }

static int32_t dosPlaySound(AudioSystem* audio, int32_t soundIndex,
                            int32_t priority, bool loop) {
    DosAudioSystem* d = (DosAudioSystem*)audio;
    DataWin* dw = audio->dw;
    Sound* sound;
    DosSoundInstance* inst;
    int16_t* pcm = NULL;
    uint32_t pcmLen = 0;
    int      srcRate = 0;

    if (!dw || soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) return -1;
    sound = &dw->sond.sounds[soundIndex];
    if (!sound->present) return -1;

    /* External file (music, some long SFX) */
    if (sound->file && sound->file[0] != '\0') {
        FILE* probe = fopen(sound->file, "rb");
        if (probe) {
            uint8_t magic[4];
            size_t got = fread(magic, 1, 4, probe);
            fclose(probe);
            if (got != 4) return -1;
            if (!memcmp(magic, "RIFF", 4))
                pcm = load_wav_full(sound->file, &pcmLen, &srcRate);
            else if (!memcmp(magic, "OggS", 4))
                pcm = load_ogg_full(sound->file, &pcmLen, &srcRate);
        }
        if (!pcm) {
            logError("playSound: external '%s' load failed\n", sound->file);
            return -1;
        }
    } else {
        /* Embedded AUDO entry inside data.win */
        AudioEntry* entry;
        if (sound->audioFile < 0) return -1;
        DataWin_loadAudoIfNeeded(dw, (uint32_t)sound->audioFile);
        if ((uint32_t)sound->audioFile >= dw->audo.count) return -1;
        entry = &dw->audo.entries[sound->audioFile];
        if (!entry->present || !entry->data || entry->dataSize == 0) {
            logError("playSound: SOND %d embedded entry %d empty\n",
                     (int)soundIndex, (int)sound->audioFile);
            return -1;
        }
        pcm = load_ogg_memory(entry->data, entry->dataSize, &pcmLen, &srcRate);
        if (!pcm) return -1;
    }

    inst = allocInstance(d);
    if (!inst) { free(pcm); return -1; }
    memset(inst, 0, sizeof(*inst));

    inst->pcm      = pcm;
    inst->pcmLen   = pcmLen;
    inst->pcmPos   = 0;
    inst->pcmPosD  = 0.0;
    inst->srcRate  = srcRate;
    inst->ownsPcm  = true;

    inst->magic = DOS_AUDIO_MAGIC;
    inst->active = true;
    inst->soundIndex = soundIndex;
    inst->instanceId = d->nextInstanceCounter++;
    if (d->nextInstanceCounter < DOS_AUDIO_INSTANCE_BASE)
        d->nextInstanceCounter = DOS_AUDIO_INSTANCE_BASE;
    inst->priority = priority;
    inst->loop = loop;
    inst->currentGain = sound->volume * d->masterGain;
    inst->targetGain = inst->currentGain;
    inst->startGain = inst->currentGain;

    logInfo("playSound: %s id=%d len=%u rate=%d\n",
            sound->file && sound->file[0] ? sound->file : "(embedded)",
            inst->instanceId, (unsigned)pcmLen, srcRate);
    return inst->instanceId;
}



static void dosStopSound(AudioSystem* a, int32_t soi) {
    DosAudioSystem* d = (DosAudioSystem*)a;
    int32_t id = resolveInstance(d, soi);
    DosSoundInstance* inst;
    if (id < 0) return;
    inst = findInstance(d, id);
    if (!inst) return;
    inst_close(inst);
    inst->active = false;
}

static void dosStopAll(AudioSystem* a) {
    DosAudioSystem* d = (DosAudioSystem*)a;
    int i;
    for (i = 0; i < DOS_AUDIO_MAX_INSTANCES; i++) {
        inst_close(&d->instances[i]);
        d->instances[i].active = false;
    }
}

static bool dosIsPlaying(AudioSystem* a, int32_t soi) {
    DosAudioSystem* d = (DosAudioSystem*)a;
    int32_t id = resolveInstance(d, soi);
    return id >= 0 && findInstance(d, id) != NULL;
}

static void dosPauseSound(AudioSystem* a, int32_t s) { (void)a; (void)s; }
static void dosResumeSound(AudioSystem* a, int32_t s) { (void)a; (void)s; }
static void dosPauseAll(AudioSystem* a) { ((DosAudioSystem*)a)->paused = true; }
static void dosResumeAll(AudioSystem* a) { ((DosAudioSystem*)a)->paused = false; }
static void dosSuspend(AudioSystem* a) { ((DosAudioSystem*)a)->suspended = true; }
static void dosResume(AudioSystem* a) { ((DosAudioSystem*)a)->suspended = false; }

static void dosSetSoundGain(AudioSystem* a, int32_t soi, float gain, uint32_t ms) {
    DosAudioSystem* d = (DosAudioSystem*)a;
    int32_t id = resolveInstance(d, soi);
    DosSoundInstance* inst;
    if (id < 0) return;
    inst = findInstance(d, id);
    if (!inst) return;
    inst->startGain = inst->currentGain;
    inst->targetGain = gain;
    inst->fadeTotalTime = ms / 1000.0f;
    inst->fadeTimeRemaining = inst->fadeTotalTime;
    if (ms == 0) inst->currentGain = gain;
}
static float dosGetSoundGain(AudioSystem* a, int32_t soi) {
    DosAudioSystem* d = (DosAudioSystem*)a;
    int32_t id = resolveInstance(d, soi);
    DosSoundInstance* inst;
    if (id < 0) return 1.0f;
    inst = findInstance(d, id);
    return inst ? inst->currentGain : 1.0f;
}
static void dosSetSoundPitch(AudioSystem* a, int32_t s, float p) { (void)a; (void)s; (void)p; }
static float dosGetSoundPitch(AudioSystem* a, int32_t s) { (void)a; (void)s; return 1.0f; }
static float dosGetTrackPosition(AudioSystem* a, int32_t s) { (void)a; (void)s; return 0.0f; }
static void dosSetTrackPosition(AudioSystem* a, int32_t s, float p) { (void)a; (void)s; (void)p; }
static float dosGetSoundLength(AudioSystem* a, int32_t s) { (void)a; (void)s; return 1.0f; }
static void dosSetMasterGain(AudioSystem* a, float g) { ((DosAudioSystem*)a)->masterGain = g; }
static void dosSetMasterGainForListener(AudioSystem* a, float g, int32_t l) { (void)l; ((DosAudioSystem*)a)->masterGain = g; }
static void dosSetChannelCount(AudioSystem* a, int32_t c) { (void)a; (void)c; }
static void dosGroupLoad(AudioSystem* a, int32_t gi) { (void)a; (void)gi; }
static bool dosGroupIsLoaded(AudioSystem* a, int32_t gi) { (void)a; (void)gi; return true; }
static int32_t dosCreateStream(AudioSystem* a, const char* f) { (void)a; (void)f; return -1; }
static bool dosDestroyStream(AudioSystem* a, int32_t s) { (void)a; (void)s; return false; }

static AudioSystemVtable dosVtable;

DosAudioSystem* DosAudioSystem_create(DataWin* dataWin) {
    DosAudioSystem* d = (DosAudioSystem*)safeCalloc(1, sizeof(DosAudioSystem));

    dosVtable.init = dosInit;
    dosVtable.destroy = dosDestroy;
    dosVtable.update = dosUpdateNoop;
    dosVtable.playSound = dosPlaySound;
    dosVtable.stopSound = dosStopSound;
    dosVtable.stopAll = dosStopAll;
    dosVtable.isPlaying = dosIsPlaying;
    dosVtable.pauseSound = dosPauseSound;
    dosVtable.resumeSound = dosResumeSound;
    dosVtable.pauseAll = dosPauseAll;
    dosVtable.resumeAll = dosResumeAll;
    dosVtable.suspend = dosSuspend;
    dosVtable.resume = dosResume;
    dosVtable.setSoundGain = dosSetSoundGain;
    dosVtable.getSoundGain = dosGetSoundGain;
    dosVtable.setSoundPitch = dosSetSoundPitch;
    dosVtable.getSoundPitch = dosGetSoundPitch;
    dosVtable.getTrackPosition = dosGetTrackPosition;
    dosVtable.setTrackPosition = dosSetTrackPosition;
    dosVtable.getSoundLength = dosGetSoundLength;
    dosVtable.setMasterGain = dosSetMasterGain;
    dosVtable.setMasterGainForListener = dosSetMasterGainForListener;
    dosVtable.setChannelCount = dosSetChannelCount;
    dosVtable.groupLoad = dosGroupLoad;
    dosVtable.groupIsLoaded = dosGroupIsLoaded;
    dosVtable.createStream = dosCreateStream;
    dosVtable.destroyStream = dosDestroyStream;

    d->base.vtable = &dosVtable;
    d->base.dw = dataWin;
    return d;
}
