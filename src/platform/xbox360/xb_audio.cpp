// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_audio.h"

#include <xtl.h>
#include <xaudio2.h>
#include <math.h>
#include <string.h>

#include "../../core/log.h"

#ifndef SOUNDTOUCH_INTEGER_SAMPLES
#define SOUNDTOUCH_INTEGER_SAMPLES 1
#endif
#ifndef ST_NO_EXCEPTION_HANDLING
#define ST_NO_EXCEPTION_HANDLING 1
#endif
#include "../../third_party/soundtouch/SoundTouch.h"

#define SLOTS 64
#define SLOT_FRAMES 1024
#define QUEUE_TARGET 5
#define RATE_GAIN 0.10f
#define RATE_MIN 0.90f
#define RATE_MAX 1.02f
#define RATE_SMOOTH 0.05f

static int s_targetMs = 120;
static u64 s_framesSubmitted;
static float s_ratio = 1.0f, s_ratioApplied = 1.0f;
static XbAudioStats s_stats;

static IXAudio2 *s_xa;
static IXAudio2MasteringVoice *s_master;
static IXAudio2SourceVoice *s_voice;
static s16 s_slots[SLOTS][SLOT_FRAMES * 2];
static int s_next;
static int s_playing;
static double s_phase;

static CRITICAL_SECTION s_lock;
static int s_lockReady;
static HANDLE s_thread;
static volatile LONG s_threadQuit;

static void lock(void)
{
    if (s_lockReady) EnterCriticalSection(&s_lock);
}

static void unlock(void)
{
    if (s_lockReady) LeaveCriticalSection(&s_lock);
}

int xb_audio_init(void)
{
    WAVEFORMATEX wf;
    if (!s_lockReady)
    {
        InitializeCriticalSection(&s_lock);
        s_lockReady = 1;
    }
    if (FAILED(XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        LOG_ERROR("[audio] XAudio2Create failed");
        s_xa = NULL;
        return -1;
    }
    if (FAILED(s_xa->CreateMasteringVoice(&s_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, NULL)))
    {
        LOG_ERROR("[audio] CreateMasteringVoice failed");
        s_master = NULL;
        return -1;
    }
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = XB_AUDIO_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = XB_AUDIO_RATE * 4;
    if (FAILED(s_xa->CreateSourceVoice(&s_voice, &wf, 0, XAUDIO2_MAX_FREQ_RATIO, NULL, NULL, NULL)))
    {
        LOG_ERROR("[audio] CreateSourceVoice failed");
        s_voice = NULL;
        return -1;
    }
    LOG_INFO("[audio] XAudio2 ready, %d Hz stereo 16-bit", XB_AUDIO_RATE);
    return 0;
}

void xb_audio_shutdown(void)
{
    if (s_thread)
    {
        InterlockedExchange(&s_threadQuit, 1);
        WaitForSingleObject(s_thread, 1000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    lock();
    if (s_voice)
    {
        s_voice->Stop(0);
        s_voice->FlushSourceBuffers();
        s_voice->DestroyVoice();
        s_voice = NULL;
    }
    if (s_master) { s_master->DestroyVoice(); s_master = NULL; }
    if (s_xa) { s_xa->Release(); s_xa = NULL; }
    unlock();
}

void xb_audio_set_target_ms(int ms)
{
    s_targetMs = ms < 40 ? 40 : ms > 500 ? 500 : ms;
}

void xb_audio_stats(XbAudioStats *out)
{
    *out = s_stats;
    s_stats.underruns = 0;
}

#define STRETCH_ON 0.95f
#define STRETCH_OFF 0.985f
#define STRETCH_MIN 0.40f
#define STRETCH_MAX 1.15f
#define STRETCH_GAIN 0.30f
#define STRETCH_IN 2048

static soundtouch::SoundTouch *s_st;
static int s_stretchOn;
static int s_stretching;
static float s_speed = 1.0f, s_tempo = 1.0f;
static u32 s_speedFrames, s_speedT0;
static s16 s_in[STRETCH_IN * 2];
static u32 s_resyncs;

static u32 backlog(const XAUDIO2_VOICE_STATE *vs)
{
    u32 n = vs->SamplesPlayed >= s_framesSubmitted ? 0 : (u32)(s_framesSubmitted - vs->SamplesPlayed);
    if (s_stretching && s_st) n += s_st->numSamples() + (u32)((float)s_st->numUnprocessedSamples() / s_tempo);
    return n;
}

static void resync(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void), u32 excess)
{
    u32 n, dropped = 0;
    if (s_stretching && s_st)
    {
        dropped = s_st->numSamples() + s_st->numUnprocessedSamples();
        s_st->clear();
    }
    while (dropped < excess && (n = available()) > 0)
    {
        n = read(s_in, n < STRETCH_IN ? n : STRETCH_IN);
        if (!n) break;
        dropped += n;
    }
    if (!dropped) return;
    s_resyncs++;
    LOG_INFO("[audio] %u ms of sound ahead dropped (it lagged behind the picture)", (u32)((u64)dropped * 1000 / XB_AUDIO_RATE));
}

static void speed_note(u32 frames)
{
    u32 now = GetTickCount(), dt;
    s_speedFrames += frames;
    if (!s_speedT0)
    {
        s_speedT0 = now;
        s_speedFrames = 0;
        return;
    }
    dt = now - s_speedT0;
    if (dt >= 250)
    {
        float v = (float)s_speedFrames * 1000.0f / ((float)dt * XB_AUDIO_RATE);
        s_speed += (v - s_speed) * 0.3f;
        s_speedFrames = 0;
        s_speedT0 = now;
    }
}

static void stretch_feed(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void), XAUDIO2_VOICE_STATE *vs, u32 target)
{
    u32 n;
    float fill, want;
    while ((n = available()) > 0)
    {
        n = read(s_in, n < STRETCH_IN ? n : STRETCH_IN);
        if (!n) break;
        speed_note(n);
        s_st->putSamples(s_in, n);
    }
    fill = (float)backlog(vs) / (float)target;
    want = s_speed * (1.0f + STRETCH_GAIN * (fill - 1.0f));
    if (want < STRETCH_MIN) want = STRETCH_MIN;
    if (want > STRETCH_MAX) want = STRETCH_MAX;
    s_tempo += (want - s_tempo) * 0.2f;
    s_st->setTempo(s_tempo);
    while (vs->BuffersQueued < SLOTS - 1 && s_st->numSamples() >= SLOT_FRAMES)
    {
        XAUDIO2_BUFFER b;
        s16 *out = s_slots[s_next];
        s_st->receiveSamples(out, SLOT_FRAMES);
        memset(&b, 0, sizeof(b));
        b.AudioBytes = sizeof(s_slots[0]);
        b.pAudioData = (const BYTE *)out;
        if (FAILED(s_voice->SubmitSourceBuffer(&b, NULL))) break;
        s_next = (s_next + 1) % SLOTS;
        s_framesSubmitted += SLOT_FRAMES;
        vs->BuffersQueued++;
    }
}

static int feed(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void))
{
    XAUDIO2_VOICE_STATE vs;
    u32 pending, target, start;
    if (!s_voice) return 0;
    s_voice->GetState(&vs);
    target = (u32)((u64)XB_AUDIO_RATE * s_targetMs / 1000);
    start = s_stats.underruns ? target / 2 : target * 3 / 5;
    if (s_playing && vs.BuffersQueued == 0)
    {
        s_voice->Stop(0);
        s_playing = 0;
        s_stats.underruns++;
    }
    if (s_st && s_stretchOn)
    {
        if (!s_stretching && s_speed < STRETCH_ON)
        {
            s_st->clear();
            s_tempo = s_speed;
            s_stretching = 1;
            if (s_ratioApplied != 1.0f)
            {
                s_ratio = s_ratioApplied = 1.0f;
                s_voice->SetFrequencyRatio(1.0f, XAUDIO2_COMMIT_NOW);
            }
            LOG_INFO("[audio] time stretching on (emulation at %d%% speed)", (int)(s_speed * 100.0f + 0.5f));
        }
        else if (s_stretching && s_speed > STRETCH_OFF && s_st->numSamples() < SLOT_FRAMES && backlog(&vs) <= target + target / 4)
        {
            s_stretching = 0;
            LOG_INFO("[audio] time stretching off");
        }
    }
    if (s_playing)
    {
        u32 b = backlog(&vs) + available();
        u32 t = s_stretching ? target * 2 : target;
        if (b > t * 2 && b > t + XB_AUDIO_RATE * 15 / 100) resync(read, available, b - t);
    }
    if (s_stretching)
    {
        stretch_feed(read, available, &vs, s_speed > STRETCH_OFF ? target : target * 2);
    }
    while (!s_stretching && vs.BuffersQueued < SLOTS - 1 && available() >= SLOT_FRAMES)
    {
        XAUDIO2_BUFFER b;
        s16 *out = s_slots[s_next];
        if (s_st && s_stretchOn) speed_note(SLOT_FRAMES);
        read(out, SLOT_FRAMES);
        memset(&b, 0, sizeof(b));
        b.AudioBytes = sizeof(s_slots[0]);
        b.pAudioData = (const BYTE *)out;
        if (FAILED(s_voice->SubmitSourceBuffer(&b, NULL))) break;
        s_next = (s_next + 1) % SLOTS;
        s_framesSubmitted += SLOT_FRAMES;
        vs.BuffersQueued++;
    }
    s_voice->GetState(&vs);
    pending = backlog(&vs);
    if (!s_playing)
    {
        u32 queued = vs.SamplesPlayed >= s_framesSubmitted ? 0 : (u32)(s_framesSubmitted - vs.SamplesPlayed);
        if (s_stretching) start *= 2;
        if (queued >= start || vs.BuffersQueued >= SLOTS - 1)
        {
            s_voice->Start(0, XAUDIO2_COMMIT_NOW);
            s_playing = 1;
        }
    }
    else if (!s_stretching)
    {
        float fill = (float)pending / (float)target;
        float want = 1.0f + RATE_GAIN * (fill - 1.0f);
        if (want < RATE_MIN) want = RATE_MIN;
        if (want > RATE_MAX) want = RATE_MAX;
        s_ratio += (want - s_ratio) * RATE_SMOOTH;
        if (fabsf(s_ratio - s_ratioApplied) > 0.002f)
        {
            s_voice->SetFrequencyRatio(s_ratio, XAUDIO2_COMMIT_NOW);
            s_ratioApplied = s_ratio;
        }
    }
    s_stats.fillMs = (u32)((u64)pending * 1000 / XB_AUDIO_RATE);
    s_stats.resyncs = s_resyncs;
    s_stats.ratioPermille = (u32)((s_stretching ? s_tempo : s_ratio) * 1000.0f + 0.5f);
    return (int)vs.BuffersQueued;
}

int xb_audio_feed(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void))
{
    int r;
    lock();
    r = feed(read, available);
    unlock();
    return r;
}

static u32 (*s_read)(s16 *out, u32 frames);
static u32 (*s_available)(void);

static DWORD WINAPI audio_thread(LPVOID)
{
    while (!s_threadQuit)
    {
        lock();
        if (s_read) feed(s_read, s_available);
        unlock();
        Sleep(4);
    }
    return 0;
}

int xb_audio_start_thread(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void), int stretch)
{
    HANDLE h;
    lock();
    s_read = read;
    s_available = available;
    s_stretchOn = stretch;
    s_stretching = 0;
    s_speed = s_tempo = 1.0f;
    s_speedT0 = 0;
    if (s_st) s_st->clear();
    if (stretch && !s_st)
    {
        s_st = new soundtouch::SoundTouch();
        s_st->setSampleRate(XB_AUDIO_RATE);
        s_st->setChannels(2);
        s_st->setSetting(SETTING_USE_QUICKSEEK, 1);
        s_st->setSetting(SETTING_USE_AA_FILTER, 0);
        s_st->setSetting(SETTING_SEQUENCE_MS, 40);
        s_st->setSetting(SETTING_SEEKWINDOW_MS, 15);
        s_st->setSetting(SETTING_OVERLAP_MS, 8);
    }
    unlock();
    if (s_thread) return 1;
    h = CreateThread(NULL, 64 * 1024, audio_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!h) return 0;
    XSetThreadProcessor(h, 5);
    ResumeThread(h);
    s_thread = h;
    return 1;
}

static int tone(int hz);

int xb_audio_tone(int hz)
{
    int r;
    lock();
    r = tone(hz);
    unlock();
    return r;
}

static int tone(int hz)
{
    XAUDIO2_VOICE_STATE vs;
    if (!s_voice) return 0;
    s_voice->GetState(&vs);
    if (hz <= 0)
    {
        if (s_playing)
        {
            s_voice->Stop(0);
            s_voice->FlushSourceBuffers();
            s_playing = 0;
        }
        return (int)vs.BuffersQueued;
    }
    while (vs.BuffersQueued < QUEUE_TARGET)
    {
        XAUDIO2_BUFFER b;
        s16 *out = s_slots[s_next];
        double step = 2.0 * 3.14159265358979 * hz / XB_AUDIO_RATE;
        int i;
        for (i = 0; i < SLOT_FRAMES; i++)
        {
            s16 v = (s16)(sin(s_phase) * 6000.0);
            out[i * 2] = v;
            out[i * 2 + 1] = v;
            s_phase += step;
            if (s_phase > 2.0 * 3.14159265358979) s_phase -= 2.0 * 3.14159265358979;
        }
        memset(&b, 0, sizeof(b));
        b.AudioBytes = sizeof(s_slots[0]);
        b.pAudioData = (const BYTE *)out;
        if (FAILED(s_voice->SubmitSourceBuffer(&b, NULL))) break;
        s_next = (s_next + 1) % SLOTS;
        vs.BuffersQueued++;
    }
    if (!s_playing)
    {
        s_voice->Start(0, XAUDIO2_COMMIT_NOW);
        s_playing = 1;
    }
    return (int)vs.BuffersQueued;
}
