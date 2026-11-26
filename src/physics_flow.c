#include "physics_models.h"
#include "ray.h"
#include "parallel.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { FLOW_STEPS = 96, PRESSURE_SWEEPS = 32, VENT_SWEEPS = 96, OPEN_SWEEPS = 160,
       FACE_CELLS = FLOW_MAX_RESOLUTION * FLOW_MAX_RESOLUTION };
static const double reference_air_density = 1.2;
static const char frame_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static_assert(sizeof(double) == sizeof(uint64_t) && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024);
static_assert(FLOW_MAX_CELLS <= UINT16_MAX);
static_assert(PRESSURE_SWEEPS % 2 == 0 && VENT_SWEEPS % 2 == 0);

size_t flow_frame_bytes(int resolution)
{
    const size_t n = (size_t)resolution;
    return (4 * n * n * n * 64 + 5) / 6;
}

static size_t pressure_values(const FlowConfig *c)
{
    return c->pressure_open ? (size_t)3 * c->resolution * c->resolution : 0;
}

size_t flow_snapshot_bytes(const FlowConfig *c)
{
    const size_t n = (size_t)c->resolution;
    return ((4*n*n*n + pressure_values(c))*64 + 5)/6;
}

char *flow_encode(const FlowField *field)
{
    if (!field->grid) return nullptr;
    const size_t n = (size_t)field->config.resolution, bulk = 4*n*n*n;
    const size_t values = bulk + pressure_values(&field->config);
    char *encoded = malloc(flow_snapshot_bytes(&field->config) + 1);
    if (!encoded) return nullptr;
    size_t end = 0;
    uint32_t buffer = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < values; ++i) {
        uint64_t word;
        const double *sample = i < bulk ? &field->grid[i] : &field->pressure_high[i - bulk];
        memcpy(&word, sample, sizeof word);
        for (unsigned byte = 0; byte < 64; byte += 8) {
            buffer = buffer << 8 | (uint8_t)(word >> byte);
            bits += 8;
            while (bits >= 6) encoded[end++] = frame_alphabet[(buffer >> (bits -= 6)) & 63];
        }
    }
    if (bits) encoded[end++] = frame_alphabet[(buffer << (6 - bits)) & 63];
    encoded[end] = 0;
    return encoded;
}

static bool flow_decode(FlowField *field, const char *encoded)
{
    const size_t n = (size_t)field->config.resolution, bulk = 4*n*n*n;
    const size_t values = bulk + pressure_values(&field->config);
    if (strlen(encoded) != flow_snapshot_bytes(&field->config)) return false;
    uint32_t buffer = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < values; ++i) {
        uint64_t word = 0;
        for (unsigned byte = 0; byte < 64; byte += 8) {
            while (bits < 8) {
                const char *digit = strchr(frame_alphabet, *encoded++);
                if (!digit || !*digit) return false;
                buffer = buffer << 6 | (uint32_t)(digit - frame_alphabet);
                bits += 6;
            }
            word |= (uint64_t)((buffer >> (bits -= 8)) & 255) << byte;
        }
        double *sample = i < bulk ? &field->grid[i] : &field->pressure_high[i - bulk];
        memcpy(sample, &word, sizeof word);
        if (!isfinite(*sample)) return false;
    }
    return !(buffer & ((1u << bits) - 1));
}

bool physics_optional(const Json *args, const char *key, double low, double high, double initial, double *value)
{
    *value = initial;
    return !at(args, key) || physics_scalar(args, key, low, high, value);
}

static bool face_limits(const Json *face, const char *key, double limits[2])
{
    const Json *array = at(face, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 2) return false;
    for (int i = 0; i < 2; ++i) {
        const Json *item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return false;
        limits[i] = item->valuedouble;
    }
    return true;
}

static bool flow_boundaries(const Json *args, FlowConfig *c)
{
    const Json *faces = at(args, "boundaries");
    const char *boundary = js(args, "boundary");
    c->no_slip = !strcmp(boundary, "no_slip");
    c->closed = c->no_slip || !strcmp(boundary, "closed") || faces;
    if ((at(args, "boundary") && strcmp(boundary, "periodic") && strcmp(boundary, "closed") && !c->no_slip) ||
        ((at(args, "boundary") || faces) && !c->cfd) ||
        (faces && (!cJSON_IsObject(faces) || !faces->child || !strcmp(boundary, "periodic")))) return false;
    const char *const names[6] = {"x_min","x_max","y_min","y_max","z_min","z_max"};
    const char *const types[] = {"free_slip","no_slip","inflow","outflow","pressure"};
    const Json *entry;
    cJSON_ArrayForEach(entry, faces) {
        int face = 0;
        while (face < 6 && (!entry->string || strcmp(entry->string, names[face]))) ++face;
        if (face == 6 || at(faces, names[face]) != entry || !cJSON_IsObject(entry)) return false;
        int type = 0;
        while (type < 5 && strcmp(js(entry, "type"), types[type])) ++type;
        if (type == 5) return false;
        FlowFace *f = &c->faces[face];
        f->type = (FlowBoundary)type;
        c->ventilated |= type >= FLOW_INFLOW;
        c->pressure_open |= type == FLOW_PRESSURE;
        const Json *field;
        cJSON_ArrayForEach(field, entry) {
            const char *key = field->string;
            if (!key || at(entry, key) != field) return false;
            if (!strcmp(key, "type") || (type >= FLOW_INFLOW &&
                (!strcmp(key, "min_m") || !strcmp(key, "max_m")))) continue;
            if ((type == FLOW_INFLOW && !strcmp(key, "tracer")) ||
                ((type == FLOW_INFLOW || type == FLOW_OUTFLOW) && !strcmp(key, "speed_m_s")) ||
                (type == FLOW_PRESSURE && (!strcmp(key, "pressure_pa") || !strcmp(key, "backflow_tracer")))) continue;
            return false;
        }
        if (type < FLOW_INFLOW) continue;
        if (type == FLOW_INFLOW) {
            if (!physics_scalar(entry, "speed_m_s", .000001, 25, &f->speed) ||
                !physics_optional(entry, "tracer", 0, 1000, 0, &f->tracer)) return false;
        } else if (type == FLOW_PRESSURE) {
            if (!physics_scalar(entry, "pressure_pa", -1000, 1000, &f->pressure_pa) ||
                !physics_optional(entry, "backflow_tracer", 0, 1000, 0, &f->tracer)) return false;
        } else if (at(entry, "speed_m_s") && !physics_scalar(entry, "speed_m_s", .000001, 25, &f->speed)) return false;
        int j = 0;
        for (int axis = 0; axis < 3; ++axis)
            if (axis != face / 2) f->max[j++] = c->length[axis];
        if (at(entry, "min_m") || at(entry, "max_m")) {
            if (!face_limits(entry, "min_m", f->min) || !face_limits(entry, "max_m", f->max)) return false;
        }
        for (int axis = 0, k = 0; axis < 3; ++axis) {
            if (axis == face / 2) continue;
            if (f->min[k] < 0 || f->min[k] >= f->max[k] || f->max[k] > c->length[axis]) return false;
            ++k;
        }
    }
    /* An absent override is distinguished by initializing its wall type below. */
    bool inlet = false, outlet = false, automatic = false, sticky = c->no_slip;
    for (int face = 0; face < 6; ++face) {
        if (!at(faces, names[face])) c->faces[face].type = c->no_slip ? FLOW_NO_SLIP : FLOW_FREE_SLIP;
        const FlowBoundary type = c->faces[face].type;
        inlet |= type == FLOW_INFLOW;
        outlet |= type == FLOW_OUTFLOW;
        automatic |= type == FLOW_OUTFLOW && !c->faces[face].speed;
        sticky |= type == FLOW_NO_SLIP;
    }
    return (c->pressure_open ? !automatic : inlet == outlet) && (!sticky || c->viscosity > 0);
}

bool flow_parse(const Json *args, const double length[3], FlowConfig *config)
{
    *config = (FlowConfig){0};
    double resolution;
    const char *method = js(args, "method"), *memory = js(args, "memory");
    const char *backend = js(args,"backend");
    const char *raster = js(args, "raster");
    config->cfd = !strcmp(method, "navier_stokes") || !strcmp(method, "cfd");
    config->mapped = !strcmp(memory, "mapped");
    config->gpu = !strcmp(backend,"gpu");
    config->conservative = !strcmp(raster, "conservative");
    if ((at(args, "method") && !config->cfd && strcmp(method, "reduced")) ||
        (at(args, "memory") && !config->mapped && strcmp(memory, "auto")) ||
        (at(args, "backend") && !config->gpu && strcmp(backend,"cpu") && strcmp(backend,"auto")) ||
        (at(args, "raster") && (!config->cfd || (!config->conservative && strcmp(raster, "center")))) ||
        !physics_optional(args, "seconds", 0, 600, 1, &config->seconds) ||
        !physics_optional(args, "sigma_m", .001, 1000, .5, &config->sigma) ||
        !physics_optional(args, "concentration", 0, 1000, 1, &config->density) ||
        !physics_optional(args, "viscosity", 0, 10, .0001, &config->viscosity) ||
        !physics_optional(args, "diffusivity", 0, 10, .0001, &config->diffusivity) ||
        !physics_optional(args, "vortex_m_s", 0, 25, 0, &config->vortex) ||
        !physics_optional(args, "resolution", 6, FLOW_MAX_RESOLUTION, 12, &resolution) || floor(resolution) != resolution ||
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
    if (!flow_boundaries(args, config)) return false;
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
    field->pressure_high = NULL;
    field->solid = NULL;
}

static size_t index3(int n, int x, int y, int z)
{
    return (size_t)((z * n + y) * n + x);
}

static double sample_grid(const double *data, const unsigned char *solid, int n, bool closed, double x, double y, double z)
{
    double coordinates[3] = {x,y,z}, fraction[3];
    int low[3], high[3];
    for (int axis = 0; axis < 3; ++axis) {
        if (!isfinite(coordinates[axis])) return NAN;
        const double coordinate = closed ? fmax(0, fmin(n - 1, coordinates[axis])) :
            coordinates[axis] - n * floor(coordinates[axis] / n);
        low[axis] = (int)coordinate % n;
        high[axis] = closed && low[axis] == n - 1 ? n - 1 : (low[axis] + 1) % n;
        fraction[axis] = coordinate - floor(coordinate);
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
        const double delta = local[axis] - config->center[axis] - config->wind[axis] * time;
        const double displacement = config->closed ? delta : remainder(delta, config->length[axis]);
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
    return sample_grid(field->grid, field->solid, n, c->closed, local[0] * n / c->length[0] - .5,
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

static void neighbors(int n, int x, int y, int z, size_t low[3], size_t high[3], bool closed)
{
    low[0] = index3(n, x ? x-1 : closed ? 0 : n-1, y, z);
    low[1] = index3(n, x, y ? y-1 : closed ? 0 : n-1, z);
    low[2] = index3(n, x, y, z ? z-1 : closed ? 0 : n-1);
    high[0] = index3(n, x < n-1 ? x+1 : closed ? n-1 : 0, y, z);
    high[1] = index3(n, x, y < n-1 ? y+1 : closed ? n-1 : 0, z);
    high[2] = index3(n, x, y, z < n-1 ? z+1 : closed ? n-1 : 0);
}

typedef struct {
    const FlowConfig *config;
    const unsigned char *solid;
    unsigned char mode[6][FACE_CELLS];
    double inward[6], pressure[6];
    double *pressure_high;
    Parallel *workers;
} FlowDomain;

static int face_cell(int n, int axis, int x, int y, int z)
{
    return axis == 0 ? y*n + z : axis == 1 ? x*n + z : x*n + y;
}

static bool is_pressure(const FlowDomain *d, int face, int x, int y, int z)
{
    return d->config->pressure_open &&
        d->mode[face][face_cell(d->config->resolution, face/2,x,y,z)] == FLOW_PRESSURE;
}

static double wall_velocity(const FlowDomain *d, int face, int x, int y, int z)
{
    if (!d->config->ventilated) return 0;
    const int n = d->config->resolution, cell = face_cell(n, face/2, x, y, z);
    if (d->mode[face][cell] == FLOW_PRESSURE)
        return face % 2 ? d->pressure_high[(face/2)*n*n+cell] : 0;
    return d->mode[face][cell] >= FLOW_INFLOW ? (face % 2 ? -d->inward[face] : d->inward[face]) : 0;
}

static bool build_faces(FlowDomain *d, const double inv[3], FlowField *field)
{
    const FlowConfig *c = d->config;
    const int n = c->resolution;
    double out_auto_area = 0, area[6] = {0};
    double reference = 0;
    for (int face = 0; face < 6; ++face)
        if (c->faces[face].type == FLOW_PRESSURE) { reference = c->faces[face].pressure_pa; break; }
    for (int face = 0; face < 6; ++face) {
        const FlowFace *f = &c->faces[face];
        const int axis = face/2;
        int tangent[2], count = 0;
        for (int j = 0; j < 3; ++j) if (j != axis) tangent[count++] = j;
        int p[3];
        p[axis] = face % 2 ? n-1 : 0;
        const double cell_area = 1 / (inv[tangent[0]] * inv[tangent[1]]);
        for (int j = 0; j < n; ++j) for (int k = 0; k < n; ++k) {
            p[tangent[0]] = j;
            p[tangent[1]] = k;
            FlowBoundary mode = f->type;
            if (mode >= FLOW_INFLOW) {
                const double s = (j+.5)/inv[tangent[0]], t = (k+.5)/inv[tangent[1]];
                if (s < f->min[0] || s >= f->max[0] || t < f->min[1] || t >= f->max[1] ||
                    (d->solid && d->solid[index3(n,p[0],p[1],p[2])]))
                    mode = c->no_slip ? FLOW_NO_SLIP : FLOW_FREE_SLIP;
                else area[face] += cell_area;
            }
            d->mode[face][j*n+k] = (unsigned char)mode;
        }
        if (f->type < FLOW_INFLOW) continue;
        if (area[face] == 0) {
            field->fallback = "A CFD vent has no uncovered face cells at this resolution";
            return false;
        }
        if (f->type == FLOW_PRESSURE) {
            d->pressure[face] = f->pressure_pa - reference;
            continue;
        }
        if (f->type == FLOW_INFLOW) {
            d->inward[face] = f->speed;
            field->inflow_m3_s += area[face] * f->speed;
        } else if (f->speed) {
            d->inward[face] = -f->speed;
            field->outflow_m3_s += area[face] * f->speed;
        } else out_auto_area += area[face];
    }
    if (c->pressure_open) return true;
    const double scale = fmax(field->inflow_m3_s, field->outflow_m3_s);
    const double remaining = field->inflow_m3_s - field->outflow_m3_s;
    const double speed = out_auto_area ? remaining / out_auto_area : 0;
    if ((out_auto_area && (speed <= 0 || speed > 25)) ||
        (!out_auto_area && fabs(remaining) > 1e-10 * scale)) {
        field->fallback = "CFD inlet and exhaust rates cannot be balanced on the resolved grid";
        return false;
    }
    for (int face = 0; face < 6; ++face)
        if (c->faces[face].type == FLOW_OUTFLOW && !c->faces[face].speed) {
            d->inward[face] = -speed;
            field->outflow_m3_s += area[face] * speed;
        }
    return true;
}

static double divergence_at(const FlowDomain *d, double *const v[3], size_t i, const size_t high[3],
                            const double inv[3], int x, int y, int z)
{
    double divergence = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const double upper = high[axis] == i ? wall_velocity(d, 2*axis+1, x, y, z) : v[axis][high[axis]];
        divergence += (upper - v[axis][i])*inv[axis];
    }
    return divergence;
}

static bool balanced_components(const FlowDomain *d, const double inv[3])
{
    if (!d->solid) return true;
    const int n = d->config->resolution;
    const size_t cells = (size_t)n*n*n;
    unsigned char seen[FLOW_MAX_CELLS] = {0};
    uint16_t queue[FLOW_MAX_CELLS];
    for (size_t initial = 0; initial < cells; ++initial) {
        if (seen[initial] || d->solid[initial]) continue;
        size_t count = 0;
        seen[initial] = 1;
        queue[count++] = (uint16_t)initial;
        double net = 0, total = 0;
        bool anchored = false;
        for (size_t k = 0; k < count; ++k) {
            const size_t i = queue[k];
            const int x = i%n, y = (i/n)%n, z = i/(n*n);
            size_t lo[3], hi[3];
            neighbors(n,x,y,z,lo,hi,true);
            for (int axis = 0; axis < 3; ++axis) {
                const size_t adjacent[] = {lo[axis], hi[axis]};
                const double area = inv[axis] / (inv[0]*inv[1]*inv[2]);
                for (int side = 0; side < 2; ++side) {
                    const size_t neighbor = adjacent[side];
                    if (neighbor == i) {
                        if (is_pressure(d,2*axis+side,x,y,z)) { anchored = true; continue; }
                        const double flux = (side ? -1 : 1) * area * wall_velocity(d,2*axis+side,x,y,z);
                        net += flux;
                        total += fabs(flux);
                    } else if (!seen[neighbor] && !d->solid[neighbor]) {
                        seen[neighbor] = 1;
                        queue[count++] = (uint16_t)neighbor;
                    }
                }
            }
        }
        if (!anchored && fabs(net) > 1e-10 * total) return false;
    }
    return true;
}

/* Compatible forward/backward difference pair gives a seven-point Poisson stencil. */
static void close_faces(const FlowDomain *d, double *const v[3])
{
    const int n = d->config->resolution;
    const bool closed = d->config->closed;
    const unsigned char *solid = d->solid;
    if (!solid && !closed) return;
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi,closed);
        for (int axis=0; axis<3; ++axis) {
            if (solid && (solid[i] || solid[lo[axis]])) v[axis][i] = 0;
            else if (lo[axis] == i && !is_pressure(d,2*axis,x,y,z))
                v[axis][i] = wall_velocity(d, 2*axis, x,y,z);
        }
    }
}

/* Pressure openings anchor the Poisson operator with a half-cell Dirichlet face. */
typedef struct {
    const FlowDomain *domain;
    const double *weights, *direction;
    double *product;
    double curvature[FLOW_MAX_RESOLUTION];
} PressureStencil;

static void pressure_stencil(void *context, size_t first, size_t end)
{
    PressureStencil *stencil = context;
    const FlowDomain *d = stencil->domain;
    const int n = d->config->resolution;
    const unsigned char *solid = d->solid;
    const double *w = stencil->weights, *direction = stencil->direction;
    double *product = stencil->product;
    for (int z = (int)first; z < (int)end; ++z) {
        double curvature = 0;
        for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
            const size_t i = index3(n,x,y,z);
            product[i] = 0;
            if (solid && solid[i]) continue;
            size_t lo[3], hi[3];
            neighbors(n,x,y,z,lo,hi,true);
            for (int axis=0; axis<3; ++axis) {
                const size_t adjacent[] = {lo[axis],hi[axis]};
                for (int side=0; side<2; ++side) {
                    const size_t j = adjacent[side];
                    if (j == i) {
                        if (is_pressure(d,2*axis+side,x,y,z)) product[i] += 2*w[axis]*direction[i];
                    } else if (!solid || !solid[j]) product[i] += w[axis]*(direction[i] - direction[j]);
                }
            }
            curvature += direction[i]*product[i];
        }
        stencil->curvature[z] = curvature;
    }
}

static bool solve_open_pressure(const FlowDomain *d, const double inv[3], double dt,
                                double *r, double *pressure, double *direction, double *product)
{
    const int n = d->config->resolution;
    const size_t cells = (size_t)n*n*n;
    const unsigned char *solid = d->solid;
    const double w[] = {inv[0]*inv[0],inv[1]*inv[1],inv[2]*inv[2]};
    const double scale = dt / reference_air_density;
    double residual = 0;
    memset(pressure, 0, cells * sizeof *pressure);
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        const size_t i = index3(n,x,y,z);
        const int p[] = {x,y,z};
        r[i] = -r[i];
        if (!solid || !solid[i]) for (int axis=0; axis<3; ++axis) {
            if (p[axis] == 0 && is_pressure(d,2*axis,x,y,z))
                r[i] += 2*w[axis]*d->pressure[2*axis]*scale;
            if (p[axis] == n-1 && is_pressure(d,2*axis+1,x,y,z))
                r[i] += 2*w[axis]*d->pressure[2*axis+1]*scale;
        }
        direction[i] = r[i];
        residual += r[i]*r[i];
    }
    const double tolerance = fmax(1e-12 * cells, residual * 1e-14);
    PressureStencil stencil = {.domain=d, .weights=w, .direction=direction, .product=product};
    for (int pass=0; pass<OPEN_SWEEPS && residual>tolerance; ++pass) {
        double curvature = 0;
        parallel_run(d->workers, (size_t)n, pressure_stencil, &stencil);
        for (int z = 0; z < n; ++z) curvature += stencil.curvature[z];
        if (!(curvature > 0) || !isfinite(curvature)) return false;
        const double alpha = residual / curvature;
        double updated = 0;
        for (size_t i=0; i<cells; ++i) {
            pressure[i] += alpha*direction[i];
            r[i] -= alpha*product[i];
            updated += r[i]*r[i];
        }
        if (!isfinite(updated)) return false;
        const double beta = updated / residual;
        for (size_t i=0; i<cells; ++i) direction[i] = r[i] + beta*direction[i];
        residual = updated;
    }
    return residual <= tolerance;
}

typedef struct {
    const FlowDomain *domain;
    const double *weights, *rhs, *input;
    double *output;
    double diagonal;
} JacobiStencil;

static void jacobi_stencil(void *context, size_t first, size_t end)
{
    const JacobiStencil *stencil = context;
    const FlowDomain *d = stencil->domain;
    const int n = d->config->resolution;
    const bool closed = d->config->closed;
    const unsigned char *solid = d->solid;
    const double *w = stencil->weights, *rhs = stencil->rhs, *a = stencil->input;
    double *b = stencil->output;
    const double diagonal = stencil->diagonal;
    for (int z = (int)first; z < (int)end; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi,closed);
        if (solid && solid[i]) { b[i] = 0; continue; }
        double adjacent = 0, open_diagonal = 0;
        for (int axis=0; axis<3; ++axis) {
            if (lo[axis] != i && (!solid || !solid[lo[axis]])) { adjacent += w[axis]*a[lo[axis]]; open_diagonal += w[axis]; }
            if (hi[axis] != i && (!solid || !solid[hi[axis]])) { adjacent += w[axis]*a[hi[axis]]; open_diagonal += w[axis]; }
        }
        double divisor = solid || closed ? open_diagonal : diagonal;
        b[i] = divisor > 0 ? a[i] + (2.0/3) * ((adjacent-rhs[i])/divisor - a[i]) : 0;
    }
}

static double sealed_cell(const double *row, const double *below, const double *above,
                          const double *w, double rhs, int x, int y, int z, int n)
{
    double adjacent = 0, diagonal = 0;
    if (x) { adjacent += w[0]*row[x-1]; diagonal += w[0]; }
    if (x+1 < n) { adjacent += w[0]*row[x+1]; diagonal += w[0]; }
    if (y) { adjacent += w[1]*row[x-n]; diagonal += w[1]; }
    if (y+1 < n) { adjacent += w[1]*row[x+n]; diagonal += w[1]; }
    if (z) { adjacent += w[2]*below[x]; diagonal += w[2]; }
    if (z+1 < n) { adjacent += w[2]*above[x]; diagonal += w[2]; }
    return row[x] + (2.0/3) * ((adjacent-rhs)/diagonal - row[x]);
}

static void sealed_plane(const JacobiStencil *job, int z, const double *below,
                         const double *plane, const double *above, double *output)
{
    const int n = job->domain->config->resolution;
    const double *w = job->weights, *rhs = job->rhs + z*n*n;
    for (int y = 0; y < n; ++y) {
        const int offset = y*n;
        const double *row = plane+offset, *lower = below+offset, *upper = above+offset;
        if (!z || z+1 == n || !y || y+1 == n) {
            for (int x = 0; x < n; ++x)
                output[offset+x] = sealed_cell(row,lower,upper,w,rhs[offset+x],x,y,z,n);
            continue;
        }
        output[offset] = sealed_cell(row,lower,upper,w,rhs[offset],0,y,z,n);
        for (int x = 1; x+1 < n; ++x) {
            double adjacent = 0;
            adjacent += w[0]*row[x-1];
            adjacent += w[0]*row[x+1];
            adjacent += w[1]*row[x-n];
            adjacent += w[1]*row[x+n];
            adjacent += w[2]*lower[x];
            adjacent += w[2]*upper[x];
            output[offset+x] = row[x] + (2.0/3)*((adjacent-rhs[offset+x])/job->diagonal - row[x]);
        }
        output[offset+n-1] = sealed_cell(row,lower,upper,w,rhs[offset+n-1],n-1,y,z,n);
    }
}

static void sealed_sweeps(void *context, size_t first, size_t end)
{
    const JacobiStencil *job = context;
    const int n = job->domain->config->resolution, pitch = n*n;
    const int start = (int)first, stop = (int)end;
    const int halo_start = start ? start-1 : start;
    double tile[3][FACE_CELLS];
    /* Recompute the two neighbor planes locally so two passes need only one rendezvous. */
    for (int z = halo_start; z <= stop; ++z) {
        if (z < n) {
            const double *plane = job->input + z*pitch;
            sealed_plane(job,z,z ? plane-pitch : plane,plane,z+1 < n ? plane+pitch : plane,tile[z%3]);
        }
        if (z <= start) continue;
        const int ready = z-1;
        const double *plane = tile[ready%3];
        sealed_plane(job,ready,ready ? tile[(ready-1)%3] : plane,plane,z < n ? tile[z%3] : plane,
                     job->output + ready*pitch);
    }
}

typedef struct {
    const FlowDomain *domain;
    const double *inv, *pressure;
    double *const *velocity;
    double *rhs;
    double pressure_scale;
} Projection;

static void divergence_range(void *context, size_t first, size_t end)
{
    const Projection *job = context;
    const FlowDomain *d = job->domain;
    const int n = d->config->resolution;
    const bool closed = d->config->closed;
    const unsigned char *solid = d->solid;
    double *rhs = job->rhs;
    double *const *v = job->velocity;
    const double *inv = job->inv;
    for (int z = (int)first; z < (int)end; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi,closed);
        rhs[i] = solid && solid[i] ? 0 : divergence_at(d, v, i, hi, inv, x,y,z);
    }
}

static void correct_velocity(void *context, size_t first, size_t end)
{
    const Projection *job = context;
    const FlowDomain *d = job->domain;
    const int n = d->config->resolution;
    const bool closed = d->config->closed;
    const unsigned char *solid = d->solid;
    const double *inv = job->inv, *a = job->pressure;
    double *const *v = job->velocity;
    const double pressure_scale = job->pressure_scale;
    for (int z = (int)first; z < (int)end; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi,closed);
        if (solid && solid[i]) continue;
        for (int axis=0; axis<3; ++axis) {
            if (lo[axis] != i && (!solid || (!solid[i] && !solid[lo[axis]])))
                v[axis][i] -= (a[i]-a[lo[axis]])*inv[axis];
            else if (lo[axis] == i && is_pressure(d,2*axis,x,y,z))
                v[axis][i] -= 2*inv[axis]*(a[i] - d->pressure[2*axis]*pressure_scale);
            if (hi[axis] == i && is_pressure(d,2*axis+1,x,y,z)) {
                double *normal = &d->pressure_high[axis*n*n + face_cell(n,axis,x,y,z)];
                *normal -= 2*inv[axis]*(d->pressure[2*axis+1]*pressure_scale - a[i]);
            }
        }
    }
}

static bool project(const FlowDomain *d, const double inv[3], double *const v[3],
                    double *rhs, double *a, double *b, double *scratch, bool hardware, double dt, bool *valid)
{
    const int n = d->config->resolution;
    const bool closed = d->config->closed;
    const unsigned char *solid = d->solid;
    const size_t cells = (size_t)n*n*n;
    const double w[3] = {inv[0]*inv[0], inv[1]*inv[1], inv[2]*inv[2]};
    const double diagonal = 2 * (w[0]+w[1]+w[2]);
    const double pressure_scale = dt / reference_air_density;
    close_faces(d, v);
    Projection job = {.domain=d, .inv=inv, .velocity=v, .rhs=rhs, .pressure_scale=pressure_scale};
    parallel_run(d->workers, (size_t)n, divergence_range, &job);
    const bool accelerated = hardware && !solid && !closed && flow_pressure_gpu(n,w,rhs,a,PRESSURE_SWEEPS);
    int sweeps = d->config->ventilated ? VENT_SWEEPS : PRESSURE_SWEEPS;
    if (d->config->pressure_open) {
        *valid = solve_open_pressure(d, inv, dt, rhs, a, b, scratch);
        if (!*valid) return false;
        sweeps = 0;
    } else if (!accelerated) memset(a,0,cells*sizeof *a);
    if (sweeps && closed && !solid) {
        /* Preserve the six additions used for an interior cell of the generic stencil. */
        double sealed_diagonal = 0;
        for (int axis = 0; axis < 3; ++axis) {
            sealed_diagonal += w[axis];
            sealed_diagonal += w[axis];
        }
        for (int pass = 0; pass < sweeps; pass += 2) {
            JacobiStencil stencil = {d,w,rhs,a,b,sealed_diagonal};
            parallel_run(d->workers,(size_t)n,sealed_sweeps,&stencil);
            double *swap=a; a=b; b=swap;
        }
        sweeps = 0;
    }
    for (int pass=0; !accelerated && pass<sweeps; ++pass) {
        JacobiStencil stencil = {d, w, rhs, a, b, diagonal};
        parallel_run(d->workers, (size_t)n, jacobi_stencil, &stencil);
        double *swap=a; a=b; b=swap;
    }
    job.pressure = a;
    parallel_run(d->workers, (size_t)n, correct_velocity, &job);
    return accelerated;
}

static double diffusion_value(const FlowDomain *d, const double *in, size_t i, size_t adjacent,
                              int face, int component, int x, int y, int z)
{
    if (i != adjacent) return d->solid && d->solid[adjacent] ? in[i] : in[adjacent];
    if (component >= 0 && component != face/2 &&
        d->mode[face][face_cell(d->config->resolution, face/2, x,y,z)] == FLOW_NO_SLIP)
        return -in[i]; /* The wall is midway between the adjacent value and its ghost. */
    return in[i];
}

typedef struct {
    const FlowDomain *domain;
    const double *inv, *in;
    double *out;
    double *const *velocity;
    double dt, coefficient;
    int component;
} Transport;

static void diffuse_range(void *context, size_t first, size_t end)
{
    const Transport *job = context;
    const FlowDomain *d = job->domain;
    const int n = d->config->resolution;
    const bool closed = d->config->closed;
    const unsigned char *solid = d->solid;
    const double *in = job->in, *inv = job->inv;
    const int component = job->component;
    const double dt = job->dt, coefficient = job->coefficient;
    double *scratch = job->out;
    for (int z = (int)first; z < (int)end; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        neighbors(n,x,y,z,lo,hi,closed);
        if (solid && solid[i]) { scratch[i] = 0; continue; }
        double laplacian = 0;
        for (int axis=0; axis<3; ++axis)
            laplacian += (diffusion_value(d,in,i,lo[axis],2*axis,component,x,y,z) +
                          diffusion_value(d,in,i,hi[axis],2*axis+1,component,x,y,z) - 2*in[i])*inv[axis]*inv[axis];
        scratch[i] = in[i] + dt*coefficient*laplacian;
    }
}

static void advect_range(void *context, size_t first, size_t end)
{
    const Transport *job = context;
    const FlowDomain *d = job->domain;
    const int n = d->config->resolution;
    const bool closed = d->config->closed;
    const unsigned char *solid = d->solid;
    const double *in = job->in, *inv = job->inv;
    double *out = job->out;
    double *const *velocity = job->velocity;
    const double dt = job->dt;
    for (int z = (int)first; z < (int)end; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z);
        out[i] = solid && solid[i] ? 0 : sample_grid(in,solid,n,closed,x-dt*velocity[0][i]*inv[0],
                            y-dt*velocity[1][i]*inv[1],z-dt*velocity[2][i]*inv[2]);
    }
}

static void transport(const FlowDomain *d, const double inv[3], double dt, double coefficient, const double *in,
                      double *out, double *scratch, double *const velocity[3], int component)
{
    const size_t planes = (size_t)d->config->resolution;
    Transport job = {d, inv, in, scratch, velocity, dt, coefficient, component};
    if (coefficient) {
        parallel_run(d->workers, planes, diffuse_range, &job);
        job.in = scratch;
    }
    job.out = out;
    parallel_run(d->workers, planes, advect_range, &job);
}

typedef struct {
    double entered, escaped, backflow;
    bool unstable;
} TracerMass;

typedef struct {
    const FlowDomain *domain;
    const double *inv, *in;
    double *out;
    double *const *velocity;
    double dt, diffusion, volume;
    TracerMass mass[FLOW_MAX_RESOLUTION];
} TracerTransport;

static void tracer_range(void *context, size_t first, size_t end)
{
    TracerTransport *job = context;
    const FlowDomain *d = job->domain;
    const int n = d->config->resolution;
    const double *inv = job->inv, *in = job->in;
    const double dt = job->dt, diffusion = job->diffusion, volume = job->volume;
    double *out = job->out;
    double *const *v = job->velocity;
    for (int z = (int)first; z < (int)end; ++z) {
        TracerMass *mass = &job->mass[z];
        for (int y=0; y<n && !mass->unstable; ++y) for (int x=0; x<n; ++x) {
            const size_t i = index3(n,x,y,z);
            if (d->solid && d->solid[i]) { out[i] = 0; continue; }
            size_t lo[3], hi[3];
            neighbors(n,x,y,z,lo,hi,true);
            double change = 0, loss = 0;
            for (int axis=0; axis<3; ++axis) {
                const size_t adjacent[] = {lo[axis],hi[axis]};
                for (int side=0; side<2; ++side) {
                    const size_t j = adjacent[side];
                    if (d->solid && d->solid[j]) continue;
                    const int face = 2*axis+side;
                    const double velocity = side ? (j == i ? wall_velocity(d,face,x,y,z) : v[axis][j]) : -v[axis][i];
                    const double source = j == i ? d->config->faces[face].tracer : in[j];
                    const double flux = -velocity * inv[axis] * (velocity > 0 ? in[i] : source);
                    const double exchange = j == i ? 0 : diffusion * inv[axis]*inv[axis];
                    change += flux + exchange * (source - in[i]);
                    loss += fmax(0,velocity)*inv[axis] + exchange;
                    if (j == i && flux > 0) {
                        const double entered = dt * volume * flux;
                        mass->entered += entered;
                        if (is_pressure(d,face,x,y,z)) mass->backflow += entered;
                    }
                    if (j == i && flux < 0) mass->escaped -= dt * volume * flux;
                }
            }
            if (!isfinite(change) || dt * loss > 1 + 1e-12) { mass->unstable = true; break; }
            out[i] = fmax(0, in[i] + dt * change);
        }
    }
}

/* Conservative upwind tracer transport. Each opening uses its upstream concentration. */
static bool ventilate_tracer(const FlowDomain *d, const double inv[3], double dt, double diffusion,
                             const double *in, double *out, double *const v[3], FlowField *field)
{
    const int n = d->config->resolution;
    TracerTransport job = {.domain=d, .inv=inv, .in=in, .out=out, .velocity=v, .dt=dt,
                           .diffusion=diffusion, .volume=1 / (inv[0]*inv[1]*inv[2])};
    parallel_run(d->workers, (size_t)n, tracer_range, &job);
    for (int z = 0; z < n; ++z) {
        if (job.mass[z].unstable) return false;
        field->tracer_in += job.mass[z].entered;
        field->tracer_out += job.mass[z].escaped;
        field->backflow_tracer_in += job.mass[z].backflow;
    }
    return true;
}

static double flow_drag(const FlowConfig *c)
{
    return c->porous ? c->viscosity / c->permeability : 0;
}

static double flow_steps(const FlowConfig *c, double inv[3], const double *const peak)
{
    double inverse2 = 0, max_courant = 0;
    const double drag = flow_drag(c);
    for (int axis=0; axis<3; ++axis) {
        inv[axis] = c->resolution/c->length[axis];
        inverse2 += inv[axis]*inv[axis];
        const double initial = fabs(c->wind[axis]) + c->vortex;
        max_courant += (peak ? fmax(initial, peak[axis]) : initial)*inv[axis];
        if (c->porous) max_courant += fabs(c->porous_drive[axis])*
            (drag > 0 ? fmin(c->seconds, 1/drag) : c->seconds)*inv[axis];
    }
    const double transport = c->ventilated ? 2*max_courant + 4*c->diffusivity*inverse2 : 2*max_courant;
    bool sticky = false;
    for (int face = 0; face < 6; ++face)
        sticky |= c->faces[face].type == FLOW_NO_SLIP ||
            (c->no_slip && c->faces[face].type >= FLOW_INFLOW);
    const double diffusion = fmax((sticky ? 6 : 4)*c->viscosity, 4*c->diffusivity) * inverse2;
    return ceil(c->seconds * fmax(transport, diffusion));
}

static int flow_sweeps(const FlowConfig *c)
{
    return c->pressure_open ? OPEN_SWEEPS : c->ventilated ? VENT_SWEEPS : PRESSURE_SWEEPS;
}

PhysicsCost flow_cost(const FlowConfig *c)
{
    PhysicsCost cost = {.seconds = c->seconds};
    if (!c->cfd) return cost;
    double inv[3];
    double steps = flow_steps(c, inv, nullptr);
    if (steps > FLOW_STEPS) return cost; /* The solver takes its analytical fallback. */
    if (c->seconds > 0) steps = fmax(1, steps);
    if (c->ventilated && c->seconds > 0) steps = FLOW_STEPS;
    const int n = c->resolution;
    const double cells = n * n * n;
    cost.steps = steps;
    cost.cells = cells;
    const int layers = c->seconds > 0 ? 11 : 4;
    const double buffer = (layers * cells + pressure_values(c)) * sizeof(double);
    cost.ram = buffer + cells * 2;
    cost.mapped = buffer;
    cost.work = cells * (1 + steps * (flow_sweeps(c) + 8));
    return cost;
}

PhysicsCost flow_step_cost(const FlowConfig *c, bool resumed)
{
    PhysicsCost cost = flow_cost(c);
    if (!cost.cells) return cost;
    if (resumed) {
        cost.steps = FLOW_STEPS;
        cost.work = cost.cells * (1 + FLOW_STEPS * (flow_sweeps(c) + 8));
    }
    cost.field_bytes = flow_snapshot_bytes(c);
    cost.ram += 2 * cost.field_bytes;
    return cost;
}

size_t flow_rasterize(const FlowConfig *c, const PhysicsBox *boxes, size_t count, unsigned char *solid)
{
    const int n = c->resolution;
    memset(solid, 0, (size_t)n*n*n);
    const double inv[] = {n/c->length[0], n/c->length[1], n/c->length[2]};
    size_t unresolved = 0;
    for (size_t box = 0; box < count; ++box) {
        int first[3], end[3];
        bool resolved = true;
        for (int axis = 0; axis < 3; ++axis) {
            first[axis] = n;
            end[axis] = n;
            for (int p = 0; p < n; ++p) {
                const double center = (p + .5) / inv[axis];
                const bool occupied = c->conservative ?
                    boxes[box].hi[axis] > boxes[box].lo[axis] &&
                    p / inv[axis] < boxes[box].hi[axis] && (p + 1) / inv[axis] > boxes[box].lo[axis] :
                    center >= boxes[box].lo[axis] && center < boxes[box].hi[axis];
                if (!occupied) continue;
                if (first[axis] == n) first[axis] = p;
                end[axis] = p + 1;
            }
            resolved &= first[axis] < end[axis];
        }
        if (!resolved) { ++unresolved; continue; }
        for (int z = first[2]; z < end[2]; ++z)
            for (int y = first[1]; y < end[1]; ++y)
                memset(solid + index3(n, first[0], y, z), 1, (size_t)(end[0] - first[0]));
    }
    return unresolved;
}

static void predict_upper_pressure(const FlowDomain *d, double *const v[3], double *const previous[3])
{
    const int n = d->config->resolution;
    for (int axis = 0; axis < 3; ++axis) {
        if (d->config->faces[2*axis+1].type != FLOW_PRESSURE) continue;
        for (int a = 0; a < n; ++a) for (int b = 0; b < n; ++b) {
            if (d->mode[2*axis+1][a*n+b] != FLOW_PRESSURE) continue;
            const size_t i = axis == 0 ? index3(n,n-1,a,b) :
                             axis == 1 ? index3(n,a,n-1,b) : index3(n,a,b,n-1);
            double *normal = &d->pressure_high[axis*n*n + a*n+b];
            if (previous) *normal += v[axis][i] - previous[axis][i];
            else *normal = v[axis][i];
        }
    }
}

static void record_boundary_rate(const FlowDomain *d, FlowField *field, double outward,
                                 double area, int face, int x, int y, int z)
{
    if (outward > 0) field->outflow_m3_s += outward * area;
    else if (outward < 0) {
        field->inflow_m3_s -= outward * area;
        if (is_pressure(d,face,x,y,z)) field->backflow_m3_s -= outward * area;
    }
}

FlowField flow_resume_domain(const FlowConfig *c, const PhysicsBox *boxes, size_t box_count, const char *frame)
{
    FlowField field = {.config = *c, .method = c->porous ? "porous CFD unavailable" : "reduced Gaussian transport",
                       .memory = c->porous ? "none" : "analytic"};
    if (!c->cfd) return field;
    const int n = c->resolution;
    const size_t cells = (size_t)n*n*n;
    double inv[3];
    double steps_needed = fmax(1, flow_steps(c, inv, nullptr));
    if (steps_needed > FLOW_STEPS) {
        field.fallback = "CFD timestep budget exceeded";
        return field;
    }
    const int layers = c->seconds > 0 ? 11 : 4;
    field.grid = physics_memory_alloc((layers*cells+pressure_values(c))*sizeof(double), c->mapped, &field.mapped, &field.memory);
    if (!field.grid) {
        field.fallback = "CFD memory allocation unavailable";
        field.memory = "analytic";
        return field;
    }
    if (c->pressure_open) field.pressure_high = field.grid + layers*cells;
    if (frame && !flow_decode(&field, frame)) {
        flow_release(&field);
        field.fallback = "Invalid saved CFD frame";
        return field;
    }
    if (box_count) {
        field.solid = malloc(cells);
        if (!field.solid) {
            flow_release(&field);
            field.fallback = "CFD obstacle mask allocation unavailable";
            field.memory = "analytic";
            return field;
        }
        field.unresolved_boxes = flow_rasterize(c, boxes, box_count, field.solid);
    }
    FlowDomain domain = {.config = c, .solid = field.solid, .pressure_high = field.pressure_high};
    if (c->closed && !build_faces(&domain, inv, &field)) {
        flow_release(&field);
        return field;
    }
    if (c->ventilated && !balanced_components(&domain, inv)) {
        flow_release(&field);
        field.fallback = "Disconnected CFD compartments have unbalanced inlet or exhaust flux";
        return field;
    }
    unsigned char *porous = c->porous ? calloc(cells, 1) : nullptr;
    if (c->porous && !porous) {
        flow_release(&field);
        field.fallback = "CFD porous mask allocation unavailable";
        field.memory = "none";
        return field;
    }
    double *velocity[3] = {field.grid+cells, field.grid+2*cells,field.grid+3*cells};
    double *next[3], *dye_next = nullptr, *rhs = nullptr, *p = nullptr, *q = nullptr;
    if (c->seconds > 0) {
        for (int axis = 0; axis < 3; ++axis) next[axis] = field.grid + (4 + axis) * cells;
        dye_next=field.grid+7*cells; rhs=field.grid+8*cells; p=field.grid+9*cells; q=field.grid+10*cells;
    }
    double peak[3] = {0};
    size_t fluid_cells = 0;
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z);
        double point[3] = {(x+.5)/inv[0],(y+.5)/inv[1],(z+.5)/inv[2]};
        if (field.solid && field.solid[i]) continue;
        if (porous) {
            bool inside = true;
            for (int axis = 0; axis < 3; ++axis)
                inside &= point[axis] >= c->porous_min[axis] && point[axis] < c->porous_max[axis];
            porous[i] = inside;
            field.porous_cells += inside;
        }
        ++fluid_cells;
        if (frame) {
            for (int axis = 0; axis < 3; ++axis) peak[axis] = fmax(peak[axis], fabs(velocity[axis][i]));
            continue;
        }
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
    if (c->pressure_open && !frame) predict_upper_pressure(&domain, velocity, nullptr);
    if ((frame || c->ventilated) && c->seconds > 0) {
        for (int face = 0; face < 6; ++face) peak[face/2] = fmax(peak[face/2], fabs(domain.inward[face]));
        if (c->pressure_open) {
            for (int axis = 0; axis < 3; ++axis)
                for (int i = 0; i < n*n; ++i)
                    peak[axis] = fmax(peak[axis], fabs(field.pressure_high[axis*n*n + i]));
            double low = 0, high = 0;
            for (int face = 0; face < 6; ++face) {
                low = fmin(low, domain.pressure[face]);
                high = fmax(high, domain.pressure[face]);
            }
            const double speed = sqrt(2 * (high - low) / reference_air_density);
            for (int face = 0; face < 6; ++face)
                if (c->faces[face].type == FLOW_PRESSURE) peak[face/2] = fmax(peak[face/2], speed);
        }
        steps_needed = fmax(1, flow_steps(c, inv, peak));
        if (steps_needed > FLOW_STEPS) {
            free(porous);
            flow_release(&field);
            field.fallback = "Saved CFD or vent velocity exceeds the timestep budget";
            return field;
        }
    }
    if (c->seconds > 0) domain.workers = parallel_open(cells, 1728);
    close_faces(&domain, velocity);
    const double dt=c->seconds/steps_needed;
    const double drag = flow_drag(c);
    double decay = exp(-drag*dt), force_integral = drag > 0 ? -expm1(-drag*dt)/drag : dt;
    bool gpu = c->gpu, used_gpu = false;
    for (int step=0; dt>0 && step<(int)steps_needed; ++step) {
        for (int axis=0; axis<3; ++axis)
            transport(&domain,inv,dt,c->viscosity,velocity[axis],next[axis],rhs,velocity,axis);
        if (porous) {
            for (int axis = 0; axis < 3; ++axis)
                for (size_t i = 0; i < cells; ++i)
                    if (porous[i]) next[axis][i] = next[axis][i]*decay + c->porous_drive[axis]*force_integral;
        }
        if (c->pressure_open) predict_upper_pressure(&domain, next, velocity);
        bool projected = true;
        gpu = project(&domain,inv,next,rhs,p,q,dye_next,gpu,dt,&projected);
        if (!projected) {
            parallel_close(domain.workers);
            free(porous);
            flow_release(&field);
            field.fallback = "Pressure openings did not converge within the projection budget";
            return field;
        }
        used_gpu |= gpu;
        if (c->ventilated) {
            if (!ventilate_tracer(&domain,inv,dt,c->diffusivity,field.grid,dye_next,next,&field)) {
                parallel_close(domain.workers);
                free(porous);
                flow_release(&field);
                field.fallback = "Ventilated tracer exceeded the stable timestep; use a shorter step";
                return field;
            }
        } else transport(&domain,inv,dt,c->diffusivity,field.grid,dye_next,rhs,next,-1);
        memcpy(field.grid,dye_next,cells*sizeof *field.grid);
        for (int axis=0; axis<3; ++axis) {
            double *swap=velocity[axis]; velocity[axis]=next[axis]; next[axis]=swap;
        }
    }
    parallel_close(domain.workers);
    if (c->pressure_open) field.inflow_m3_s = field.outflow_m3_s = 0;
    const double cell_volume = 1 / (inv[0]*inv[1]*inv[2]);
    for (int z=0; z<n; ++z) for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
        size_t i=index3(n,x,y,z), lo[3], hi[3];
        if (field.solid && field.solid[i]) continue;
        neighbors(n,x,y,z,lo,hi,c->closed);
        if (c->pressure_open) {
            for (int axis = 0; axis < 3; ++axis) {
                const double area = cell_volume * inv[axis];
                if (lo[axis] == i) record_boundary_rate(&domain, &field, -velocity[axis][i], area, 2*axis,x,y,z);
                if (hi[axis] == i)
                    record_boundary_rate(&domain, &field, wall_velocity(&domain,2*axis+1,x,y,z), area, 2*axis+1,x,y,z);
            }
        }
        double div=divergence_at(&domain,velocity,i,hi,inv,x,y,z);
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
        for (int axis = 0; axis < 3; ++axis)
            if (velocity[axis] != field.grid + (axis + 1) * cells)
                memcpy(field.grid + (axis + 1) * cells, velocity[axis], cells * sizeof(double));
        field.method = used_gpu ? "3D incompressible Navier-Stokes, OpenCL pressure projection" :
                                  "3D incompressible Navier-Stokes, CPU";
        if (c->ventilated) {
            field.method = c->pressure_open ? "3D incompressible Navier-Stokes, CPU pressure openings" :
                                              "3D incompressible Navier-Stokes, CPU ventilated exterior";
            if (c->gpu) field.fallback = "OpenCL pressure uses periodic boundaries; used CPU for ventilation";
        } else if (c->closed) {
            field.method = field.solid ? "3D incompressible Navier-Stokes, CPU voxel obstacles, closed exterior" :
                                        "3D incompressible Navier-Stokes, CPU closed exterior";
            if (c->gpu) field.fallback = "OpenCL pressure uses periodic boundaries; used CPU for closed exterior";
        } else if (field.solid) {
            field.method = "3D incompressible Navier-Stokes, CPU voxel obstacles, periodic exterior";
            if (c->gpu) field.fallback = "OpenCL pressure does not support the obstacle stencil";
        } else if (dt > 0 && c->gpu && !gpu)
            field.fallback = "OpenCL pressure unavailable; continued on pageable CPU memory";
    }
    return field;
}

FlowField flow_solve_domain(const FlowConfig *config, const PhysicsBox *solids, size_t count)
{
    return flow_resume_domain(config, solids, count, nullptr);
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

void flow_geometry_summary(const FlowField *field, Text *out)
{
    if (field->unresolved_boxes)
        format(out, " %zu CFD obstacle boxes occupy no cells. Use a finer grid or raster:conservative.",
               field->unresolved_boxes);
}

void flow_boundary_summary(const FlowField *field, Text *out)
{
    if (field->config.ventilated && field->grid)
        format(out, " Ventilation: inlet %.8g m³/s, exhaust %.8g m³/s; tracer in %.8g, out %.8g concentration-m³ this step.",
               field->inflow_m3_s, field->outflow_m3_s, field->tracer_in, field->tracer_out);
    if (field->config.pressure_open && field->grid)
        format(out, " Pressure-opening backflow %.8g m³/s; tracer admitted by backflow %.8g concentration-m³ this step.",
               field->backflow_m3_s, field->backflow_tracer_in);
}
