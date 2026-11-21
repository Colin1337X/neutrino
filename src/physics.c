#include "app.h"

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
enum { GRAVITY, OUTLET_COEFFICIENT, AMBIENT_KELVIN };
static const struct {
    const char *key;
    double initial, low, high;
} tuning[] = {
    [GRAVITY] = {"gravity", 9.80665, 0.1, 30},
    [OUTLET_COEFFICIENT] = {"outlet_coefficient", 0.62, 0.1, 1},
    [AMBIENT_KELVIN] = {"ambient_kelvin", 293.15, 150, 400},
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
    if (!strcmp(name, "fluid")) return fluid(args, settings, out);
    if (!strcmp(name, "gas")) return gas(args, settings, out);
    if (!strcmp(name, "prop_load")) return prop_load(args, out);
    return false;
}
