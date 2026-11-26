#ifndef NEUTRINO_RIGID_SPIN_H
#define NEUTRINO_RIGID_SPIN_H
#include "physics_models.h"

typedef struct {
    double q[4], half[3], momentum[3], travel;
} RigidSpin;

void spin_cross(double out[3], const double a[3], const double b[3]);
void spin_rotate(const RigidSpin *spin, const double vector[3], double out[3], bool inverse);
void spin_inertia(const RigidSpin *spin, double mass, const double vector[3], double out[3], bool inverse);
void spin_step(RigidSpin *spin, double mass, double dt);
PhysicsBox spin_bounds(const RigidSpin *spin, const double center[3]);
bool spin_load(RigidSpin *spin, const PhysicsBox *box, const Json *rigid);
void spin_save(const RigidSpin *spin, Json *rigid);
#endif
