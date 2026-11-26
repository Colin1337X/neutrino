#include "rigid_spin.h"

#include <math.h>
#include <string.h>

void spin_cross(double out[3], const double a[3], const double b[3])
{
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

void spin_rotate(const RigidSpin *spin, const double vector[3], double out[3], bool inverse)
{
    double p[3], a[3], b[3];
    for (int k = 0; k < 3; ++k) p[k] = spin->q[k + 1] * (inverse ? -1 : 1);
    spin_cross(a, p, vector);
    spin_cross(b, p, a);
    for (int k = 0; k < 3; ++k) out[k] = vector[k] + 2*(spin->q[0]*a[k] + b[k]);
}

void spin_inertia(const RigidSpin *spin, double mass, const double vector[3], double out[3], bool inverse)
{
    double local[3];
    spin_rotate(spin, vector, local, true);
    for (int k = 0; k < 3; ++k) {
        double a = spin->half[(k+1)%3], b = spin->half[(k+2)%3];
        double moment = mass*(a*a+b*b)/3;
        local[k] = inverse ? local[k] / moment : local[k] * moment;
    }
    spin_rotate(spin, local, out, false);
}

void spin_step(RigidSpin *spin, double mass, double dt)
{
    double omega[3];
    spin_inertia(spin, mass, spin->momentum, omega, true);
    double speed = hypot(hypot(omega[0], omega[1]), omega[2]);
    if (speed == 0 || dt == 0) return;
    double angle = speed * dt, q[4] = {cos(angle/2)}, cross[3];
    for (int k = 0; k < 3; ++k) q[k+1] = omega[k] * (sin(angle/2)/speed);
    spin_cross(cross, q+1, spin->q+1);
    double w = q[0]*spin->q[0];
    for (int k = 0; k < 3; ++k) w -= q[k+1]*spin->q[k+1];
    for (int k = 0; k < 3; ++k)
        spin->q[k+1] = q[0]*spin->q[k+1] + spin->q[0]*q[k+1] + cross[k];
    spin->q[0] = w;
    double norm = hypot(hypot(spin->q[0], spin->q[1]), hypot(spin->q[2], spin->q[3]));
    for (int k = 0; k < 4; ++k) spin->q[k] /= norm;
    spin->travel += angle;
}

PhysicsBox spin_bounds(const RigidSpin *spin, const double center[3])
{
    double half[3] = {0};
    for (int k = 0; k < 3; ++k) {
        double basis[3] = {0}, turned[3];
        basis[k] = spin->half[k];
        spin_rotate(spin, basis, turned, false);
        for (int j = 0; j < 3; ++j) half[j] += fabs(turned[j]);
    }
    PhysicsBox box;
    for (int k = 0; k < 3; ++k) {
        box.lo[k] = center[k] - half[k];
        box.hi[k] = center[k] + half[k];
    }
    return box;
}

bool spin_load(RigidSpin *spin, const PhysicsBox *box, const Json *rigid)
{
    *spin = (RigidSpin){.q={1,0,0,0}};
    for (int k = 0; k < 3; ++k) spin->half[k] = (box->hi[k]-box->lo[k])/2;
    const Json *orientation = at(rigid, "orientation");
    if (!orientation) return true;
    if (cJSON_GetArraySize(orientation) != 4 ||
        !physics_vector(rigid, "half_extents_m", spin->half) ||
        !physics_vector(rigid, "angular_momentum", spin->momentum)) return false;
    double center[3], norm = 0;
    for (int k = 0; k < 3; ++k) {
        if (spin->half[k] <= 0) return false;
        center[k] = (box->lo[k]+box->hi[k])/2;
    }
    for (int k = 0; k < 4; ++k) {
        const Json *component = cJSON_GetArrayItem(orientation, k);
        if (!cJSON_IsNumber(component) || !isfinite(component->valuedouble)) return false;
        spin->q[k] = component->valuedouble;
        norm += spin->q[k]*spin->q[k];
    }
    if (fabs(norm-1) > 1e-8) return false;
    PhysicsBox expected = spin_bounds(spin, center);
    for (int k = 0; k < 3; ++k)
        if (fabs(expected.lo[k]-box->lo[k]) > 1e-8 || fabs(expected.hi[k]-box->hi[k]) > 1e-8) return false;
    return true;
}

void spin_save(const RigidSpin *spin, Json *rigid)
{
    put(rigid, "orientation", cJSON_CreateDoubleArray(spin->q, 4));
    put(rigid, "half_extents_m", cJSON_CreateDoubleArray(spin->half, 3));
    put(rigid, "angular_momentum", cJSON_CreateDoubleArray(spin->momentum, 3));
    put(rigid, "rotation_rad", cJSON_CreateNumber(spin->travel));
}
