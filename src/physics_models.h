#ifndef NEUTRINO_PHYSICS_MODELS_H
#define NEUTRINO_PHYSICS_MODELS_H

#include "neutrino.h"

bool physics_scalar(const Json *args, const char *key, double low, double high, double *value);
bool physics_vector(const Json *args, const char *key, double value[3]);
bool physics_optional(const Json *args, const char *key, double low, double high, double initial, double *value);
bool physics_model_tool(const char *name);
bool physics_model_run(const char *name, const Json *args, double gravity, Text *out, bool *unavailable);
void *physics_memory_alloc(size_t bytes, bool prefer_mapped, bool *mapped, const char **description);
void physics_memory_free(void *memory, bool mapped);

typedef struct {
    double length[3], center[3], wind[3];
    double seconds, sigma, density, viscosity, diffusivity, vortex;
    double sound_loss, light_loss;
    int resolution;
    bool cfd, mapped, gpu;
} FlowConfig;

typedef struct { double lo[3], hi[3]; } PhysicsBox;

typedef struct {
    FlowConfig config;
    double *grid;
    unsigned char *solid;
    double divergence, speed, tracer_mean;
    const char *method;
    const char *memory;
    const char *fallback;
    bool mapped;
} FlowField;

bool flow_parse(const Json *args, const double length[3], FlowConfig *config);
FlowField flow_solve(const FlowConfig *config);
FlowField flow_solve_domain(const FlowConfig *config, const PhysicsBox *solids, size_t count);
double flow_sample(const FlowField *field, const double local[3]);
double flow_column(const FlowField *field, const double a[3], const double b[3], const double origin[3],
                   double start, double end);
void flow_release(FlowField *field);
#endif
