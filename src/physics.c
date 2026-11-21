#include "app.h"
#include "ray.h"

#include <float.h>
#include <math.h>
#include <string.h>

static bool scalar(const Json *args, const char *key, double low, double high, double *value)
{
    const Json *n = at(args, key);
    if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble) ||
        n->valuedouble < low || n->valuedouble > high) return false;
    *value = n->valuedouble;
    return true;
}
enum { GRAVITY, OUTLET_COEFFICIENT, AMBIENT_KELVIN, WALL_REFLECTIVITY };
static const struct {
    const char *key;
    double initial, low, high;
} tuning[] = {
    [GRAVITY] = {"gravity", 9.80665, 0.1, 30},
    [OUTLET_COEFFICIENT] = {"outlet_coefficient", 0.62, 0.1, 1},
    [AMBIENT_KELVIN] = {"ambient_kelvin", 293.15, 150, 400},
    [WALL_REFLECTIVITY] = {"wall_reflectivity", 0.65, 0, 1},
};
static double setting(const Json *settings, size_t index)
{
    double value;
    return scalar(settings, tuning[index].key, tuning[index].low, tuning[index].high, &value)
        ? value : tuning[index].initial;
}
bool physics_tune(Json *settings, const Json *op)
{
    const char *key = js(op, "setting");
    for (size_t i = 0; i < sizeof tuning / sizeof *tuning; i++) {
        if (strcmp(key, tuning[i].key)) continue;
        double value;
        if (!scalar(op, "value", tuning[i].low, tuning[i].high, &value)) return false;
        put(settings, key, cJSON_CreateNumber(value));
        return true;
    }
    return false;
}
void physics_describe(const Json *settings, Text *out)
{
    format(out, "Physics: gravity %.6g m/s² · outlet coefficient %.5g · ambient %.5g K\n",
           setting(settings, GRAVITY), setting(settings, OUTLET_COEFFICIENT), setting(settings, AMBIENT_KELVIN));
    format(out, "Acoustic wall reflection: %.3g amplitude ratio\n", setting(settings, WALL_REFLECTIVITY));
}
static bool vector(const Json *args, const char *key, double values[3])
{
    const Json *v = at(args, key);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != 3) return false;
    for (int axis = 0; axis < 3; axis++) {
        const Json *n = cJSON_GetArrayItem(v, axis);
        if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble) || fabs(n->valuedouble) > 1e6) return false;
        values[axis] = n->valuedouble;
    }
    return true;
}
static bool ray(const Json *args, Text *out)
{
    double origin[3], direction[3], min[3], max[3];
    if (!vector(args, "origin", origin) || !vector(args, "direction", direction) ||
        !vector(args, "min", min) || !vector(args, "max", max)) return false;
    double norm = hypot(hypot(direction[0], direction[1]), direction[2]);
    if (norm < 1e-9) return false;
    for (int k = 0; k < 3; k++) if (min[k] > max[k]) return false;
    double first = 0, last = DBL_MAX;
    for (int k = 0; k < 3; k++) {
        const double dir = direction[k] / norm;
        if (fabs(dir) < 1e-12) {
            if (origin[k] < min[k] || origin[k] > max[k]) {
                append(out, "Ray misses the box.");
                return true;
            }
            continue;
        }
        double near = (min[k] - origin[k]) / dir, far = (max[k] - origin[k]) / dir;
        first = fmax(first, fmin(near, far));
        last = fmin(last, fmax(near, far));
    }
    if (first > last) append(out, "Ray misses the box.");
    else format(out, "Ray hits the box at %.5g m, point [%.5g, %.5g, %.5g] m.", first,
                origin[0] + direction[0] * first / norm, origin[1] + direction[1] * first / norm,
                origin[2] + direction[2] * first / norm);
    return true;
}

enum { RAY_BATCH_MAX = 128, ACOUSTIC_BLOCKERS = 16, ACOUSTIC_PATHS = 7 };
typedef struct { double lo[3], hi[3]; } Box;
typedef enum { BACKEND_AUTO, BACKEND_CPU, BACKEND_GPU, BACKEND_INVALID } Backend;

static Backend backend(const Json *args)
{
    const char *name = js(args, "backend");
    if (!at(args, "backend") || !strcmp(name, "auto")) return BACKEND_AUTO;
    if (!strcmp(name, "cpu")) return BACKEND_CPU;
    if (!strcmp(name, "gpu")) return BACKEND_GPU;
    return BACKEND_INVALID;
}
static bool read_box(const Json *json, Box *box)
{
    if (!vector(json, "min", box->lo) || !vector(json, "max", box->hi)) return false;
    for (int k = 0; k < 3; ++k)
        if (box->lo[k] > box->hi[k]) return false;
    return true;
}
static double length3(const double v[3])
{
    return hypot(hypot(v[0], v[1]), v[2]);
}
static void pack_ray(float row[RAY_VALUES], const double origin[3], const double direction[3], const Box *box)
{
    for (int k = 0; k < 3; ++k) {
        row[k] = 0.0f;
        row[3 + k] = (float)direction[k];
        row[6 + k] = (float)(box->lo[k] - origin[k]);
        row[9 + k] = (float)(box->hi[k] - origin[k]);
    }
}
static float trace_cpu(const float *row)
{
    RAY_SLAB_BODY
}
static const char *trace_batch(Backend selected, const float *rays, size_t count, float *hits)
{
    if ((selected == BACKEND_GPU || (selected == BACKEND_AUTO && count >= 32)) &&
        ray_gpu(rays, count, hits)) return "OpenCL GPU";
    if (selected == BACKEND_GPU && count) return nullptr;
    for (size_t i = 0; i < count; ++i) hits[i] = trace_cpu(rays + i * RAY_VALUES);
    return "CPU";
}
static bool rays(const Json *args, Text *out)
{
    const Json *entries = at(args, "rays"), *entry;
    Box box;
    Backend selected = backend(args);
    int count = cJSON_GetArraySize(entries);
    if (!read_box(args, &box) || selected == BACKEND_INVALID ||
        !cJSON_IsArray(entries) || count < 1 || count > RAY_BATCH_MAX) return false;
    float rows[RAY_BATCH_MAX * RAY_VALUES], hits[RAY_BATCH_MAX];
    int index = 0;
    cJSON_ArrayForEach(entry, entries) {
        double o[3], d[3];
        if (!vector(entry, "origin", o) || !vector(entry, "direction", d)) return false;
        double norm = length3(d);
        if (norm < 1e-9) return false;
        for (int k = 0; k < 3; ++k) d[k] /= norm;
        pack_ray(rows + index++ * RAY_VALUES, o, d, &box);
    }
    const char *engine = trace_batch(selected, rows, (size_t)count, hits);
    if (!engine) { append(out, "OpenCL GPU unavailable for this session."); return true; }
    format(out, "%s ray batch (%d) hit distances in m:", engine, count);
    for (int i = 0; i < count; ++i) {
        if (hits[i] < 0) append(out, " miss");
        else format(out, " %.5g", (double)hits[i]);
    }
    return true;
}
static bool strictly_inside(const Box *room, const double position[3])
{
    for (int k = 0; k < 3; ++k)
        if (position[k] <= room->lo[k] || position[k] >= room->hi[k]) return false;
    return true;
}
static double separation(const double a[3], const double b[3])
{
    double d[3];
    for (int k = 0; k < 3; ++k) d[k] = a[k] - b[k];
    return length3(d);
}
static void pack_segment(float *row, const double a[3], const double b[3], const Box *box)
{
    double segment[3];
    for (int k = 0; k < 3; ++k) segment[k] = b[k] - a[k];
    pack_ray(row, a, segment, box);
}
static bool acoustic(const Json *args, const Json *settings, Text *out)
{
    Box room, blockers[ACOUSTIC_BLOCKERS];
    const Json *boxes = at(args, "blockers"), *item;
    int obstacles = cJSON_GetArraySize(boxes);
    double source[3], listener[3], bounce[ACOUSTIC_PATHS-1][3];
    Backend selected = backend(args);
    if (selected == BACKEND_INVALID || obstacles > ACOUSTIC_BLOCKERS ||
        (boxes && !cJSON_IsArray(boxes)) || !read_box(args, &room) ||
        !vector(args, "source", source) || !vector(args, "listener", listener) ||
        !strictly_inside(&room, source) || !strictly_inside(&room, listener)) return false;
    int i = 0;
    cJSON_ArrayForEach(item, boxes)
        if (!read_box(item, &blockers[i++])) return false;
    double distances[ACOUSTIC_PATHS] = {separation(source, listener)};
    const double reflection = setting(settings, WALL_REFLECTIVITY);
    if (distances[0] < 1e-6) return false;
    for (int face = 0; face < ACOUSTIC_PATHS - 1; ++face) {
        int axis = face / 2;
        double wall = face % 2 ? room.hi[axis] : room.lo[axis];
        double image[3] = {source[0], source[1], source[2]};
        image[axis] = 2 * wall - source[axis];
        double fraction = (wall - listener[axis]) / (image[axis] - listener[axis]);
        for (int k = 0; k < 3; ++k)
            bounce[face][k] = listener[k] + fraction * (image[k] - listener[k]);
        bounce[face][axis] = wall;
        distances[face + 1] = separation(image, listener);
    }
    enum { MAX_SEGMENTS = 1 + 2 * (ACOUSTIC_PATHS - 1), MAX_TESTS = MAX_SEGMENTS * ACOUSTIC_BLOCKERS };
    float rows[MAX_TESTS * RAY_VALUES], hits[MAX_TESTS];
    int path_of[MAX_TESTS];
    size_t total = 0;
    for (int path = 0; path < ACOUSTIC_PATHS; ++path) {
        if (path && reflection == 0) break;
        for (int b = 0; b < obstacles; ++b) {
            const double *end = path ? bounce[path - 1] : listener;
            pack_segment(rows + total * RAY_VALUES, source, end, &blockers[b]);
            path_of[total++] = path;
            if (path) {
                pack_segment(rows + total * RAY_VALUES, end, listener, &blockers[b]);
                path_of[total++] = path;
            }
        }
    }
    const char *engine = trace_batch(selected, rows, total, hits);
    if (!engine) { append(out, "OpenCL GPU unavailable for this session."); return true; }
    bool blocked[ACOUSTIC_PATHS] = {0};
    for (size_t j = 0; j < total; ++j)
        if (hits[j] >= 0 && hits[j] < 1.0f) blocked[path_of[j]] = true;
    const double speed = sqrt(1.4 * 287.05 * setting(settings, AMBIENT_KELVIN));
    const char *const paths[] = {"Direct", "X-min echo", "X-max echo", "Y-min echo",
        "Y-max echo", "Z-min echo", "Z-max echo"};
    format(out, "Acoustic paths (%s visibility, %.4g m/s). Levels relative to the direct source at 1 m.",
           total ? engine : "no blocker tests", speed);
    for (int path = 0; path < ACOUSTIC_PATHS; ++path) {
        if (path && reflection == 0) break;
        if (blocked[path]) format(out, "\n%s blocked.", paths[path]);
        else format(out, "\n%s %.4g ms · %+.3g dB", paths[path], 1000 * distances[path] / speed,
                    20 * log10((path ? reflection : 1) / distances[path]));
    }
    return true;
}
static bool fluid(const Json *args, const Json *settings, Text *out)
{
    double height, tank, hole, seconds;
    if (!scalar(args, "height", 0, 1000, &height) || !scalar(args, "tank_area", 1e-6, 1e6, &tank) ||
        !scalar(args, "outlet_area", 0, 1000, &hole) || !scalar(args, "seconds", 0, 86400, &seconds) ||
        hole > tank * 0.1) return false;
    const double rate = setting(settings, OUTLET_COEFFICIENT) * hole * sqrt(2 * setting(settings, GRAVITY));
    const double remaining = fmax(0, sqrt(height) - rate * seconds / (2 * tank));
    const double level = remaining * remaining;
    format(out, "Tank drain estimate: final level %.5g m, drained %.5g L, final flow %.5g L/s.",
           level, (height - level) * tank * 1000, rate * remaining * 1000);
    return true;
}
static bool gas(const Json *args, const Json *settings, Text *out)
{
    double moles, volume, leak, seconds, kelvin = setting(settings, AMBIENT_KELVIN);
    if (!scalar(args, "moles", 0, 1e9, &moles) || !scalar(args, "volume", 0.001, 1e9, &volume) ||
        (at(args, "kelvin") && !scalar(args, "kelvin", 50, 2000, &kelvin)) || !scalar(args, "leak", 0, 1e6, &leak) ||
        !scalar(args, "seconds", 0, 86400, &seconds)) return false;
    const double left = fmax(0, moles - leak * seconds);
    format(out, "Ideal-gas room estimate: %.5g mol remaining, %.5g kPa at fixed temperature, %.5g mol/m³.",
           left, left * 8.314462618 * kelvin / (volume * 1000), left / volume);
    return true;
}
static bool prop_load(const Json *args, Text *out)
{
    const Json *field;
    cJSON_ArrayForEach(field, args)
        if (!field->string || (strcmp(field->string, "neutrino_tool") &&
            strcmp(field->string, "strength") && strcmp(field->string, "load"))) return false;
    const char *strength = js(args, "strength"), *load = js(args, "load");
    const char *const strengths[] = {"fragile", "ordinary", "reinforced"};
    const char *const loads[] = {"light", "steady", "heavy"};
    size_t capacity = 0, stress = 0;
    while (capacity < 3 && strcmp(strength, strengths[capacity])) capacity++;
    while (stress < 3 && strcmp(load, loads[stress])) stress++;
    if (capacity == 3 || stress == 3) return false;
    format(out, "Fictional scenery under load: %s.",
           stress < capacity ? "holds" : stress == capacity ? "gives slightly" : "breaks");
    return true;
}
bool physics_run(const char *name, const Json *args, const Json *settings, Text *out)
{
    if (!strcmp(name, "ray")) return ray(args, out);
    if (!strcmp(name, "rays")) return rays(args, out);
    if (!strcmp(name, "acoustic")) return acoustic(args, settings, out);
    if (!strcmp(name, "fluid")) return fluid(args, settings, out);
    if (!strcmp(name, "gas")) return gas(args, settings, out);
    if (!strcmp(name, "prop_load")) return prop_load(args, out);
    return false;
}
