// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "thread.h"

#include <stdlib.h>

#if defined(_XBOX)
#include <xtl.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

#ifdef _WIN32

struct Thread
{
    HANDLE handle;
    ThreadFn fn;
    void *arg;
};

struct Event
{
    HANDLE handle;
};

static DWORD WINAPI thread_entry(LPVOID p)
{
    Thread *t = (Thread *)p;
    t->fn(t->arg);
    return 0;
}

Thread *thread_start(ThreadFn fn, void *arg, int hwThread)
{
    Thread *t = (Thread *)calloc(1, sizeof(Thread));
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 256 * 1024, thread_entry, t, CREATE_SUSPENDED, NULL);
    if (!t->handle)
    {
        free(t);
        return NULL;
    }
#ifdef _XBOX
    if (hwThread >= 0 && hwThread < 6) XSetThreadProcessor(t->handle, (DWORD)hwThread);
#else
    (void)hwThread;
#endif
    ResumeThread(t->handle);
    return t;
}

void thread_join(Thread *t)
{
    if (!t) return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

void thread_yield(void)
{
    SwitchToThread();
}

Event *event_create(void)
{
    Event *e = (Event *)calloc(1, sizeof(Event));
    if (!e) return NULL;
    e->handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    return e;
}

void event_destroy(Event *e)
{
    if (!e) return;
    CloseHandle(e->handle);
    free(e);
}

void event_signal(Event *e)
{
    SetEvent(e->handle);
}

int event_wait(Event *e, int ms)
{
    return WaitForSingleObject(e->handle, (DWORD)ms) == WAIT_OBJECT_0;
}

void thread_barrier(void)
{
    MemoryBarrier();
}

struct Mutex
{
    CRITICAL_SECTION cs;
};

Mutex *mutex_create(void)
{
    Mutex *m = (Mutex *)calloc(1, sizeof(Mutex));
    if (m) InitializeCriticalSection(&m->cs);
    return m;
}

void mutex_destroy(Mutex *m)
{
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

void mutex_lock(Mutex *m) { EnterCriticalSection(&m->cs); }
void mutex_unlock(Mutex *m) { LeaveCriticalSection(&m->cs); }

#else

struct Thread
{
    pthread_t id;
    ThreadFn fn;
    void *arg;
};

struct Event
{
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int set;
};

static void *thread_entry(void *p)
{
    Thread *t = (Thread *)p;
    t->fn(t->arg);
    return NULL;
}

Thread *thread_start(ThreadFn fn, void *arg, int hwThread)
{
    Thread *t = (Thread *)calloc(1, sizeof(Thread));
    (void)hwThread;
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->id, NULL, thread_entry, t) != 0)
    {
        free(t);
        return NULL;
    }
    return t;
}

void thread_join(Thread *t)
{
    if (!t) return;
    pthread_join(t->id, NULL);
    free(t);
}

void thread_yield(void)
{
    sched_yield();
}

Event *event_create(void)
{
    Event *e = (Event *)calloc(1, sizeof(Event));
    if (!e) return NULL;
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->cond, NULL);
    return e;
}

void event_destroy(Event *e)
{
    if (!e) return;
    pthread_cond_destroy(&e->cond);
    pthread_mutex_destroy(&e->lock);
    free(e);
}

void event_signal(Event *e)
{
    pthread_mutex_lock(&e->lock);
    e->set = 1;
    pthread_cond_signal(&e->cond);
    pthread_mutex_unlock(&e->lock);
}

int event_wait(Event *e, int ms)
{
    struct timespec ts;
    int r;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&e->lock);
    while (!e->set)
        if (pthread_cond_timedwait(&e->cond, &e->lock, &ts) == ETIMEDOUT) break;
    r = e->set;
    e->set = 0;
    pthread_mutex_unlock(&e->lock);
    return r;
}

void thread_barrier(void)
{
    __sync_synchronize();
}

struct Mutex
{
    pthread_mutex_t lock;
};

Mutex *mutex_create(void)
{
    Mutex *m = (Mutex *)calloc(1, sizeof(Mutex));
    if (m) pthread_mutex_init(&m->lock, NULL);
    return m;
}

void mutex_destroy(Mutex *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->lock);
    free(m);
}

void mutex_lock(Mutex *m) { pthread_mutex_lock(&m->lock); }
void mutex_unlock(Mutex *m) { pthread_mutex_unlock(&m->lock); }

#endif
