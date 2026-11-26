#include "runoff.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool pair(const Json *args, const char *key, double min, double max, double out[2])
{
    const Json *array = at(args, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 2) return false;
    for (int k = 0; k < 2; ++k) {
        const Json *number = cJSON_GetArrayItem(array, k);
        if (!cJSON_IsNumber(number) || !isfinite(number->valuedouble) ||
            number->valuedouble < min || number->valuedouble > max) return false;
        out[k] = number->valuedouble;
    }
    return true;
}

static bool parameters(Runoff *grid, const PhysicsBox *bounds, const Json *args)
{
    double max[2], cells[2];
    if (!pair(args, "min_xy", -1e6, 1e6, grid->min) || !pair(args, "max_xy", -1e6, 1e6, max) ||
        !pair(args, "cells", 1, 64, cells) ||
        !physics_optional(args, "conductivity_m2_s", .00001, .1, .025, &grid->conductivity) ||
        (at(args, "slope") && !pair(args, "slope", -.5, .5, grid->slope))) return false;
    for (int k = 0; k < 2; ++k) {
        if (grid->min[k] < bounds->lo[k] || max[k] > bounds->hi[k] || max[k]-grid->min[k] < .01 ||
            floor(cells[k]) != cells[k]) return false;
        grid->cell[k] = (max[k]-grid->min[k])/cells[k];
    }
    grid->width = (int)cells[0];
    grid->height = (int)cells[1];
    grid->count = grid->width * grid->height;
    grid->area = grid->cell[0] * grid->cell[1];
    double ratio = grid->cell[1]/grid->cell[0];
    grid->stable_dt = .24 * grid->area / (grid->conductivity * (ratio + 1/ratio));
    return grid->stable_dt >= 1.0/(240 * 16);
}

bool runoff_configure(Json *room, const PhysicsBox *bounds, const Json *args)
{
    Runoff grid = {0};
    if (!parameters(&grid, bounds, args)) return false;
    const Json *spill;
    cJSON_ArrayForEach(spill, at(room, "spills"))
        if (at(spill, "film")) return false;
    Json *config = cJSON_CreateObject();
    const char *keys[] = {"min_xy", "max_xy", "cells", "conductivity_m2_s", "slope"};
    for (size_t i = 0; i < sizeof keys/sizeof *keys; ++i)
        if (at(args, keys[i])) put(config, keys[i], cJSON_Duplicate(at(args, keys[i]), true));
    put(room, "runoff", config);
    return true;
}

bool runoff_open(Runoff *grid, const Json *room, const PhysicsBox *bounds)
{
    const Json *config = at(room, "runoff");
    if (!config) return true;
    if (!parameters(grid, bounds, config)) return false;
    grid->bed = calloc((size_t)grid->count, sizeof *grid->bed);
    if (!grid->bed) return false;
    for (int i = 0; i < grid->count; ++i) grid->bed[i] = (RunoffCell){.base=bounds->lo[2],.surface="@room"};
    const Json *object;
    cJSON_ArrayForEach(object, at(room, "objects")) {
        double lo[3], hi[3];
        if (number(at(object, "rigid"), "axes") || !physics_vector(object, "min", lo) ||
            !physics_vector(object, "max", hi)) continue;
        for (int y = 0; y < grid->height; ++y) {
            double py = grid->min[1] + (y+.5)*grid->cell[1];
            if (py < lo[1] || py >= hi[1]) continue;
            for (int x = 0; x < grid->width; ++x) {
                double px = grid->min[0] + (x+.5)*grid->cell[0];
                RunoffCell *bed = &grid->bed[y*grid->width+x];
                if (px >= lo[0] && px < hi[0] && hi[2] > bed->base) {
                    bed->base = hi[2];
                    bed->surface = js(object, "name");
                }
            }
        }
    }
    for (int i = 0; i < grid->count; ++i)
        grid->bed[i].height = grid->bed[i].base + grid->slope[0]*(i%grid->width+.5)*grid->cell[0] +
            grid->slope[1]*(i/grid->width+.5)*grid->cell[1];
    return true;
}

static int deposit_cell(const Runoff *grid, const SpillParcel *parcel, int x, int y)
{
    int best = -1;
    double distance = DBL_MAX;
    for (int row = y-1; row <= y+1; ++row) {
        if (row < 0 || row >= grid->height) continue;
        for (int col = x-1; col <= x+1; ++col) {
            if (col < 0 || col >= grid->width) continue;
            int index = row*grid->width+col;
            const RunoffCell *bed = &grid->bed[index];
            if (fabs(parcel->point[2]-bed->base) > 1e-7 || strcmp(parcel->surface, bed->surface)) continue;
            double dx = grid->min[0]+(col+.5)*grid->cell[0]-parcel->point[0];
            double dy = grid->min[1]+(row+.5)*grid->cell[1]-parcel->point[1];
            double candidate = dx*dx+dy*dy;
            if (candidate < distance) { best = index; distance = candidate; }
        }
    }
    return best;
}

static bool seed(Runoff *grid, SpillSet *spills, bool *wet)
{
    for (int i = 0; i < spills->count; ++i) {
        Spill *spill = &spills->items[i];
        if (spill->film && spill->film_cells != grid->count) return false;
        for (int j = 0; j < SPILL_PARCELS; ++j) {
            SpillParcel *parcel = &spill->parcels[j];
            if (!*parcel->surface || parcel->routed) continue;
            double x = floor((parcel->point[0]-grid->min[0])/grid->cell[0]);
            double y = floor((parcel->point[1]-grid->min[1])/grid->cell[1]);
            if (x < 0 || x >= grid->width || y < 0 || y >= grid->height) continue;
            int cell = deposit_cell(grid, parcel, (int)x, (int)y);
            if (cell < 0) continue;
            if (!spill->film) {
                spill->film = calloc((size_t)grid->count, sizeof *spill->film);
                if (!spill->film) return false;
                spill->film_cells = grid->count;
            }
            spill->film[cell] += spill->litres / SPILL_PARCELS;
            parcel->routed = true;
        }
        *wet |= spill->film != nullptr;
    }
    return true;
}

static double requested(const Runoff *grid, const double *total, int i, int j, double coefficient)
{
    double diff = grid->bed[i].height - grid->bed[j].height + (total[i]-total[j])/(1000*grid->area);
    return (diff > 0 && total[i] > 0) || (diff < 0 && total[j] > 0) ? diff*coefficient : 0;
}

typedef struct {
    const Runoff *grid;
    const SpillSet *spills;
    double *total, *out, *right, *down, *change, *film;
    double coefficient_x, coefficient_y;
} RunoffSweep;

static void runoff_depth(void *context, size_t first, size_t end)
{
    const RunoffSweep *sweep = context;
    for (int i = (int)first; i < (int)end; ++i) {
        double total = 0;
        for (int s = 0; s < sweep->spills->count; ++s) {
            const double *film = sweep->spills->items[s].film;
            if (film) total += film[i];
        }
        sweep->total[i] = total;
    }
}

static void runoff_flux(void *context, size_t first, size_t end)
{
    const RunoffSweep *sweep = context;
    const Runoff *grid = sweep->grid;
    const int width = grid->width;
    for (int i = (int)first; i < (int)end; ++i) {
        sweep->right[i] = (i+1)%width ?
            requested(grid,sweep->total,i,i+1,sweep->coefficient_x) : 0;
        sweep->down[i] = i+width < grid->count ?
            requested(grid,sweep->total,i,i+width,sweep->coefficient_y) : 0;
    }
}

static void runoff_outgoing(void *context, size_t first, size_t end)
{
    const RunoffSweep *sweep = context;
    const int width = sweep->grid->width;
    for (int i = (int)first; i < (int)end; ++i) {
        double up = i >= width ? sweep->down[i-width] : 0;
        double left = i%width ? sweep->right[i-1] : 0;
        sweep->out[i] = fmax(0,-up) + fmax(0,-left) + fmax(0,sweep->right[i]) + fmax(0,sweep->down[i]);
    }
}

static double film_flux(const RunoffSweep *sweep, int first, int second, double flow)
{
    if (!flow) return 0;
    const int source = flow > 0 ? first : second;
    return flow * sweep->film[source] / fmax(sweep->total[source], sweep->out[source]);
}

static void runoff_film(void *context, size_t first, size_t end)
{
    const RunoffSweep *sweep = context;
    const int width = sweep->grid->width;
    for (int i = (int)first; i < (int)end; ++i) {
        double change = 0;
        if (i >= width) change += film_flux(sweep,i-width,i,sweep->down[i-width]);
        if (i%width) change += film_flux(sweep,i-1,i,sweep->right[i-1]);
        change -= film_flux(sweep,i,i+1,sweep->right[i]);
        change -= film_flux(sweep,i,i+width,sweep->down[i]);
        sweep->change[i] = fmax(0,sweep->film[i]+change);
    }
}

static void relax(Runoff *grid, SpillSet *spills, double dt)
{
    const size_t n = (size_t)grid->count;
    const double ratio = grid->cell[1]/grid->cell[0], coefficient = 1000 * dt * grid->conductivity;
    RunoffSweep sweep = {grid,spills,grid->scratch,grid->scratch+n,grid->scratch+2*n,
                         grid->scratch+3*n,grid->scratch+4*n,nullptr,coefficient*ratio,coefficient/ratio};
    parallel_run(grid->workers,n,runoff_depth,&sweep);
    parallel_run(grid->workers,n,runoff_flux,&sweep);
    parallel_run(grid->workers,n,runoff_outgoing,&sweep);
    for (int s = 0; s < spills->count; ++s) {
        sweep.film = spills->items[s].film;
        if (!sweep.film) continue;
        parallel_run(grid->workers,n,runoff_film,&sweep);
        memcpy(sweep.film,sweep.change,n*sizeof *sweep.film);
    }
}

bool runoff_advance(Runoff *grid, SpillSet *spills, double dt)
{
    if (!grid->count) return true;
    bool wet = false;
    if (!seed(grid, spills, &wet)) return false;
    if (!wet || dt <= 0) return true;
    if (!grid->scratch) {
        grid->scratch = calloc((size_t)grid->count*5, sizeof *grid->scratch);
        if (!grid->scratch) return false;
        grid->workers = parallel_open((size_t)grid->count, 1024);
    }
    int steps = (int)ceil(dt/grid->stable_dt);
    for (int i = 0; i < steps; ++i) relax(grid, spills, dt/steps);
    return true;
}

void runoff_release(Runoff *grid)
{
    free(grid->bed);
    free(grid->scratch);
    parallel_close(grid->workers);
    *grid = (Runoff){0};
}

bool runoff_evidence(const Json *room, const PhysicsBox *bounds, const char *entity, const char *surface,
                     Json *evidence, double *litres, double *depth)
{
    Runoff grid = {0};
    SpillSet spills = {0};
    if (!runoff_open(&grid, room, bounds) || !grid.count || !spill_load(&spills, room)) {
        runoff_release(&grid);
        spill_release(&spills);
        return false;
    }
    double *total = calloc((size_t)grid.count*2, sizeof *total), *selected = total ? total+grid.count : nullptr;
    double released = 0, airborne = 0, unrouted = 0, wet = 0, pooled = 0, peak[3] = {0};
    bool destination = !*surface;
    for (int i = 0; !destination && i < grid.count; ++i)
        destination = !strcmp(grid.bed[i].surface, surface);
    bool valid = total != nullptr && destination;
    *litres = *depth = 0;
    for (int s = 0; valid && s < spills.count; ++s) {
        const Spill *spill = &spills.items[s];
        bool match = !strcmp(spill->source, entity);
        double parcel = spill->litres/SPILL_PARCELS;
        if (match) {
            released += spill->litres;
            for (int j = 0; j < SPILL_PARCELS; ++j)
                if (!*spill->parcels[j].surface) airborne += parcel;
                else if (!spill->parcels[j].routed) unrouted += parcel;
        }
        if (!spill->film) continue;
        if (spill->film_cells != grid.count) { valid = false; break; }
        for (int i = 0; i < grid.count; ++i) {
            total[i] += spill->film[i];
            if (match) {
                selected[i] += spill->film[i];
                pooled += spill->film[i];
            }
        }
    }
    for (int i = 0; valid && i < grid.count; ++i) {
        if (selected[i] <= 0 || (*surface && strcmp(grid.bed[i].surface, surface))) continue;
        *litres += selected[i];
        double d = total[i]/(1000*grid.area);
        if (d >= .00005) wet += grid.area;
        if (d > *depth) {
            *depth = d;
            peak[0] = grid.min[0] + (i%grid.width+.5)*grid.cell[0];
            peak[1] = grid.min[1] + (i/grid.width+.5)*grid.cell[1];
            peak[2] = grid.bed[i].height;
        }
    }
    if (valid && released > 0) {
        // Strip roundoff tails from reports, without deleting water from the simulation.
        if (*litres < 1e-12) *litres = 0;
        if (*depth < 1e-12) *depth = 0;
        put(evidence, "released_l", cJSON_CreateNumber(released));
        put(evidence, "airborne_l", cJSON_CreateNumber(airborne));
        put(evidence, "unrouted_l", cJSON_CreateNumber(unrouted));
        put(evidence, "grid_water_l", cJSON_CreateNumber(pooled));
        put(evidence, "selected_l", cJSON_CreateNumber(*litres));
        put(evidence, "max_depth_m", cJSON_CreateNumber(*depth));
        put(evidence, "wet_area_m2", cJSON_CreateNumber(wet));
        put(evidence, "wet_cutoff_m", cJSON_CreateNumber(.00005));
        if (*depth >= .00005) put(evidence, "deepest_cell_m", cJSON_CreateDoubleArray(peak, 3));
    }
    free(total);
    runoff_release(&grid);
    spill_release(&spills);
    return valid && released > 0;
}
