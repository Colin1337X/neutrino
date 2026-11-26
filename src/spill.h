#ifndef NEUTRINO_SPILL_H
#define NEUTRINO_SPILL_H
#include "physics_models.h"
#include "parallel.h"

enum { SPILL_PARCELS = 25, SPILL_LIMIT = 128 };
typedef struct {
    double point[3], velocity[3];
    char surface[65];
    bool routed;
} SpillParcel;
typedef struct {
    char source[65];
    double litres, released_s;
    double *film;
    int film_cells;
    SpillParcel parcels[SPILL_PARCELS];
} Spill;
typedef struct { Spill *items; int count; } SpillSet;
typedef struct { PhysicsBox box; const char *name; } SpillSurface;

Spill *spill_add(SpillSet *set);
bool spill_load(SpillSet *set, const Json *room);
void spill_store(const SpillSet *set, Json *room);
void spill_release(SpillSet *set);
void spill_advance(SpillSet *set, const PhysicsBox *room, const SpillSurface surfaces[], int count,
                   double gravity, double start, double end, Parallel *workers);
bool spill_evidence(const Json *room, const char *source, const char *target, Json *evidence, double *litres);
#endif
