#ifndef NEUTRINO_RAY_H
#define NEUTRINO_RAY_H

#include <stdbool.h>
#include <stddef.h>

/* Three origin, direction, lower and upper coordinates, in that order.
   Direction defines the parameterization. A negative hit means a miss. */
enum { RAY_VALUES = 12 };
bool ray_gpu(const float *rays, size_t count, float *hits);

#define RAY_SLAB_BODY \
    float low = 0.0f, high = 3.402823466e38f; \
    for (int axis = 0; axis < 3; ++axis) { \
        float origin = row[axis], direction = row[3 + axis]; \
        float a = row[6 + axis] - origin, b = row[9 + axis] - origin; \
        if (direction > -1.0e-12f && direction < 1.0e-12f) { \
            if (a > 0.0f || b < 0.0f) return -1.0f; \
            continue; \
        } \
        a /= direction; b /= direction; \
        if (a > b) { float swap = a; a = b; b = swap; } \
        if (a > low) low = a; \
        if (b < high) high = b; \
        if (low > high) return -1.0f; \
    } \
    return low;

#endif
