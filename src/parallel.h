#ifndef NEUTRINO_PARALLEL_H
#define NEUTRINO_PARALLEL_H

#include <stddef.h>
#include <stdint.h>

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
void parallel_run(Parallel *group, size_t count, ParallelRange range, void *context);
void parallel_close(Parallel *group);

#endif
