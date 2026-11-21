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
static bool fluid(const Json *args, Text *out)
{
    double height, tank, hole, seconds;
    if (!scalar(args, "height", 0, 1000, &height) || !scalar(args, "tank_area", 1e-6, 1e6, &tank) ||
        !scalar(args, "outlet_area", 0, 1000, &hole) || !scalar(args, "seconds", 0, 86400, &seconds) ||
        hole > tank * 0.1) return false;
    const double rate = 0.62 * hole * sqrt(2 * 9.80665);
    const double remaining = fmax(0, sqrt(height) - rate * seconds / (2 * tank));
    const double level = remaining * remaining;
    format(out, "Tank drain estimate: final level %.5g m, drained %.5g L, final flow %.5g L/s.",
           level, (height - level) * tank * 1000, rate * remaining * 1000);
    return true;
}
static bool gas(const Json *args, Text *out)
{
    double moles, volume, kelvin, leak, seconds;
    if (!scalar(args, "moles", 0, 1e9, &moles) || !scalar(args, "volume", 0.001, 1e9, &volume) ||
        !scalar(args, "kelvin", 50, 2000, &kelvin) || !scalar(args, "leak", 0, 1e6, &leak) ||
        !scalar(args, "seconds", 0, 86400, &seconds)) return false;
    const double left = fmax(0, moles - leak * seconds);
    format(out, "Ideal-gas room estimate: %.5g mol remaining, %.5g kPa at fixed temperature, %.5g mol/m³.",
           left, left * 8.314462618 * kelvin / (volume * 1000), left / volume);
    return true;
}
bool physics_run(const char *name, const Json *args, Text *out)
{
    if (!strcmp(name, "ray")) return ray(args, out);
    if (!strcmp(name, "fluid")) return fluid(args, out);
    if (!strcmp(name, "gas")) return gas(args, out);
    return false;
}
