#ifndef NEUTRINO_PARALLEL_H
#define NEUTRINO_PARALLEL_H

#include "neutrino.h"

typedef struct Parallel Parallel;
typedef void (*ParallelRange)(void *context, size_t first, size_t end);

typedef enum {
    PARALLEL_CFD,
    PARALLEL_RAYCAST,
    PARALLEL_REFLECTIONS,
    PARALLEL_COLLISIONS,
    PARALLEL_RUNOFF,
    PARALLEL_SOFTBODY
} ParallelWorkload;

/* A group belongs to one caller. A dispatch is complete when it returns. */
Parallel *parallel_open(ParallelWorkload workload, uint64_t work, size_t units);
unsigned parallel_count(const Parallel *group);
unsigned parallel_override(void);
bool parallel_set_workers(const char *mode);
void parallel_mode(Text *out);
/* Diagnostics belong to the calling thread. Restore the previous sink after the operation. */
Text *parallel_trace_to(Text *out);
void parallel_trace_serial(ParallelWorkload workload, const char *reason);
void parallel_run(Parallel *group, size_t count, ParallelRange range, void *context);
void parallel_close(Parallel *group);

#endif
