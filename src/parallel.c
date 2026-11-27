#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "parallel.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <inttypes.h>

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
#ifdef __linux__
#include <sched.h>
#endif
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

enum { MAX_WORKERS = 16 };

static const struct {
    const char *name, *measure, *partition;
    unsigned work_per_lane, automatic_limit;
    bool short_batch;
} policies[] = {
    [PARALLEL_CFD] = {"CFD", "grid cells", "grid planes", 3456, 4, false},
    [PARALLEL_RAYCAST] = {"Raycast", "ray–object tests", "rays", 32768, 16, true},
    [PARALLEL_REFLECTIONS] = {"Recursive reflections", "candidate work units", "candidate tiles", 4096, 4, true},
    [PARALLEL_COLLISIONS] = {"Rigid collisions", "potential pairs", "bodies", 576, 8, false},
    [PARALLEL_RUNOFF] = {"Runoff", "cell–spill pairs", "grid cells", 2048, 4, false},
    [PARALLEL_SOFTBODY] = {"Soft body", "nodes", "movable nodes", 8192, 8, false},
};

static _Thread_local Text *scheduler_trace;

Text *parallel_trace_to(Text *out)
{
    Text *previous = scheduler_trace;
    scheduler_trace = out;
    return previous;
}

void parallel_trace_serial(ParallelWorkload workload, const char *reason)
{
    if (scheduler_trace) format(scheduler_trace,"Physics scheduler · %s: used 1 worker (%s).\n",
                                policies[workload].name,reason);
}

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

static unsigned available_cpus(void)
{
#ifdef _WIN32
    DWORD_PTR allowed, system;
    if (GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system) && allowed) {
        unsigned count = 0;
        for (; allowed; allowed &= allowed-1) ++count;
        return count;
    }
    const unsigned count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count ? count : 1;
#else
#ifdef __linux__
    cpu_set_t allowed;
    if (!sched_getaffinity(0,sizeof allowed,&allowed)) {
        const int count = CPU_COUNT(&allowed);
        if (count > 0) return (unsigned)count;
    }
#endif
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? (unsigned)online : 1;
#endif
}

unsigned parallel_override(void)
{
    const char *setting = getenv("NEUTRINO_PHYSICS_WORKERS");
    if (setting && *setting) {
        char *end;
        const unsigned long requested = strtoul(setting,&end,10);
        if (!*end && requested > 0 && requested <= MAX_WORKERS)
            return (unsigned)requested;
    }
    return 0;
}

void parallel_mode(Text *out)
{
    const unsigned requested = parallel_override();
    if (requested) format(out,"Manual (%u requested)",requested);
    else append(out,"Automatic");
}

static unsigned worker_limit(ParallelWorkload workload, uint64_t work, size_t units,
                             unsigned requested, unsigned *cpus)
{
    if (requested) return requested < units ? requested : (unsigned)units;
    uint64_t lanes = work/policies[workload].work_per_lane;
    if (lanes > policies[workload].automatic_limit) lanes = policies[workload].automatic_limit;
    if (lanes > units) lanes = units;
    if (lanes < 2 && !scheduler_trace) return 1;
    *cpus = available_cpus();
    return lanes < *cpus ? (unsigned)lanes : *cpus;
}

static Parallel *open_workers(ParallelWorkload workload, unsigned lanes)
{
    if (lanes < 2) return nullptr;
    Parallel *group = calloc(1, sizeof *group);
    if (!group) return nullptr;
#ifdef _WIN32
    if (policies[workload].short_batch) {
        group->lanes = lanes;
        atomic_init(&group->next_lane,1);
        group->work = CreateThreadpoolWork(shared_worker,group,nullptr);
        if (group->work) return group;
        free(group);
        return nullptr;
    }
    InitializeSRWLock(&group->mutex);
    InitializeConditionVariable(&group->changed);
    InitializeConditionVariable(&group->finished);
#else
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

Parallel *parallel_open(ParallelWorkload workload, uint64_t work, size_t units)
{
    const unsigned requested = parallel_override();
    unsigned cpus = 0;
    const unsigned lanes = worker_limit(workload,work,units,requested,&cpus);
    Parallel *group = open_workers(workload,lanes);
    if (!scheduler_trace) return group;

    const unsigned workers = parallel_count(group);
    format(scheduler_trace,"Physics scheduler · %s: %u %s (%s; ",policies[workload].name,
           workers,workers == 1 ? "worker" : "workers",requested ? "manual" : "auto");
    if (requested) {
        format(scheduler_trace,"%u requested; %zu independent %s",requested,units,policies[workload].partition);
        if (requested > units) append(scheduler_trace," limited the request");
    } else {
        format(scheduler_trace,"%" PRIu64 " %s at %u per worker; %zu %s; job cap %u; %u available CPUs",
               work,policies[workload].measure,policies[workload].work_per_lane,
               units,policies[workload].partition,policies[workload].automatic_limit,cpus);
        if (work/policies[workload].work_per_lane < 2) append(scheduler_trace,"; too little work to split");
    }
    if (lanes > workers) append(scheduler_trace,"; worker startup fell back to fewer workers");
    append(scheduler_trace,").\n");
    return group;
}

unsigned parallel_count(const Parallel *group)
{
    return group ? group->lanes : 1;
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
