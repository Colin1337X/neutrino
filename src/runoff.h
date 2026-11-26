#ifndef NEUTRINO_RUNOFF_H
#define NEUTRINO_RUNOFF_H
#include "spill.h"

typedef struct {
    double base, height;
    const char *surface;
} RunoffCell;

typedef struct {
    double min[2], cell[2], slope[2], area, conductivity, stable_dt;
    int width, height, count;
    RunoffCell *bed;
    double *scratch;
    Parallel *workers;
} Runoff;

bool runoff_configure(Json *room, const PhysicsBox *bounds, const Json *args);
bool runoff_open(Runoff *grid, const Json *room, const PhysicsBox *bounds);
bool runoff_advance(Runoff *grid, SpillSet *spills, double dt);
void runoff_release(Runoff *grid);
bool runoff_evidence(const Json *room, const PhysicsBox *bounds, const char *entity, const char *surface,
                     Json *evidence, double *litres, double *depth);
#endif
