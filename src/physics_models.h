#ifndef NEUTRINO_PHYSICS_MODELS_H
#define NEUTRINO_PHYSICS_MODELS_H

#include "neutrino.h"

bool physics_scalar(const Json *args, const char *key, double low, double high, double *value);
bool physics_vector(const Json *args, const char *key, double value[3]);
bool physics_optional(const Json *args, const char *key, double low, double high, double initial, double *value);
bool physics_model_tool(const char *name);
bool physics_model_run(const char *name, const Json *args, double gravity, Text *out, bool *unavailable);
typedef struct {
    double work, steps, seconds, rays, cells, nodes, ram, mapped, field_bytes;
} PhysicsCost;
PhysicsCost physics_cost(const Json *op, const Json *settings);
const Json *physics_sensors(const Json *settings);
bool physics_emitter(const Json *settings, const Json *args, double source[3]);
extern const char flow_frame_event[];
Json *physics_prepare_flow_step(const Json *settings, const Json *op, Text *out);
PhysicsCost softbody_cost(const Json *args);
PhysicsCost rigid_cost(const Json *op, const Json *room);
typedef struct {
    const char *method;
    double final_kg, transferred_kg, equilibrium_kg, gas_kg;
    bool closed;
} MassTransfer;
bool mass_transfer_tool(const char *name);
bool mass_transfer_run(const char *name, const Json *args, MassTransfer *result);
void *physics_memory_alloc(size_t bytes, bool prefer_mapped, bool *mapped, const char **description);
void physics_memory_free(void *memory, bool mapped);

typedef struct {
    double length[3], center[3], wind[3];
    double seconds, sigma, density, viscosity, diffusivity, vortex;
    double sound_loss, light_loss;
    double permeability, porosity, porous_min[3], porous_max[3], porous_drive[3];
    int resolution;
    bool cfd, mapped, gpu, porous, conservative, closed;
} FlowConfig;

typedef struct { double lo[3], hi[3]; } PhysicsBox;
enum { FLOW_MAX_RESOLUTION = 24, FLOW_MAX_CELLS = 24 * 24 * 24 };
bool rigid_edit(Json *room, Json *object, const Json *materials, const PhysicsBox *bounds,
                const Json *op, double gravity);
bool rigid_tool(const char *name);

typedef struct {
    FlowConfig config;
    double *grid;
    unsigned char *solid;
    double divergence, speed, tracer_mean, porous_speed, pore_volume;
    size_t porous_cells, unresolved_boxes;
    const char *method;
    const char *memory;
    const char *fallback;
    bool mapped;
} FlowField;

typedef struct PhysicsCache {
    FlowField flow;
    const Json *room;
} PhysicsCache;

void physics_cache_release(PhysicsCache *cache);

bool flow_parse(const Json *args, const double length[3], FlowConfig *config);
size_t flow_rasterize(const FlowConfig *config, const PhysicsBox *boxes, size_t count, unsigned char *solid);
PhysicsCost flow_cost(const FlowConfig *config);
PhysicsCost flow_step_cost(const FlowConfig *config, bool resumed);
size_t flow_frame_bytes(int resolution);
char *flow_encode(const FlowField *field);
FlowField flow_solve(const FlowConfig *config);
FlowField flow_solve_domain(const FlowConfig *config, const PhysicsBox *solids, size_t count);
FlowField flow_resume_domain(const FlowConfig *config, const PhysicsBox *solids, size_t count, const char *frame);
double flow_sample(const FlowField *field, const double local[3]);
double flow_column(const FlowField *field, const double a[3], const double b[3], const double origin[3],
                   double start, double end);
void flow_release(FlowField *field);
void flow_porous_summary(const FlowField *field, Text *out);
void flow_geometry_summary(const FlowField *field, Text *out);
#endif
