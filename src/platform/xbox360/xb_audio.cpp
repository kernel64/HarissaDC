// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "xb_audio.h"

#include <xtl.h>
#include <xaudio2.h>
#include <math.h>
#include <string.h>

#include "../../core/log.h"
#include "../../hw/aica.h"
#include "../../hw/cdda.h"

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

#define CD_SLOT_FRAMES 512
#define CD_SLOTS 8
#define CD_QUEUED 4
#define CD_IDLE_MS 100
static IXAudio2SourceVoice *s_cdVoice;
static s16 s_cdSlots[CD_SLOTS][CD_SLOT_FRAMES * 2];
static int s_cdNext, s_cdPlaying;
static DWORD s_lastInMs;
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

int g_xb_audio_measure;
u32 g_xb_audio_rms[32], g_xb_audio_rms_count;

static void measure_slot(const s16 *p)
{
    u64 sum = 0;
    u32 i;
    if (!g_xb_audio_measure) return;
    for (i = 0; i < SLOT_FRAMES * 2; i++) sum += (u64)((s32)p[i] * p[i]);
    g_xb_audio_rms[g_xb_audio_rms_count & 31] = (u32)sqrt((double)sum / (SLOT_FRAMES * 2.0));
    g_xb_audio_rms_count++;
}

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
    if (FAILED(s_xa->CreateSourceVoice(&s_cdVoice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)))
    {
        LOG_WARN("[audio] CreateSourceVoice failed for the CD voice: CD audio through the AICA only");
        s_cdVoice = NULL;
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
    if (s_cdVoice)
    {
        s_cdVoice->Stop(0);
        s_cdVoice->FlushSourceBuffers();
        s_cdVoice->DestroyVoice();
        s_cdVoice = NULL;
    }
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
#define STRETCH_MAX 1.04f
#define STRETCH_GAIN 0.30f
#define STRETCH_IN 2048
#define RESERVE_FALL_MS 15

static soundtouch::SoundTouch *s_st;
static int s_stretchOn;
static int s_stretching;
static float s_speed = 1.0f, s_tempo = 1.0f;
static u32 s_speedFrames, s_speedT0;
static s16 s_in[STRETCH_IN * 2];
static u32 s_resyncs;
static u32 s_reserve, s_reserveT;
static u32 s_drop;

static u32 backlog(const XAUDIO2_VOICE_STATE *vs)
{
    u32 n = vs->SamplesPlayed >= s_framesSubmitted ? 0 : (u32)(s_framesSubmitted - vs->SamplesPlayed);
    if (s_stretching && s_st) n += s_st->numSamples() + (u32)((float)s_st->numUnprocessedSamples() / s_tempo);
    return n;
}

static void speed_note(u32 frames);

static void resync(u32 excess)
{
    u32 held = 0;
    if (s_stretching && s_st)
    {
        held = s_st->numSamples() + s_st->numUnprocessedSamples();
        s_st->clear();
    }
    s_drop = excess > held ? excess - held : 0;
    s_resyncs++;
    LOG_INFO("[audio] %u ms of sound ahead dropped (it lagged behind the picture)", (u32)((u64)excess * 1000 / XB_AUDIO_RATE));
}

static void drop_frames(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void))
{
    u32 n;
    while (s_drop && (n = available()) > 0)
    {
        if (n > STRETCH_IN) n = STRETCH_IN;
        n = read(s_in, n < s_drop ? n : s_drop);
        if (!n) break;
        if (s_st && s_stretchOn) speed_note(n);
        s_drop -= n;
    }
}

static u32 reserve(u32 target)
{
    u32 now = GetTickCount();
    if (s_stretching) s_reserve = target * 2;
    else if (s_reserve > target)
    {
        u32 dt = now - s_reserveT, fall;
        if (dt < 100) return s_reserve;
        fall = (u32)((u64)dt * RESERVE_FALL_MS * XB_AUDIO_RATE / 1000000);
        s_reserve = s_reserve > target + fall ? s_reserve - fall : target;
    }
    else s_reserve = target;
    s_reserveT = now;
    return s_reserve;
}

static int submit(XAUDIO2_VOICE_STATE *vs, u32 frames)
{
    XAUDIO2_BUFFER b;
    s16 *out = s_slots[s_next];
    if (frames < SLOT_FRAMES) memset(out + frames * 2, 0, (SLOT_FRAMES - frames) * 2 * sizeof(s16));
    measure_slot(out);
    memset(&b, 0, sizeof(b));
    b.AudioBytes = frames * 2 * sizeof(s16);
    b.pAudioData = (const BYTE *)out;
    if (FAILED(s_voice->SubmitSourceBuffer(&b, NULL))) return 0;
    s_next = (s_next + 1) % SLOTS;
    s_framesSubmitted += frames;
    vs->BuffersQueued++;
    return 1;
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
        s_st->receiveSamples(s_slots[s_next], SLOT_FRAMES);
        if (!submit(vs, SLOT_FRAMES)) break;
    }
}

static void stretch_end(XAUDIO2_VOICE_STATE *vs)
{
    s_st->flush();
    while (vs->BuffersQueued < SLOTS - 1)
    {
        u32 n = s_st->receiveSamples(s_slots[s_next], SLOT_FRAMES);
        if (!n || !submit(vs, n)) break;
    }
    s_st->clear();
    s_stretching = 0;
    s_reserveT = GetTickCount();
}

static int feed(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void))
{
    XAUDIO2_VOICE_STATE vs;
    u32 pending, target, start, aim;
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
        else if (s_stretching && s_speed > STRETCH_OFF && backlog(&vs) <= target * 2 + target / 2)
        {
            stretch_end(&vs);
            LOG_INFO("[audio] time stretching off");
        }
    }
    aim = reserve(target);
    if (s_playing)
    {
        u32 b = backlog(&vs) + available();
        if (s_drop)
        {
            if (b <= aim) s_drop = 0;
        }
        else if (b > aim * 2 && b > aim + XB_AUDIO_RATE * 15 / 100) resync(b - aim);
    }
    else s_drop = 0;
    drop_frames(read, available);
    if (s_stretching) stretch_feed(read, available, &vs, aim);
    while (!s_stretching && vs.BuffersQueued < SLOTS - 1 && available() >= SLOT_FRAMES)
    {
        if (s_st && s_stretchOn) speed_note(SLOT_FRAMES);
        read(s_slots[s_next], SLOT_FRAMES);
        if (!submit(&vs, SLOT_FRAMES)) break;
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
        float fill = (float)pending / (float)aim;
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

static void cd_feed(u32 (*available)(void))
{
    XAUDIO2_VOICE_STATE vs;
    if (!s_cdVoice) return;
    if (available() > 0) s_lastInMs = GetTickCount();
    if (!g_cdda_realtime || GetTickCount() - s_lastInMs > CD_IDLE_MS)
    {
        if (s_cdPlaying) s_cdVoice->Stop(0);
        s_cdPlaying = 0;
        return;
    }
    s_cdVoice->GetState(&vs);
    while (vs.BuffersQueued < CD_QUEUED)
    {
        XAUDIO2_BUFFER b;
        s16 *o = s_cdSlots[s_cdNext];
        u32 n = cdda_rt_read(o, CD_SLOT_FRAMES), i;
        s32 g[4];
        if (!n) break;
        aica_cdda_gains(g);
        for (i = 0; i < n; i++)
        {
            s32 l = o[i * 2], r = o[i * 2 + 1];
            s32 L = (s32)(((s64)l * g[0] + (s64)r * g[2]) >> 16), Rr = (s32)(((s64)l * g[1] + (s64)r * g[3]) >> 16);
            o[i * 2] = (s16)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
            o[i * 2 + 1] = (s16)(Rr > 32767 ? 32767 : Rr < -32768 ? -32768 : Rr);
        }
        memset(&b, 0, sizeof(b));
        b.AudioBytes = n * 4;
        b.pAudioData = (const BYTE *)o;
        if (FAILED(s_cdVoice->SubmitSourceBuffer(&b, NULL))) break;
        s_cdNext = (s_cdNext + 1) % CD_SLOTS;
        vs.BuffersQueued++;
    }
    if (!s_cdPlaying && vs.BuffersQueued)
    {
        s_cdVoice->Start(0);
        s_cdPlaying = 1;
    }
}

int xb_audio_feed(u32 (*read)(s16 *out, u32 frames), u32 (*available)(void))
{
    int r;
    lock();
    cd_feed(available);
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
        if (s_read)
        {
            cd_feed(s_available);
            feed(s_read, s_available);
        }
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
    s_reserve = 0;
    s_drop = 0;
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
