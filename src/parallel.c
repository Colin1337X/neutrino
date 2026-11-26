#include "parallel.h"

#include <stdatomic.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
typedef HANDLE Thread;
typedef SRWLOCK Lock;
typedef CONDITION_VARIABLE Event;
#define WORKER unsigned __stdcall
#define THREAD_RETURN 0
#define lock AcquireSRWLockExclusive
#define unlock ReleaseSRWLockExclusive
#define signal_event WakeAllConditionVariable
static void wait_event(Event *event, Lock *mutex)
{
    SleepConditionVariableSRW(event, mutex, INFINITE, 0);
}
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t Thread;
typedef pthread_mutex_t Lock;
typedef pthread_cond_t Event;
#define WORKER void *
#define THREAD_RETURN nullptr
#define lock pthread_mutex_lock
#define unlock pthread_mutex_unlock
#define wait_event pthread_cond_wait
#define signal_event pthread_cond_broadcast
#endif

enum { MAX_WORKERS = 16, DEFAULT_LIMIT = 4 };

typedef struct {
    Parallel *group;
    unsigned lane;
    Thread thread;
} Worker;

struct Parallel {
    Lock mutex;
    Event changed;
    Event finished;
    Worker workers[MAX_WORKERS - 1];
    unsigned lanes, pending;
    size_t epoch;
    bool stopping;
#ifdef _WIN32
    PTP_WORK work;
    atomic_uint next_lane;
#endif
    size_t count;
    ParallelRange range;
    void *context;
};

static void run_lane(const Parallel *group, unsigned lane)
{
    const size_t per_lane = group->count / group->lanes;
    const size_t remainder = group->count % group->lanes;
    const size_t first = lane * per_lane + (lane < remainder ? lane : remainder);
    const size_t end = first + per_lane + (lane < remainder);
    if (first < end) group->range(group->context, first, end);
}

static WORKER worker_main(void *argument)
{
    Worker *worker = argument;
    Parallel *group = worker->group;
    size_t seen = 0;
    lock(&group->mutex);
    for (;;) {
        while (seen == group->epoch && !group->stopping) wait_event(&group->changed, &group->mutex);
        if (group->stopping) break;
        seen = group->epoch;
        unlock(&group->mutex);
        run_lane(group, worker->lane);
        lock(&group->mutex);
        if (--group->pending == 0) signal_event(&group->finished);
    }
    unlock(&group->mutex);
    return THREAD_RETURN;
}

#ifdef _WIN32
static void CALLBACK shared_worker(PTP_CALLBACK_INSTANCE instance, void *argument, PTP_WORK work)
{
    (void)instance;
    (void)work;
    Parallel *group = argument;
    run_lane(group,atomic_fetch_add_explicit(&group->next_lane,1,memory_order_relaxed));
}
#endif

static unsigned worker_limit(void)
{
#ifdef _WIN32
    unsigned available = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned available = online > 0 ? (unsigned)online : 1;
#endif
    const char *setting = getenv("NEUTRINO_PHYSICS_WORKERS");
    if (setting && *setting) {
        char *end;
        unsigned long requested = strtoul(setting, &end, 10);
        if (!*end && requested > 0 && requested <= MAX_WORKERS) return (unsigned)requested;
    }
    return available < DEFAULT_LIMIT ? available : DEFAULT_LIMIT;
}

static Parallel *open_group(size_t work, size_t minimum_per_worker, bool shared)
{
    size_t lanes = work / minimum_per_worker;
    if (lanes < 2) return nullptr;
    const unsigned limit = worker_limit();
    if (lanes > limit) lanes = limit;
    if (lanes < 2) return nullptr;
    Parallel *group = calloc(1, sizeof *group);
    if (!group) return nullptr;
#ifdef _WIN32
    if (shared) {
        group->lanes = (unsigned)lanes;
        group->work = CreateThreadpoolWork(shared_worker,group,nullptr);
        if (group->work) return group;
        free(group);
        return nullptr;
    }
    InitializeSRWLock(&group->mutex);
    InitializeConditionVariable(&group->changed);
    InitializeConditionVariable(&group->finished);
#else
    (void)shared;
    if (pthread_mutex_init(&group->mutex, nullptr)) { free(group); return nullptr; }
    if (pthread_cond_init(&group->changed, nullptr)) {
        pthread_mutex_destroy(&group->mutex);
        free(group);
        return nullptr;
    }
    if (pthread_cond_init(&group->finished, nullptr)) {
        pthread_cond_destroy(&group->changed);
        pthread_mutex_destroy(&group->mutex);
        free(group);
        return nullptr;
    }
#endif
    for (group->lanes = 1; group->lanes < lanes; ++group->lanes) {
        Worker *worker = &group->workers[group->lanes - 1];
        worker->lane = group->lanes;
        worker->group = group;
#ifdef _WIN32
        worker->thread = (HANDLE)_beginthreadex(nullptr,0,worker_main,worker,0,nullptr);
        if (!worker->thread) break;
#else
        if (pthread_create(&worker->thread, nullptr, worker_main, worker)) break;
#endif
    }
    if (group->lanes > 1) return group;
    parallel_close(group);
    return nullptr;
}

Parallel *parallel_open(size_t work, size_t minimum_per_worker)
{
    return open_group(work,minimum_per_worker,false);
}

Parallel *parallel_open_batch(size_t work, size_t minimum_per_worker)
{
    return open_group(work,minimum_per_worker,true);
}

void parallel_run(Parallel *group, size_t count, ParallelRange range, void *context)
{
    if (!group || count < group->lanes) {
        if (count) range(context, 0, count);
        return;
    }
#ifdef _WIN32
    if (group->work) {
        group->count = count;
        group->range = range;
        group->context = context;
        atomic_store_explicit(&group->next_lane,1,memory_order_relaxed);
        for (unsigned i = 0; i < group->lanes - 1; ++i) SubmitThreadpoolWork(group->work);
        run_lane(group,0);
        WaitForThreadpoolWorkCallbacks(group->work,FALSE);
        return;
    }
#endif
    lock(&group->mutex);
    group->count = count;
    group->range = range;
    group->context = context;
    group->pending = group->lanes - 1;
    ++group->epoch;
    signal_event(&group->changed);
    unlock(&group->mutex);
    run_lane(group, 0);
    lock(&group->mutex);
    while (group->pending) wait_event(&group->finished, &group->mutex);
    unlock(&group->mutex);
}

void parallel_close(Parallel *group)
{
    if (!group) return;
#ifdef _WIN32
    if (group->work) {
        CloseThreadpoolWork(group->work);
        free(group);
        return;
    }
#endif
    lock(&group->mutex);
    group->stopping = true;
    signal_event(&group->changed);
    unlock(&group->mutex);
    for (unsigned i = 0; i < group->lanes - 1; ++i) {
#ifdef _WIN32
        WaitForSingleObject(group->workers[i].thread, INFINITE);
        CloseHandle(group->workers[i].thread);
#else
        pthread_join(group->workers[i].thread, nullptr);
#endif
    }
#ifndef _WIN32
    pthread_cond_destroy(&group->finished);
    pthread_cond_destroy(&group->changed);
    pthread_mutex_destroy(&group->mutex);
#endif
    free(group);
}
