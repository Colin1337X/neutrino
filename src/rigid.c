#include "physics_models.h"
#include "rigid_spin.h"
#include "spill.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_BODIES = 128, AXES = 3, ALL_AXES = 7, HZ = 240, MAX_IMPACTS = 24 };
static const double epsilon = 1e-9;

typedef struct {
    double energy, time;
    bool occurred;
} Contact;

typedef struct {
    PhysicsBox box;
    RigidSpin spin;
    double velocity[AXES], origin[AXES], dry_kg, liquid_l, restitution, limit;
    double released_l, fracture_j, fracture_s, impact_j, impact_s;
    char name[65];
    const Json *source;
    int axes, parent;
    bool body, broken, piece, has_impact, rotation_blocked;
    Contact contacts[MAX_BODIES + 1];
} Body;

typedef struct {
    double fraction;
    int a, b, axis, sign;
} Impact;

bool rigid_tool(const char *name)
{
    return !strcmp(name, "scene_body") || !strcmp(name, "scene_constraint") ||
        !strcmp(name, "scene_step") || !strcmp(name, "scene_impulse");
}

static bool box_read(const Json *item, PhysicsBox *box)
{
    if (!physics_vector(item, "min", box->lo) || !physics_vector(item, "max", box->hi)) return false;
    for (int k = 0; k < AXES; ++k)
        if (box->hi[k] <= box->lo[k]) return false;
    return true;
}

static void num(Json *item, const char *key, double value)
{
    put(item, key, cJSON_CreateNumber(value));
}

static double stored(const Json *item, const char *key)
{
    const Json *value = at(item, key);
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) ? value->valuedouble : 0;
}

static double inverse_mass(const Body *body, int axis)
{
    return body->axes & (1 << axis) ? 1 / (body->dry_kg + body->liquid_l) : 0;
}

static bool cross_section(const PhysicsBox *a, const PhysicsBox *b, int axis)
{
    for (int k = 0; k < AXES; ++k)
        if (axis != k && (a->hi[k] - b->lo[k] <= epsilon || b->hi[k] - a->lo[k] <= epsilon))
            return false;
    return true;
}

static bool overlap(const PhysicsBox *a, const PhysicsBox *b)
{
    return cross_section(a, b, -1);
}

static void box_center(const PhysicsBox *box, double center[3])
{
    for (int k = 0; k < AXES; ++k) center[k] = (box->lo[k] + box->hi[k])*0.5;
}

static bool read_body(Body *body, const Json *item, const Json *materials, const PhysicsBox *room)
{
    const char *name = js(item, "name");
    if (!*name || strlen(name) >= sizeof body->name || !strcmp(name, "@room") ||
        !box_read(item, &body->box)) return false;
    for (int k = 0; k < AXES; ++k)
        if (body->box.lo[k] < room->lo[k] - epsilon || body->box.hi[k] > room->hi[k] + epsilon)
            return false;
    strcpy(body->name, name);
    box_center(&body->box, body->origin);
    body->source = item;
    body->parent = -1;
    const Json *rigid = at(item, "rigid");
    if (!rigid) return true;
    if (!physics_vector(rigid, "velocity", body->velocity) ||
        !physics_scalar(rigid, "mass_kg", 0.0005, 1e6, &body->dry_kg) ||
        !physics_scalar(rigid, "liquid_l", 0, 1e5, &body->liquid_l) ||
        !physics_scalar(rigid, "restitution", 0, 1, &body->restitution) ||
        !spin_load(&body->spin, &body->box, rigid)) return false;
    body->body = true;
    body->axes = number(rigid, "axes");
    if (body->axes < 0 || body->axes > ALL_AXES) return false;
    body->broken = cJSON_IsTrue(at(rigid, "fractured"));
    body->piece = *js(rigid, "fragment_of") != 0;
    body->released_l = stored(rigid, "released_l");
    body->fracture_j = stored(rigid, "fracture_energy_j");
    body->fracture_s = stored(rigid, "fracture_time_s");
    if (!body->piece && !body->broken) {
        const Json *material = at(materials, js(item, "material_id"));
        body->limit = stored(at(material, "mechanical"), "fracture_j");
    }
    return true;
}

static double acceleration(const Body *bodies, int count, int index, const PhysicsBox *room, double gravity)
{
    const Body *body = &bodies[index];
    if (!(body->axes & 4)) return 0;
    if (fabs(body->velocity[2]) > epsilon) return -gravity;
    if (body->box.lo[2] <= room->lo[2] + epsilon) return 0;
    for (int j = 0; j < count; ++j) {
        const Body *other = &bodies[j];
        if (index != j && !(other->axes & 4) && fabs(body->box.lo[2] - other->box.hi[2]) <= epsilon &&
            cross_section(&body->box, &other->box, 2)) return 0;
    }
    return -gravity;
}

static void consider(Impact *hit, double fraction, int a, int b, int axis, int sign)
{
    if (fraction >= -epsilon && fraction <= hit->fraction &&
        (hit->a < 0 || fraction < hit->fraction - epsilon))
        *hit = (Impact){fmax(0, fraction), a, b, axis, sign};
}

static void pair_impact(Impact *hit, const Body *a, const Body *b, const double da[AXES],
                        const double db[AXES], int i, int j)
{
    double entry = -DBL_MAX, exit = DBL_MAX;
    int axis = -1, sign = 0;
    for (int k = 0; k < AXES; ++k) {
        double relative = da[k] - db[k];
        if (fabs(relative) < epsilon) {
            if (a->box.hi[k] - b->box.lo[k] <= epsilon || b->box.hi[k] - a->box.lo[k] <= epsilon)
                return;
            continue;
        }
        double first = (b->box.lo[k] - a->box.hi[k]) / relative;
        double last = (b->box.hi[k] - a->box.lo[k]) / relative;
        double near = fmin(first, last);
        if (near > entry) { entry = near; axis = k; sign = relative > 0 ? -1 : 1; }
        exit = fmin(exit, fmax(first, last));
    }
    if (axis >= 0 && exit > fmax(0, entry) && inverse_mass(a, axis) + inverse_mass(b, axis) > 0)
        consider(hit, entry, i, j, axis, sign);
}

static bool release_water(const Body *body, SpillSet *spills, double time)
{
    if (body->liquid_l == 0) return true;
    Spill *spill = spill_add(spills);
    if (!spill) return false;
    strcpy(spill->source, body->name);
    spill->litres = body->liquid_l;
    spill->released_s = time;
    double center[3], omega[3];
    box_center(&body->box, center);
    spin_inertia(&body->spin, body->dry_kg + body->liquid_l, body->spin.momentum, omega, true);
    for (int j = 0; j < SPILL_PARCELS; ++j) {
        double local[3] = {(j%5-2)*0.4*body->spin.half[0], (j/5-2)*0.4*body->spin.half[1], 0};
        double offset[3], tangential[3];
        spin_rotate(&body->spin, local, offset, false);
        spin_cross(tangential, omega, offset);
        for (int k = 0; k < 3; ++k) {
            spill->parcels[j].point[k] = center[k] + offset[k];
            spill->parcels[j].velocity[k] = body->velocity[k] + tangential[k];
        }
    }
    return true;
}

static bool split(Body *bodies, int *count, int index, double energy, double time, SpillSet *spills)
{
    Body *body = &bodies[index];
    if (*count >= MAX_BODIES || strlen(body->name) > 62 || body->dry_kg < 0.001) return false;
    char name[65];
    snprintf(name, sizeof name, "%s#2", body->name);
    for (int i = 0; i < *count; ++i)
        if (!strcmp(bodies[i].name, name)) return false;
    int axis = body->box.hi[0] - body->box.lo[0] >= body->box.hi[1] - body->box.lo[1] ? 0 : 1;
    double midpoint = (body->box.lo[axis] + body->box.hi[axis]) * 0.5;
    if (midpoint <= body->box.lo[axis] || midpoint >= body->box.hi[axis]) return false;
    if (!release_water(body, spills, time)) return false;
    Body *piece = &bodies[(*count)++];
    *piece = *body;
    strcpy(piece->name, name);
    piece->parent = index;
    piece->piece = true;
    double quarter = (body->box.hi[axis]-body->box.lo[axis])*0.25;
    piece->origin[axis] += quarter;
    body->origin[axis] -= quarter;
    piece->box.lo[axis] = midpoint;
    body->box.hi[axis] = midpoint;
    body->dry_kg *= 0.5;
    piece->dry_kg = body->dry_kg;
    body->released_l += body->liquid_l;
    body->liquid_l = piece->liquid_l = 0;
    body->limit = piece->limit = 0;
    body->broken = true;
    body->fracture_j = energy;
    body->fracture_s = time;
    // Fracture partitions the current world-space collision hull. Its fragments start without spin.
    double travel = body->spin.travel;
    spin_load(&body->spin, &body->box, nullptr);
    spin_load(&piece->spin, &piece->box, nullptr);
    body->spin.travel = travel;
    return true;
}

static void contact(Body *body, int other, double energy, double time)
{
    Contact *target = &body->contacts[other];
    if (!target->occurred || energy > target->energy) *target = (Contact){energy, time, true};
    if (!body->has_impact || energy > body->impact_j) {
        body->has_impact = true;
        body->impact_j = energy;
        body->impact_s = time;
    }
}

static bool advance(Body *bodies, int *count, const PhysicsBox *room, double gravity,
                    double start, double duration, SpillSet *spills)
{
    double remaining = duration;
    for (int event = 0; event < MAX_IMPACTS && remaining > 1e-10; ++event) {
        double delta[MAX_BODIES][AXES] = {{0}}, accel[MAX_BODIES];
        Impact hit = {.fraction = 1, .a = -1};
        for (int i = 0; i < *count; ++i) {
            Body *body = &bodies[i];
            accel[i] = acceleration(bodies, *count, i, room, gravity);
            for (int axis = 0; axis < AXES; ++axis) {
                if (!(body->axes & (1 << axis))) continue;
                double d = body->velocity[axis] * remaining;
                if (axis == 2) d += 0.5 * accel[i] * remaining * remaining;
                delta[i][axis] = d;
                if (d < -epsilon)
                    consider(&hit, (room->lo[axis] - body->box.lo[axis]) / d, i, MAX_BODIES, axis, 1);
                else if (d > epsilon)
                    consider(&hit, (room->hi[axis] - body->box.hi[axis]) / d, i, MAX_BODIES, axis, -1);
            }
        }
        for (int i = 0; i < *count; ++i)
            for (int j = i + 1; j < *count; ++j)
                if (bodies[i].axes || bodies[j].axes)
                    pair_impact(&hit, &bodies[i], &bodies[j], delta[i], delta[j], i, j);
        const double elapsed = remaining * hit.fraction;
        for (int i = 0; i < *count; ++i) {
            for (int k = 0; k < AXES; ++k) {
                double offset = delta[i][k] * hit.fraction;
                bodies[i].box.lo[k] += offset;
                bodies[i].box.hi[k] += offset;
            }
            bodies[i].velocity[2] += accel[i] * elapsed;
        }
        remaining -= elapsed;
        if (hit.a < 0) return true;
        Body *a = &bodies[hit.a], *b = hit.b == MAX_BODIES ? nullptr : &bodies[hit.b];
        int axis = hit.axis;
        double ia = inverse_mass(a, axis), ib = b ? inverse_mass(b, axis) : 0;
        double velocity = (a->velocity[axis] - (b ? b->velocity[axis] : 0)) * hit.sign;
        double closing = fmax(0, -velocity);
        double energy = 0.5 * closing * closing / (ia + ib);
        bool break_a = ia > 0 && a->limit > 0 && energy >= a->limit;
        bool break_b = ib > 0 && b->limit > 0 && energy >= b->limit;
        double restitution = a->body && b && b->body ? fmin(a->restitution, b->restitution) :
                             a->body ? a->restitution : b ? b->restitution : 0;
        if (closing < 0.02 || break_a || break_b) restitution = 0;
        double impulse = (1 + restitution) * closing / (ia + ib) * hit.sign;
        a->velocity[axis] += ia * impulse;
        if (b) b->velocity[axis] -= ib * impulse;
        double time = start + duration - remaining;
        contact(a, hit.b, energy, time);
        if (b) contact(b, hit.a, energy, time);
        if (break_a && !split(bodies, count, hit.a, energy, time, spills)) return false;
        if (break_b && !split(bodies, count, hit.b, energy, time, spills)) return false;
    }
    return remaining <= 1e-10;
}

static void rotate_bodies(Body *bodies, int count, const PhysicsBox *room, double dt)
{
    for (int i = 0; i < count; ++i) {
        Body *body = &bodies[i];
        if (body->axes != ALL_AXES ||
            !(body->spin.momentum[0] || body->spin.momentum[1] || body->spin.momentum[2])) continue;
        double center[3];
        box_center(&body->box, center);
        RigidSpin rotated = body->spin;
        spin_step(&rotated, body->dry_kg + body->liquid_l, dt);
        PhysicsBox box = spin_bounds(&rotated, center);
        bool clear = true;
        for (int k = 0; k < 3; ++k)
            clear &= box.lo[k] >= room->lo[k]-epsilon && box.hi[k] <= room->hi[k]+epsilon;
        for (int j = 0; clear && j < count; ++j)
            if (i != j && overlap(&box, &bodies[j].box)) clear = false;
        if (clear) {
            body->box = box;
            body->spin = rotated;
        } else {
            memset(body->spin.momentum, 0, sizeof body->spin.momentum);
            body->rotation_blocked = true;
        }
    }
}

static bool configure_body(Json *object, const Json *op)
{
    double mass, liquid, bounce, velocity[AXES] = {0};
    PhysicsBox bounds;
    if (!object || strcmp(js(object, "kind"), "object") || at(object, "rigid") ||
        !box_read(object, &bounds) ||
        !physics_scalar(op, "mass_kg", .001, 1e6, &mass) ||
        !physics_optional(op, "liquid_l", 0, 1e5, 0, &liquid) ||
        !physics_optional(op, "restitution", 0, 1, 0, &bounce) ||
        (at(op, "velocity") && !physics_vector(op, "velocity", velocity))) return false;
    double volume = 1000;
    for (int k = 0; k < AXES; ++k) volume *= bounds.hi[k] - bounds.lo[k];
    if (liquid > volume) return false;
    Json *rigid = cJSON_CreateObject();
    num(rigid, "mass_kg", mass);
    num(rigid, "liquid_l", liquid);
    num(rigid, "restitution", bounce);
    integer(rigid, "axes", ALL_AXES);
    put(rigid, "velocity", cJSON_CreateDoubleArray(velocity, AXES));
    put(object, "rigid", rigid);
    return true;
}

static bool impulse(Json *object, const Json *op)
{
    Json *rigid = at(object, "rigid");
    PhysicsBox box;
    RigidSpin spin;
    double push[3], offset[3] = {0}, local[3], torque[3], speed[3];
    if (!rigid || !box_read(object, &box) || !spin_load(&spin, &box, rigid) ||
        !physics_vector(op, "impulse_ns", push) || !physics_vector(rigid, "velocity", speed) ||
        (at(op, "offset_m") && !physics_vector(op, "offset_m", offset))) return false;
    spin_rotate(&spin, offset, local, true);
    int axes = number(rigid, "axes");
    double mass = stored(rigid, "mass_kg") + stored(rigid, "liquid_l");
    if (!axes || mass <= 0) return false;
    spin_cross(torque, offset, push);
    for (int k = 0; k < 3; ++k) {
        if (fabs(local[k]) > spin.half[k] + epsilon) return false;
        if (axes & (1 << k)) speed[k] += push[k] / mass;
        if (axes == ALL_AXES) spin.momentum[k] += torque[k];
        if (fabs(speed[k]) > 1e6 || fabs(spin.momentum[k]) > 1e6) return false;
    }
    double omega[3];
    spin_inertia(&spin, mass, spin.momentum, omega, true);
    if (hypot(hypot(omega[0], omega[1]), omega[2]) > 120) return false;
    spin.travel = stored(rigid, "rotation_rad");
    spin_save(&spin, rigid);
    put(rigid, "velocity", cJSON_CreateDoubleArray(speed, 3));
    return true;
}

static bool constrain(Json *rigid, const Json *op)
{
    double velocity[AXES];
    if (!physics_vector(rigid, "velocity", velocity)) return false;
    const char *mode = js(op, "mode"), *axis = js(op, "axis");
    int axes = ALL_AXES;
    if (!strcmp(mode, "fixed")) axes = 0;
    else if (!strcmp(mode, "rail")) {
        if (!strcmp(axis, "x")) axes = 1;
        else if (!strcmp(axis, "y")) axes = 2;
        else if (!strcmp(axis, "z")) axes = 4;
        else return false;
    } else if (strcmp(mode, "free")) return false;
    for (int k = 0; k < AXES; ++k)
        if (!(axes & (1 << k))) velocity[k] = 0;
    integer(rigid, "axes", axes);
    if (axes != ALL_AXES && at(rigid, "orientation")) {
        const double zero[3] = {0};
        put(rigid, "angular_momentum", cJSON_CreateDoubleArray(zero, 3));
    }
    put(rigid, "velocity", cJSON_CreateDoubleArray(velocity, AXES));
    return true;
}

static void store_body(Json *item, const Body *bodies, int count, const Body *body)
{
    set(item, "name", body->name);
    put(item, "min", cJSON_CreateDoubleArray(body->box.lo, AXES));
    put(item, "max", cJSON_CreateDoubleArray(body->box.hi, AXES));
    Json *rigid = at(item, "rigid"), *contacts = cJSON_CreateArray();
    put(rigid, "velocity", cJSON_CreateDoubleArray(body->velocity, AXES));
    num(rigid, "mass_kg", body->dry_kg);
    num(rigid, "liquid_l", body->liquid_l);
    num(rigid, "released_l", body->released_l);
    double center[3];
    box_center(&body->box, center);
    num(rigid, "translation_m", hypot(hypot(center[0] - body->origin[0],
                                          center[1] - body->origin[1]), center[2] - body->origin[2]));
    spin_save(&body->spin, rigid);
    put(rigid, "rotation_blocked", cJSON_CreateBool(body->rotation_blocked));
    put(rigid, "fractured", cJSON_CreateBool(body->broken));
    if (body->parent >= 0) set(rigid, "fragment_of", bodies[body->parent].name);
    if (body->broken) {
        num(rigid, "fracture_energy_j", body->fracture_j);
        num(rigid, "fracture_time_s", body->fracture_s);
    }
    for (int i = 0; i <= count; ++i) {
        const Contact *contact = &body->contacts[i == count ? MAX_BODIES : i];
        if (!contact->occurred) continue;
        Json *event = cJSON_CreateObject();
        set(event, "target", i == count ? "@room" : bodies[i].name);
        num(event, "peak_impact_j", contact->energy);
        num(event, "peak_impact_time_s", contact->time);
        cJSON_AddItemToArray(contacts, event);
    }
    put(rigid, "contacts", contacts);
    if (body->has_impact) {
        num(rigid, "peak_impact_j", body->impact_j);
        num(rigid, "peak_impact_time_s", body->impact_s);
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(rigid, "peak_impact_j");
        cJSON_DeleteItemFromObjectCaseSensitive(rigid, "peak_impact_time_s");
    }
}

static bool step_room(Json *room, const Json *materials, const PhysicsBox *bounds, const Json *op, double gravity)
{
    double seconds;
    if (!physics_scalar(op, "seconds", 0.001, 10, &seconds)) return false;
    Json *objects = at(room, "objects"), *item;
    int count = cJSON_GetArraySize(objects);
    if (count > MAX_BODIES || (!count && !at(room, "spills"))) return false;
    Body *bodies = calloc(MAX_BODIES, sizeof *bodies);
    if (!bodies) return false;
    SpillSet spills = {0};
    bool valid = spill_load(&spills, room), any = spills.count != 0;
    SpillSurface surfaces[MAX_BODIES];
    int surface_count = 0;
    int i = 0;
    cJSON_ArrayForEach(item, objects) {
        if (!read_body(&bodies[i], item, materials, bounds)) { valid = false; break; }
        any |= bodies[i].body;
        if (!bodies[i].axes) surfaces[surface_count++] = (SpillSurface){bodies[i].box, bodies[i].name};
        for (int j = 0; j < i; ++j)
            if ((bodies[i].axes || bodies[j].axes) && overlap(&bodies[i].box, &bodies[j].box)) valid = false;
        ++i;
    }
    double start = stored(room, "rigid_time_s");
    int steps = (int)ceil(seconds * HZ);
    double dt = seconds / steps;
    for (int tick = 0; valid && any && tick < steps; ++tick) {
        double now = start + tick*dt;
        rotate_bodies(bodies, count, bounds, dt);
        valid = advance(bodies, &count, bounds, gravity, now, dt, &spills);
        if (valid && spills.count) spill_advance(&spills, bounds, surfaces, surface_count, gravity, now, now+dt);
    }
    if (valid && any) {
        int original = cJSON_GetArraySize(objects);
        for (int j = original; j < count; ++j)
            cJSON_AddItemToArray(objects, cJSON_Duplicate(bodies[j].source, true));
        for (int j = 0; j < count; ++j)
            if (bodies[j].body) store_body(cJSON_GetArrayItem(objects, j), bodies, count, &bodies[j]);
        num(room, "rigid_time_s", start + seconds);
        spill_store(&spills, room);
    }
    free(spills.items);
    free(bodies);
    return valid && any;
}

bool rigid_edit(Json *room, Json *object, const Json *materials, const PhysicsBox *bounds,
                const Json *op, double gravity)
{
    const char *tool = js(op, "neutrino_tool");
    if (!strcmp(tool, "scene_body")) return configure_body(object, op);
    if (!strcmp(tool, "scene_constraint")) return constrain(at(object, "rigid"), op);
    if (!strcmp(tool, "scene_impulse")) return impulse(object, op);
    return !strcmp(tool, "scene_step") && step_room(room, materials, bounds, op, gravity);
}
