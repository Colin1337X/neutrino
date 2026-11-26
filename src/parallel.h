#ifndef NEUTRINO_PARALLEL_H
#define NEUTRINO_PARALLEL_H

#include <stddef.h>

typedef struct Parallel Parallel;
typedef void (*ParallelRange)(void *context, size_t first, size_t end);

/* A group belongs to one caller. A dispatch is complete when it returns. */
Parallel *parallel_open(size_t work, size_t minimum_per_worker);
/* Use the system thread pool on Windows for jobs that require few dispatches. */
Parallel *parallel_open_batch(size_t work, size_t minimum_per_worker);
void parallel_run(Parallel *group, size_t count, ParallelRange range, void *context);
void parallel_close(Parallel *group);

#endif
