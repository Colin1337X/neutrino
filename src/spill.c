#include "spill.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

Spill *spill_add(SpillSet *set)
{
    if (!set->items) set->items = calloc(SPILL_LIMIT, sizeof *set->items);
    return set->items && set->count < SPILL_LIMIT ? &set->items[set->count++] : nullptr;
}

bool spill_load(SpillSet *set, const Json *room)
{
    const Json *item;
    cJSON_ArrayForEach(item, at(room, "spills")) {
        Spill *spill = spill_add(set);
        const char *source = js(item, "source");
        const Json *points = at(item, "parcels");
        if (!spill || !*source || strlen(source) > 64 ||
            !physics_scalar(item, "litres", 0, 1e5, &spill->litres) ||
            !physics_scalar(item, "released_s", 0, DBL_MAX, &spill->released_s) ||
            cJSON_GetArraySize(points) != SPILL_PARCELS) return false;
        strcpy(spill->source, source);
        int i = 0;
        const Json *point;
        cJSON_ArrayForEach(point, points) {
            SpillParcel *parcel = &spill->parcels[i++];
            const char *surface = js(point, "surface");
            if (strlen(surface) > 64 || !physics_vector(point, "point", parcel->point) ||
                !physics_vector(point, "velocity", parcel->velocity)) return false;
            strcpy(parcel->surface, surface);
        }
    }
    return true;
}

void spill_store(const SpillSet *spills, Json *room)
{
    if (!spills->count) return;
    Json *array = cJSON_CreateArray();
    for (int i = 0; i < spills->count; ++i) {
        const Spill *spill = &spills->items[i];
        Json *item = cJSON_CreateObject(), *points = cJSON_CreateArray();
        set(item, "source", spill->source);
        put(item, "litres", cJSON_CreateNumber(spill->litres));
        put(item, "released_s", cJSON_CreateNumber(spill->released_s));
        for (int j = 0; j < SPILL_PARCELS; ++j) {
            const SpillParcel *parcel = &spill->parcels[j];
            Json *point = cJSON_CreateObject();
            put(point, "point", cJSON_CreateDoubleArray(parcel->point, 3));
            put(point, "velocity", cJSON_CreateDoubleArray(parcel->velocity, 3));
            if (*parcel->surface) set(point, "surface", parcel->surface);
            cJSON_AddItemToArray(points, point);
        }
        put(item, "parcels", points);
        cJSON_AddItemToArray(array, item);
    }
    put(room, "spills", array);
}

typedef struct { double fraction; int axis, normal; const char *surface; } Landing;

static void hit_box(Landing *hit, const PhysicsBox *box, const double p[3], const double d[3], const char *name)
{
    double entry = -DBL_MAX, exit = DBL_MAX;
    int axis = -1;
    for (int k = 0; k < 3; ++k) {
        if (fabs(d[k]) < 1e-12) {
            if (p[k] < box->lo[k] - 1e-10 || p[k] > box->hi[k] + 1e-10) return;
            continue;
        }
        double a = (box->lo[k] - p[k]) / d[k], b = (box->hi[k] - p[k]) / d[k];
        if (fmin(a,b) > entry) { entry = fmin(a,b); axis = k; }
        exit = fmin(exit, fmax(a,b));
    }
    if (axis >= 0 && entry >= -1e-8 && entry <= hit->fraction && exit > fmax(entry, 0) &&
        (hit->axis < 0 || entry < hit->fraction - 1e-10))
        *hit = (Landing){fmax(0, entry), axis, d[axis] < 0 ? 1 : -1, name};
}

static void advance_parcel(SpillParcel *parcel, const PhysicsBox *room, const SpillSurface surfaces[],
                           int count, double gravity, double dt)
{
    double *p = parcel->point, *v = parcel->velocity;
    double d[3] = {v[0]*dt, v[1]*dt, (v[2] - 0.5*gravity*dt)*dt};
    Landing hit = {.fraction = 1, .axis = -1};
    for (int k = 0; k < 3; ++k) {
        if (fabs(d[k]) < 1e-12) continue;
        double fraction = ((d[k] < 0 ? room->lo[k] : room->hi[k]) - p[k]) / d[k];
        if (fraction >= -1e-8 && fraction <= hit.fraction)
            hit = (Landing){fmax(0, fraction), k, d[k] < 0 ? 1 : -1, "@room"};
    }
    for (int i = 0; i < count; ++i) hit_box(&hit, &surfaces[i].box, p, d, surfaces[i].name);
    for (int k = 0; k < 3; ++k) p[k] = fmin(room->hi[k], fmax(room->lo[k], p[k] + d[k]*hit.fraction));
    v[2] -= gravity * dt * hit.fraction;
    if (hit.axis < 0) return;
    v[hit.axis] = 0;
    if (hit.axis == 2 && hit.normal > 0) {
        strcpy(parcel->surface, hit.surface);
        v[0] = v[1] = 0;
    }
}

void spill_advance(SpillSet *spills, const PhysicsBox *room, const SpillSurface surfaces[], int count,
                   double gravity, double start, double end)
{
    for (int i = 0; i < spills->count; ++i) {
        Spill *spill = &spills->items[i];
        double dt = end - fmax(start, spill->released_s);
        if (dt <= 0) continue;
        for (int j = 0; j < SPILL_PARCELS; ++j)
            if (!*spill->parcels[j].surface) advance_parcel(&spill->parcels[j], room, surfaces, count, gravity, dt);
    }
}

bool spill_evidence(const Json *room, const char *source, const char *target, Json *evidence, double *litres)
{
    double centroid[3] = {0}, low[3] = {DBL_MAX,DBL_MAX,DBL_MAX}, high[3] = {-DBL_MAX,-DBL_MAX,-DBL_MAX};
    double landed = 0, airborne = 0, selected = 0;
    bool found = false;
    const Json *spill;
    cJSON_ArrayForEach(spill, at(room, "spills")) {
        double volume;
        if (strcmp(js(spill, "source"), source) || !physics_scalar(spill, "litres", 0, 1e5, &volume)) continue;
        found = true;
        double parcel_l = volume / SPILL_PARCELS;
        const Json *parcel;
        cJSON_ArrayForEach(parcel, at(spill, "parcels")) {
            const char *surface = js(parcel, "surface");
            if (!*surface) { airborne += parcel_l; continue; }
            landed += parcel_l;
            if (*target && strcmp(surface, target)) continue;
            double point[3];
            if (!physics_vector(parcel, "point", point)) return false;
            selected += parcel_l;
            for (int axis = 0; axis < 3; ++axis) {
                centroid[axis] += point[axis] * parcel_l;
                low[axis] = fmin(low[axis], point[axis]);
                high[axis] = fmax(high[axis], point[axis]);
            }
        }
    }
    if (!found) return false;
    *litres = selected;
    put(evidence, "deposited_l", cJSON_CreateNumber(landed));
    put(evidence, "airborne_l", cJSON_CreateNumber(airborne));
    if (selected > 0) {
        for (int axis = 0; axis < 3; ++axis) centroid[axis] /= selected;
        put(evidence, "centroid_m", cJSON_CreateDoubleArray(centroid, 3));
        put(evidence, "min_m", cJSON_CreateDoubleArray(low, 3));
        put(evidence, "max_m", cJSON_CreateDoubleArray(high, 3));
    }
    return true;
}
