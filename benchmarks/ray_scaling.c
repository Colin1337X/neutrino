#include "ray.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static double timestamp(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return (double)ticks.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
#endif
}

static float cpu_ray(const float *row)
{
    RAY_SLAB_BODY
}

static const char *dispatch(const char *backend, const float *rays, size_t count, float *hits)
{
#ifdef NEUTRINO_VULKAN
    if (!strcmp(backend, "vulkan")) return ray_vulkan(rays, count, hits, true);
#endif
    if (!strcmp(backend, "opencl")) return ray_gpu(rays, count, hits) ? "OpenCL GPU" : NULL;
    if (strcmp(backend, "cpu")) return NULL;
    for (size_t i = 0; i < count; ++i) hits[i] = cpu_ray(rays + i * RAY_VALUES);
    return "CPU";
}

int main(int argc, char **argv)
{
    const char *backend = argc > 1 ? argv[1] : "cpu";
    const size_t counts[] = {1, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    static float rays[4096 * RAY_VALUES], hits[4096];
    for (size_t i = 0; i < 4096; ++i) {
        float x = 2.0f + (float)(i % 11) * 0.25f;
        float row[RAY_VALUES] = {0, i % 2 ? 2.0f : 0, 0, 1, 0, 0, x, -1, -1, x + 1, 1, 1};
        memcpy(rays + i * RAY_VALUES, row, sizeof row);
    }

    double started = timestamp();
    const char *actual = dispatch(backend, rays, 1, hits);
    double first = timestamp() - started;
    if (!actual) {
        fprintf(stderr, "Requested backend unavailable: %s\n", backend);
        return 2;
    }

    puts("requested_backend,actual_backend,intersections,repetition,batches,mean_batch_us,rays_per_second,first_dispatch_ms");
    for (size_t size = 0; size < sizeof counts / sizeof counts[0]; ++size) {
        size_t count = counts[size];
        unsigned rounds = !strcmp(backend, "cpu") ? 3000 : 50;
        for (unsigned rep = 1; rep <= 5; ++rep) {
            started = timestamp();
            volatile float observed = 0;
            for (unsigned trial = 0; trial < rounds; ++trial) {
                if (!dispatch(backend, rays, count, hits)) return 3;
                observed += hits[trial % count];
            }
            double elapsed = timestamp() - started;
            if (!isfinite(observed) || elapsed <= 0) return 4;
            for (size_t i = 0; i < count; ++i) {
                float expected = i % 2 ? -1.0f : 2.0f + (float)(i % 11) * 0.25f;
                if (!isfinite(hits[i]) || fabsf(hits[i] - expected) > 0.00001f) {
                    fprintf(stderr, "%s case %zu ray %zu: got %g, expected %g\n", backend, count, i, hits[i], expected);
                    return 5;
                }
            }
            printf("%s,%s,%zu,%u,%u,%.6f,%.3f,%.6f\n",
                   backend, actual, count, rep, rounds, 1e6 * elapsed / rounds,
                   count * rounds / elapsed, 1000 * first);
        }
    }
    return 0;
}
