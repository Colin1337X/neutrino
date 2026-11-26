#include "physics_models.h"
#include "ray.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { FLOW_STEPS = 96, PRESSURE_SWEEPS = 32 };

bool physics_optional(const Json *args, const char *key, double low, double high, double initial, double *value)
{
    *value = initial;
    return !at(args, key) || physics_scalar(args, key, low, high, value);
}

bool flow_parse(const Json *args, const double length[3], FlowConfig *config)
{
    *config = (FlowConfig){0};
    double resolution;
    const char *method = js(args, "method"), *memory = js(args, "memory");
    const char *backend = js(args,"backend");
    config->cfd = !strcmp(method, "navier_stokes") || !strcmp(method, "cfd");
    config->mapped = !strcmp(memory, "mapped");
    config->gpu = !strcmp(backend,"gpu");
    if ((at(args, "method") && !config->cfd && strcmp(method, "reduced")) ||
        (at(args, "memory") && !config->mapped && strcmp(memory, "auto")) ||
        (at(args, "backend") && !config->gpu && strcmp(backend,"cpu") && strcmp(backend,"auto")) ||
        !physics_optional(args, "seconds", 0, 600, 1, &config->seconds) ||
        !physics_optional(args, "sigma_m", .001, 1000, .5, &config->sigma) ||
        !physics_optional(args, "concentration", 0, 1000, 1, &config->density) ||
        !physics_optional(args, "viscosity", 0, 10, .0001, &config->viscosity) ||
        !physics_optional(args, "diffusivity", 0, 10, .0001, &config->diffusivity) ||
        !physics_optional(args, "vortex_m_s", 0, 25, 0, &config->vortex) ||
        !physics_optional(args, "resolution", 6, 24, 12, &resolution) || floor(resolution) != resolution ||
        !physics_optional(args, "sound_loss", 0, 1000, 0, &config->sound_loss) ||
        !physics_optional(args, "light_loss", 0, 1000, 0, &config->light_loss)) return false;
    config->resolution = (int)resolution;
    if (at(args, "wind") && !physics_vector(args, "wind", config->wind)) return false;
    for (int axis = 0; axis < 3; ++axis) config->center[axis] = length[axis] / 2;
    if (at(args, "center") && !physics_vector(args, "center", config->center)) return false;
    for (int axis = 0; axis < 3; ++axis) {
        if (length[axis] < .01 || length[axis] > 1000 || fabs(config->wind[axis]) > 25 ||
            config->center[axis] < 0 || config->center[axis] >= length[axis]) return false;
        config->length[axis] = length[axis];
        config->porous_max[axis] = length[axis];
    }
    const Json *porous = at(args, "porous");
    if (!porous) return true;
    if (!config->cfd || config->viscosity <= 0 || !cJSON_IsObject(porous) ||
        !physics_scalar(porous, "permeability_m2", 1e-16, 1e6, &config->permeability) ||
        !physics_scalar(porous, "porosity", .001, 1, &config->porosity) ||
        (at(porous, "min") && !physics_vector(porous, "min", config->porous_min)) ||
        (at(porous, "max") && !physics_vector(porous, "max", config->porous_max)) ||
        (at(porous, "drive_m_s2") && !physics_vector(porous, "drive_m_s2", config->porous_drive))) return false;
    for (int axis = 0; axis < 3; ++axis)
        if (config->porous_min[axis] < 0 || config->porous_max[axis] > length[axis] ||
            config->porous_max[axis] <= config->porous_min[axis] || fabs(config->porous_drive[axis]) > 25) return false;
    config->porous = true;
    return true;
}

void *physics_memory_alloc(size_t bytes, bool prefer_mapped, bool *mapped, const char **description)
{
    *mapped = false;
#ifdef _WIN32
    if (!prefer_mapped) {
        void *memory = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) {
            *description = "pageable system memory";
            return memory;
        }
    }
    wchar_t directory[MAX_PATH], name[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, directory);
    if (!length || length >= MAX_PATH || !GetTempFileNameW(directory, L"ncf", 0, name)) return NULL;
    HANDLE file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(name);
        return NULL;
    }
    uint64_t length_bytes = bytes;
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READWRITE,
                                        (DWORD)(length_bytes >> 32), (DWORD)length_bytes, NULL);
    void *memory = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, bytes) : NULL;
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    if (memory) {
        *mapped = true;
        *description = "temporary file mapping";
    }
    return memory;
#else
    if (prefer_mapped) return NULL;
    *description = "pageable system memory";
    return calloc(1, bytes);
#endif
}

void physics_memory_free(void *memory, bool mapped)
{
    if (!memory) return;
#ifdef _WIN32
    if (mapped) UnmapViewOfFile(memory);
    else VirtualFree(memory, 0, MEM_RELEASE);
#else
    (void)mapped;
    free(memory);
#endif
}

void flow_release(FlowField *field)
{
    physics_memory_free(field->grid, field->mapped);
    free(field->solid);
    field->grid = NULL;
    field->solid = NULL;
}

static size_t index3(int n, int x, int y, int z)
{
    return (size_t)((z * n + y) * n + x);
}

static double sample_grid(const double *data, const unsigned char *solid, int n, double x, double y, double z)
{
    double coordinates[3] = {x,y,z}, fraction[3];
    int low[3], high[3];
    for (int axis = 0; axis < 3; ++axis) {
        if (!isfinite(coordinates[axis])) return NAN;
        double wrapped = coordinates[axis] - n * floor(coordinates[axis] / n);
        low[axis] = (int)wrapped % n;
        high[axis] = (low[axis] + 1) % n;
        fraction[axis] = wrapped - floor(wrapped);
    }
    double result = 0, included = 0;
    for (int corner = 0; corner < 8; ++corner) {
        int cell[3];
        double weight = 1;
        for (int axis = 0; axis < 3; ++axis) {
            bool upper = (corner & (1 << axis)) != 0;
            cell[axis] = upper ? high[axis] : low[axis];
            weight *= upper ? fraction[axis] : 1 - fraction[axis];
        }
        size_t i = index3(n, cell[0], cell[1], cell[2]);
        if (solid && solid[i]) continue;
        result += weight * data[i];
        included += weight;
    }
    return solid && included > 0 ? result / included : result;
}

static double gaussian(const FlowConfig *config, const double local[3], double time)
{
    double radius2 = 0, variance = config->sigma * config->sigma + 2 * config->diffusivity * time;
    for (int axis = 0; axis < 3; ++axis) {
        const double displacement = remainder(local[axis] - config->center[axis] -
                                              config->wind[axis] * time, config->length[axis]);
        radius2 += displacement * displacement;
    }
    return config->density * pow(config->sigma / sqrt(variance), 3) * exp(-radius2 / (2 * variance));
}

double flow_sample(const FlowField *field, const double local[3])
{
    const FlowConfig *c = &field->config;
    const int n = c->resolution;
    if (!field->grid) return gaussian(c, local, c->seconds);
    if (field->solid) {
        int cell[3];
        for (int axis = 0; axis < 3; ++axis) {
            if (!isfinite(local[axis]) || local[axis] < 0 || local[axis] >= c->length[axis]) return 0;
            cell[axis] = (int)fmin(n - 1, local[axis] * n / c->length[axis]);
        }
        if (field->solid[index3(n, cell[0], cell[1], cell[2])]) return 0;
    }
    return sample_grid(field->grid, field->solid, n, local[0] * n / c->length[0] - .5,
                       local[1] * n / c->length[1] - .5, local[2] * n / c->length[2] - .5);
}

double flow_column(const FlowField *field, const double a[3], const double b[3], const double origin[3],
                   double start, double end)
{
    if (!field || end <= start) return 0;
    double delta[3], length2 = 0, sum = 0;
    for (int axis = 0; axis < 3; ++axis) {
        delta[axis] = b[axis] - a[axis];
        length2 += delta[axis] * delta[axis];
    }
    enum { SAMPLES = 32 };
    for (int i = 0; i < SAMPLES; ++i) {
        const double t = start + (end - start) * (i + .5) / SAMPLES;
        double local[3];
        for (int axis = 0; axis < 3; ++axis) local[axis] = a[axis] - origin[axis] + t * delta[axis];
        sum += flow_sample(field, local);
    }
    return sqrt(length2) * (end - start) * sum / SAMPLES;
}

static void neighbors(int n, int x, int y, int z, size_t low[3], size_t high[3])
{
    low[0] = index3(n, (x+n-1)%n, y, z);
    low[1] = index3(n, x, (y+n-1)%n, z);
    low[2] = index3(n, x, y, (z+n-1)%n);
    high[0] = index3(n, (x+1)%n, y, z);
    high[1] = index3(n, x, (y+1)%n, z);
    high[2] = index3(n, x, y, (z+1)%n);
}

static double divergence_at(double *const v[3], size_t i, const size_t high[3], const double inv[3])
{
    return (v[0][high[0]]-v[0][i])*inv[0] + (v[1][high[1]]-v[1][i])*inv[1] +
           (v[2][high[2]]-v[2][i])*inv[2];
}

/* Compatible forward/backward difference pair gives a seven-point Poisson stencil. */
static void close_faces(int n, const unsigned char *solid, double *const v[3])
{
    if (!solid) return;
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi);
        for (int axis=0; axis<3; ++axis)
            if (solid[i] || solid[lo[axis]]) v[axis][i] = 0;
    }
}

static bool project(int n, const unsigned char *solid, const double inv[3], double *const v[3],
                    double *rhs, double *a, double *b, bool hardware)
{
    const size_t cells = (size_t)n*n*n;
    const double w[3] = {inv[0]*inv[0], inv[1]*inv[1], inv[2]*inv[2]};
    const double diagonal = 2 * (w[0]+w[1]+w[2]);
    close_faces(n, solid, v);
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi);
        rhs[i] = solid && solid[i] ? 0 : divergence_at(v, i, hi, inv);
    }
    const bool accelerated = hardware && !solid && flow_pressure_gpu(n,w,rhs,a,PRESSURE_SWEEPS);
    if (!accelerated) memset(a,0,cells*sizeof *a);
    for (int pass=0; !accelerated && pass<PRESSURE_SWEEPS; ++pass) {
        for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
            size_t i=index3(n,x,y,z), lo[3], hi[3];
            neighbors(n,x,y,z,lo,hi);
            if (solid && solid[i]) { b[i] = 0; continue; }
            double adjacent = 0, open_diagonal = 0;
            for (int axis=0; axis<3; ++axis) {
                if (!solid || !solid[lo[axis]]) { adjacent += w[axis]*a[lo[axis]]; open_diagonal += w[axis]; }
                if (!solid || !solid[hi[axis]]) { adjacent += w[axis]*a[hi[axis]]; open_diagonal += w[axis]; }
            }
            double divisor = solid ? open_diagonal : diagonal;
            b[i] = divisor > 0 ? a[i] + (2.0/3) * ((adjacent-rhs[i])/divisor - a[i]) : 0;
        }
        double *swap=a; a=b; b=swap;
    }
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi);
        for (int axis=0; axis<3; ++axis)
            if (!solid || (!solid[i] && !solid[lo[axis]])) v[axis][i] -= (a[i]-a[lo[axis]])*inv[axis];
    }
    return accelerated;
}

static void transport(int n, const unsigned char *solid, const double inv[3], double dt, double coefficient, const double *in,
                      double *out, double *scratch, double *const velocity[3])
{
    if (coefficient) {
        for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
            size_t i=index3(n,x,y,z), lo[3], hi[3];
            neighbors(n,x,y,z,lo,hi);
            if (solid && solid[i]) { scratch[i] = 0; continue; }
            double laplacian = 0;
            for (int axis=0; axis<3; ++axis)
                laplacian += ((solid && solid[lo[axis]] ? in[i] : in[lo[axis]]) +
                              (solid && solid[hi[axis]] ? in[i] : in[hi[axis]]) - 2*in[i])*inv[axis]*inv[axis];
            scratch[i] = in[i] + dt*coefficient*laplacian;
        }
        in = scratch;
    }
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z);
        out[i] = solid && solid[i] ? 0 : sample_grid(in,solid,n,x-dt*velocity[0][i]*inv[0],
                            y-dt*velocity[1][i]*inv[1],z-dt*velocity[2][i]*inv[2]);
    }
}

FlowField flow_solve_domain(const FlowConfig *c, const PhysicsBox *boxes, size_t box_count)
{
    FlowField field = {.config = *c, .method = c->porous ? "porous CFD unavailable" : "reduced Gaussian transport",
                       .memory = c->porous ? "none" : "analytic"};
    if (!c->cfd) return field;
    const int n = c->resolution;
    const size_t cells = (size_t)n*n*n;
    double inv[3], inverse2 = 0, max_courant = 0;
    double drag = c->porous ? c->viscosity / c->permeability : 0;
    for (int axis=0; axis<3; ++axis) {
        inv[axis] = n/c->length[axis];
        inverse2 += inv[axis]*inv[axis];
        max_courant += (fabs(c->wind[axis])+c->vortex)*inv[axis];
        if (c->porous) max_courant += fabs(c->porous_drive[axis])*
            (drag > 0 ? fmin(c->seconds, 1/drag) : c->seconds)*inv[axis];
    }
    const double steps_needed = fmax(1, ceil(c->seconds * fmax(2*max_courant,
                                                  4*fmax(c->viscosity,c->diffusivity)*inverse2)));
    if (steps_needed > FLOW_STEPS) {
        field.fallback = "CFD timestep budget exceeded";
        return field;
    }
    field.grid = physics_memory_alloc(11*cells*sizeof(double), c->mapped, &field.mapped, &field.memory);
    if (!field.grid) {
        field.fallback = "CFD memory allocation unavailable";
        field.memory = "analytic";
        return field;
    }
    if (box_count) {
        field.solid = calloc(cells, sizeof *field.solid);
        if (!field.solid) {
            flow_release(&field);
            field.fallback = "CFD obstacle mask allocation unavailable";
            field.memory = "analytic";
            return field;
        }
    }
    unsigned char *porous = c->porous ? calloc(cells, 1) : nullptr;
    if (c->porous && !porous) {
        flow_release(&field);
        field.fallback = "CFD porous mask allocation unavailable";
        field.memory = "none";
        return field;
    }
    double *velocity[3] = {field.grid+cells, field.grid+2*cells,field.grid+3*cells};
    double *next[3] = {field.grid+4*cells,field.grid+5*cells,field.grid+6*cells};
    double *dye_next=field.grid+7*cells, *rhs=field.grid+8*cells, *p=field.grid+9*cells, *q=field.grid+10*cells;
    size_t fluid_cells = 0;
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z);
        double point[3] = {(x+.5)/inv[0],(y+.5)/inv[1],(z+.5)/inv[2]};
        bool solid = false;
        for (size_t j = 0; j < box_count && !solid; ++j) {
            solid = true;
            for (int axis = 0; axis < 3; ++axis)
                solid &= point[axis] >= boxes[j].lo[axis] && point[axis] < boxes[j].hi[axis];
        }
        if (solid) { field.solid[i] = 1; continue; }
        if (porous) {
            bool inside = true;
            for (int axis = 0; axis < 3; ++axis)
                inside &= point[axis] >= c->porous_min[axis] && point[axis] < c->porous_max[axis];
            porous[i] = inside;
            field.porous_cells += inside;
        }
        ++fluid_cells;
        const double tau = 6.283185307179586;
        velocity[0][i] = c->wind[0] + c->vortex*sin(tau*(y+.5)/n);
        velocity[1][i] = c->wind[1] + c->vortex*sin(tau*(z+.5)/n);
        velocity[2][i] = c->wind[2] + c->vortex*sin(tau*(x+.5)/n);
        field.grid[i] = gaussian(c,point,0);
    }
    if (!fluid_cells || (field.solid && fluid_cells == cells) || (porous && !field.porous_cells)) {
        free(porous);
        flow_release(&field);
        field.fallback = !fluid_cells || (box_count && fluid_cells == cells) ?
            "CFD geometry has no resolved fluid/solid interface at this resolution" :
            "CFD porous region has no fluid cells at this resolution";
        field.memory = c->porous ? "none" : "analytic";
        return field;
    }
    close_faces(n, field.solid, velocity);
    const double dt=c->seconds/steps_needed;
    double decay = exp(-drag*dt), force_integral = drag > 0 ? -expm1(-drag*dt)/drag : dt;
    bool gpu = c->gpu, used_gpu = false;
    for (int step=0; dt>0 && step<(int)steps_needed; ++step) {
        for (int axis=0; axis<3; ++axis)
            transport(n,field.solid,inv,dt,c->viscosity,velocity[axis],next[axis],rhs,velocity);
        if (porous) {
            for (int axis = 0; axis < 3; ++axis)
                for (size_t i = 0; i < cells; ++i)
                    if (porous[i]) next[axis][i] = next[axis][i]*decay + c->porous_drive[axis]*force_integral;
        }
        gpu = project(n,field.solid,inv,next,rhs,p,q,gpu);
        used_gpu |= gpu;
        transport(n,field.solid,inv,dt,c->diffusivity,field.grid,dye_next,rhs,next);
        memcpy(field.grid,dye_next,cells*sizeof *field.grid);
        for (int axis=0; axis<3; ++axis) {
            double *swap=velocity[axis]; velocity[axis]=next[axis]; next[axis]=swap;
        }
    }
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        if (field.solid && field.solid[i]) continue;
        neighbors(n,x,y,z,lo,hi);
        double div=divergence_at(velocity,i,hi,inv);
        field.divergence += div*div;
        double speed = hypot(hypot(velocity[0][i],velocity[1][i]),velocity[2][i]);
        field.speed += speed;
        if (porous && porous[i]) field.porous_speed += speed;
        field.tracer_mean += field.grid[i];
    }
    field.divergence = sqrt(field.divergence/fluid_cells);
    field.speed /= fluid_cells;
    field.tracer_mean /= fluid_cells;
    if (porous && field.porous_cells) {
        field.porous_speed /= field.porous_cells;
        field.pore_volume = c->porosity * field.porous_cells / (inv[0]*inv[1]*inv[2]);
    }
    free(porous);
    if (!isfinite(field.divergence) || !isfinite(field.speed) || !isfinite(field.tracer_mean)) {
        flow_release(&field);
        field.fallback = "CFD produced a non-finite state";
        field.memory = "analytic";
    } else {
        field.method = used_gpu ? "3D incompressible Navier-Stokes, OpenCL pressure projection" :
                                  "3D incompressible Navier-Stokes, CPU";
        if (field.solid) {
            field.method = "3D incompressible Navier-Stokes, CPU voxel obstacles, periodic exterior";
            if (c->gpu) field.fallback = "OpenCL pressure does not support the obstacle stencil";
        } else if (dt > 0 && c->gpu && !gpu)
            field.fallback = "OpenCL pressure unavailable; continued on pageable CPU memory";
    }
    return field;
}

FlowField flow_solve(const FlowConfig *config)
{
    return flow_solve_domain(config, nullptr, 0);
}

void flow_porous_summary(const FlowField *field, Text *out)
{
    if (field->grid && field->config.porous)
        format(out, " Darcy momentum drag: %zu porous cells, pore volume %.8g m³, mean porous speed %.8g m/s.",
               field->porous_cells, field->pore_volume, field->porous_speed);
}
