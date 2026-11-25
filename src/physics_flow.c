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
    }
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
    field->grid = NULL;
}

static size_t index3(int n, int x, int y, int z)
{
    return (size_t)((z * n + y) * n + x);
}

static double sample_grid(const double *data, int n, double x, double y, double z)
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
    double result = 0;
    for (int corner = 0; corner < 8; ++corner) {
        int cell[3];
        double weight = 1;
        for (int axis = 0; axis < 3; ++axis) {
            bool upper = (corner & (1 << axis)) != 0;
            cell[axis] = upper ? high[axis] : low[axis];
            weight *= upper ? fraction[axis] : 1 - fraction[axis];
        }
        result += weight * data[index3(n, cell[0], cell[1], cell[2])];
    }
    return result;
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
    return sample_grid(field->grid, n, local[0] * n / c->length[0] - .5,
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
static bool project(int n, const double inv[3], double *const v[3], double *rhs, double *a, double *b, bool hardware)
{
    const size_t cells = (size_t)n*n*n;
    const double w[3] = {inv[0]*inv[0], inv[1]*inv[1], inv[2]*inv[2]};
    const double diagonal = 2 * (w[0]+w[1]+w[2]);
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi);
        rhs[i] = divergence_at(v, i, hi, inv);
    }
    const bool accelerated = hardware && flow_pressure_gpu(n,w,rhs,a,PRESSURE_SWEEPS);
    if (!accelerated) memset(a,0,cells*sizeof *a);
    for (int pass=0; !accelerated && pass<PRESSURE_SWEEPS; ++pass) {
        for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
            size_t i=index3(n,x,y,z), lo[3], hi[3];
            neighbors(n,x,y,z,lo,hi);
            double adjacent = 0;
            for (int axis=0; axis<3; ++axis) adjacent += w[axis]*(a[lo[axis]]+a[hi[axis]]);
            b[i] = a[i] + (2.0/3) * ((adjacent-rhs[i])/diagonal - a[i]);
        }
        double *swap=a; a=b; b=swap;
    }
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi);
        for (int axis=0; axis<3; ++axis) v[axis][i] -= (a[i]-a[lo[axis]])*inv[axis];
    }
    return accelerated;
}

static void transport(int n, const double inv[3], double dt, double coefficient, const double *in,
                      double *out, double *scratch, double *const velocity[3])
{
    if (coefficient) {
        for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
            size_t i=index3(n,x,y,z), lo[3], hi[3];
            neighbors(n,x,y,z,lo,hi);
            double laplacian = 0;
            for (int axis=0; axis<3; ++axis)
                laplacian += (in[lo[axis]]+in[hi[axis]]-2*in[i])*inv[axis]*inv[axis];
            scratch[i] = in[i] + dt*coefficient*laplacian;
        }
        in = scratch;
    }
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z);
        out[i] = sample_grid(in,n,x-dt*velocity[0][i]*inv[0],y-dt*velocity[1][i]*inv[1],
                            z-dt*velocity[2][i]*inv[2]);
    }
}

FlowField flow_solve(const FlowConfig *c)
{
    FlowField field = {.config = *c, .method = "reduced Gaussian transport", .memory = "analytic"};
    if (!c->cfd) return field;
    const int n = c->resolution;
    const size_t cells = (size_t)n*n*n;
    double inv[3], inverse2 = 0, max_courant = 0;
    for (int axis=0; axis<3; ++axis) {
        inv[axis] = n/c->length[axis];
        inverse2 += inv[axis]*inv[axis];
        max_courant += (fabs(c->wind[axis])+c->vortex)*inv[axis];
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
    double *velocity[3] = {field.grid+cells, field.grid+2*cells,field.grid+3*cells};
    double *next[3] = {field.grid+4*cells,field.grid+5*cells,field.grid+6*cells};
    double *dye_next=field.grid+7*cells, *rhs=field.grid+8*cells, *p=field.grid+9*cells, *q=field.grid+10*cells;
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z);
        double point[3] = {(x+.5)/inv[0],(y+.5)/inv[1],(z+.5)/inv[2]};
        const double tau = 6.283185307179586;
        velocity[0][i] = c->wind[0] + c->vortex*sin(tau*(y+.5)/n);
        velocity[1][i] = c->wind[1] + c->vortex*sin(tau*(z+.5)/n);
        velocity[2][i] = c->wind[2] + c->vortex*sin(tau*(x+.5)/n);
        field.grid[i] = gaussian(c,point,0);
    }
    const double dt=c->seconds/steps_needed;
    bool gpu = c->gpu, used_gpu = false;
    for (int step=0; dt>0 && step<(int)steps_needed; ++step) {
        for (int axis=0; axis<3; ++axis) transport(n,inv,dt,c->viscosity,velocity[axis],next[axis],rhs,velocity);
        gpu = project(n,inv,next,rhs,p,q,gpu);
        used_gpu |= gpu;
        transport(n,inv,dt,c->diffusivity,field.grid,dye_next,rhs,next);
        memcpy(field.grid,dye_next,cells*sizeof *field.grid);
        for (int axis=0; axis<3; ++axis) {
            double *swap=velocity[axis]; velocity[axis]=next[axis]; next[axis]=swap;
        }
    }
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi);
        double div=divergence_at(velocity,i,hi,inv);
        field.divergence += div*div;
        field.speed += hypot(hypot(velocity[0][i],velocity[1][i]),velocity[2][i]);
        field.tracer_mean += field.grid[i];
    }
    field.divergence = sqrt(field.divergence/cells);
    field.speed /= cells;
    field.tracer_mean /= cells;
    if (!isfinite(field.divergence) || !isfinite(field.speed) || !isfinite(field.tracer_mean)) {
        flow_release(&field);
        field.fallback = "CFD produced a non-finite state";
        field.memory = "analytic";
    } else {
        field.method = used_gpu ? "3D incompressible Navier-Stokes, OpenCL pressure projection" :
                                  "3D incompressible Navier-Stokes, CPU";
        if (dt > 0 && c->gpu && !gpu) field.fallback = "OpenCL pressure unavailable; continued on pageable CPU memory";
    }
    return field;
}
