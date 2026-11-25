#include "app.h"
#include "ray.h"
#include "physics_models.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool physics_scalar(const Json *args, const char *key, double low, double high, double *value)
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
    return physics_scalar(settings, tuning[index].key, tuning[index].low, tuning[index].high, &value)
        ? value : tuning[index].initial;
}
bool physics_tune(Json *settings, const Json *op)
{
    const char *key = js(op, "setting");
    for (size_t i = 0; i < sizeof tuning / sizeof *tuning; i++) {
        if (strcmp(key, tuning[i].key)) continue;
        double value;
        if (!physics_scalar(op, "value", tuning[i].low, tuning[i].high, &value)) return false;
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
static bool vector_value(const Json *v, double values[3])
{
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != 3) return false;
    for (int axis = 0; axis < 3; axis++) {
        const Json *n = cJSON_GetArrayItem(v, axis);
        if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble) || fabs(n->valuedouble) > 1e6) return false;
        values[axis] = n->valuedouble;
    }
    return true;
}
bool physics_vector(const Json *args, const char *key, double values[3])
{
    return vector_value(at(args, key), values);
}
static bool ray(const Json *args, Text *out)
{
    double origin[3], direction[3], min[3], max[3];
    if (!physics_vector(args, "origin", origin) || !physics_vector(args, "direction", direction) ||
        !physics_vector(args, "min", min) || !physics_vector(args, "max", max)) return false;
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

enum { RAY_BATCH_MAX = 4096 };
typedef PhysicsBox Box;
typedef enum { BACKEND_AUTO, BACKEND_CPU, BACKEND_GPU, BACKEND_VULKAN, BACKEND_RT, BACKEND_INVALID } Backend;

static Backend backend(const Json *args)
{
    const char *name = js(args, "backend");
    if (!at(args, "backend") || !strcmp(name, "auto")) return BACKEND_AUTO;
    if (!strcmp(name, "cpu")) return BACKEND_CPU;
    if (!strcmp(name, "gpu")) return BACKEND_GPU;
    if (!strcmp(name, "vulkan")) return BACKEND_VULKAN;
    if (!strcmp(name, "rt")) return BACKEND_RT;
    return BACKEND_INVALID;
}
static bool read_box(const Json *json, Box *box)
{
    if (!physics_vector(json, "min", box->lo) || !physics_vector(json, "max", box->hi)) return false;
    for (int k = 0; k < 3; ++k)
        if (box->lo[k] > box->hi[k]) return false;
    return true;
}

static bool positive_box(const Box *box)
{
    for (int axis = 0; axis < 3; ++axis)
        if (box->hi[axis] <= box->lo[axis]) return false;
    return true;
}

static bool contained_box(const Box *outer, const Box *inner)
{
    for (int axis = 0; axis < 3; ++axis)
        if (inner->lo[axis] < outer->lo[axis] || inner->hi[axis] > outer->hi[axis]) return false;
    return true;
}
static bool room_flow(const Json *args, const Box *room, FlowConfig *config)
{
    double length[3];
    for (int axis = 0; axis < 3; ++axis) length[axis] = room->hi[axis] - room->lo[axis];
    return flow_parse(args, length, config) &&
        (!at(args, "solid_geometry") || (cJSON_IsBool(at(args, "solid_geometry")) &&
        (!cJSON_IsTrue(at(args, "solid_geometry")) || config->cfd)));
}

static FlowField room_flow_solve(const Json *room, const Box *bounds, const FlowConfig *config)
{
    if (!cJSON_IsTrue(at(at(room, "medium"), "solid_geometry"))) return flow_solve(config);
    Box solids[128];
    size_t count = 0;
    const Json *item;
    cJSON_ArrayForEach(item, at(room, "objects")) {
        if (count == sizeof solids / sizeof *solids || !read_box(item, &solids[count])) {
            return (FlowField){.fallback = "CFD requires at most 128 valid obstacle boxes"};
        }
        for (int axis = 0; axis < 3; ++axis) {
            solids[count].lo[axis] -= bounds->lo[axis];
            solids[count].hi[axis] -= bounds->lo[axis];
        }
        ++count;
    }
    return flow_solve_domain(config, solids, count);
}

static Json *scene_object(const Json *objects, const char *name)
{
    Json *item;
    cJSON_ArrayForEach(item, objects)
        if (!strcmp(js(item, "name"), name)) return item;
    return nullptr;
}

static Json *active_room(const Json *settings)
{
    const char *name = js(settings,"active3d");
    return *name ? scene_object(at(settings,"rooms3d"),name) : at(settings,"room3d");
}

static bool scene_name(const char *name)
{
    if (!*name || strlen(name) > 64) return false;
    for (; *name; ++name)
        if ((unsigned char)*name < 32 || (unsigned char)*name == 127) return false;
    return true;
}

int physics_scene_relation(const Json *settings, const char *entity, const char *target)
{
    const Json *room = active_room(settings), *objects = at(room, "objects");
    if (!room) return -1;
    const Json *object = scene_object(objects, entity);
    if (!target) return object != nullptr;
    Box a, b;
    if (!read_box(object, &a) || !read_box(scene_object(objects, target), &b)) return -1;
    for (int axis = 0; axis < 3; ++axis)
        if (a.hi[axis] <= b.lo[axis] || b.hi[axis] <= a.lo[axis]) return 0;
    return 1;
}

static bool acoustic_properties(const Json *material, double *loss, double *reflectivity)
{
    return cJSON_IsObject(material) && physics_scalar(material, "loss_db_per_m", 0, 1e6, loss) &&
        physics_scalar(material, "reflectivity", 0, 1, reflectivity);
}

static bool optical_properties(const Json *optical, double *transmission, double *reflectivity, double *loss)
{
    return cJSON_IsObject(optical) && physics_optional(optical, "transmission", 0, 1, 0, transmission) &&
        physics_optional(optical, "reflectivity", 0, 1, 0, reflectivity) &&
        physics_optional(optical, "loss_db_per_m", 0, 1e6, 0, loss) && *transmission + *reflectivity <= 1;
}

bool physics_scene_edit(Json *settings, const Json *op)
{
    const char *tool = js(op, "neutrino_tool");
    if (!strcmp(tool, "scene_material")) {
        const char *name = js(op, "name");
        const Json *acoustic = at(op, "acoustic"), *optical = at(op, "optical");
        double first, second, third;
        if (!scene_name(name) || (!acoustic && !optical) ||
            (acoustic && !acoustic_properties(acoustic, &first, &second)) ||
            (optical && !optical_properties(optical, &first, &second, &third))) return false;
        Json *materials = at(settings, "materials3d");
        if (!materials) put(settings, "materials3d", materials = cJSON_CreateObject());
        Json *entry = cJSON_CreateObject();
        if (acoustic) put(entry, "material", cJSON_Duplicate(acoustic, true));
        if (optical) put(entry, "optical", cJSON_Duplicate(optical, true));
        put(materials, name, entry);
        return true;
    }
    if (!strcmp(tool,"scene_select")) {
        const char *name = js(op,"name");
        if (!cJSON_IsString(at(op,"name")) ||
            (*name ? !scene_object(at(settings,"rooms3d"),name) : !at(settings,"room3d"))) return false;
        set(settings,"active3d",name);
        return true;
    }
    Json *room = active_room(settings), *objects = at(room, "objects");
    Box bounds, shape;
    if (!strcmp(tool, "scene_room")) {
        const char *name = js(op,"name");
        Json *layouts = at(settings,"rooms3d");
        if (at(op,"name")) {
            if (!scene_name(name)) return false;
            room = scene_object(layouts,name);
            objects = at(room,"objects");
        }
        if (!read_box(op, &bounds) || !positive_box(&bounds)) return false;
        Json *item;
        cJSON_ArrayForEach(item, objects)
            if (!read_box(item, &shape) || !contained_box(&bounds, &shape)) return false;
        FlowConfig config;
        if (at(room, "medium") && !room_flow(at(room, "medium"), &bounds, &config)) return false;
        if (!room) {
            room = cJSON_CreateObject();
            if (*name) {
                if (!layouts) put(settings,"rooms3d",layouts = cJSON_CreateArray());
                set(room,"name",name);
                cJSON_AddItemToArray(layouts,room);
            } else put(settings,"room3d",room);
            put(room, "objects", cJSON_CreateArray());
        }
        if (*name) set(settings,"active3d",name);
        put(room, "min", cJSON_Duplicate(at(op, "min"), true));
        put(room, "max", cJSON_Duplicate(at(op, "max"), true));
        return true;
    }
    if (!room || !read_box(room, &bounds)) return false;
    if (!strcmp(tool, "scene_air")) {
        const char *id = js(op, "material_id");
        if (!cJSON_IsString(at(op, "material_id")) ||
            (*id && !at(at(settings, "materials3d"), id))) return false;
        set(room, "air", id);
        return true;
    }
    if (!strcmp(tool, "scene_medium")) {
        if (cJSON_IsTrue(at(op, "off"))) {
            cJSON_DeleteItemFromObjectCaseSensitive(room, "medium");
            return true;
        }
        FlowConfig config;
        if (!room_flow(op, &bounds, &config)) return false;
        Json *medium = cJSON_CreateObject();
        const char *const keys[] = {"method","seconds","sigma_m","concentration","viscosity","diffusivity","vortex_m_s",
                                    "resolution","center","wind","sound_loss","light_loss","memory","backend","solid_geometry"};
        for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i)
            if (at(op,keys[i])) put(medium,keys[i],cJSON_Duplicate(at(op,keys[i]),true));
        put(room, "medium", medium);
        return true;
    }
    const char *name = js(op, "name");
    if (!scene_name(name)) return false;
    Json *object = scene_object(objects, name);
    if (!strcmp(tool, "scene_translate")) {
        double offset[3];
        if (!read_box(object, &shape) || !physics_vector(op, "offset", offset)) return false;
        for (int axis = 0; axis < 3; ++axis) {
            shape.lo[axis] += offset[axis];
            shape.hi[axis] += offset[axis];
        }
        if (!contained_box(&bounds, &shape)) return false;
        put(object, "min", cJSON_CreateDoubleArray(shape.lo, 3));
        put(object, "max", cJSON_CreateDoubleArray(shape.hi, 3));
        return true;
    }
    if (!strcmp(tool, "scene_remove")) {
        if (!object) return false;
        cJSON_Delete(cJSON_DetachItemViaPointer(objects, object));
        return true;
    }
    if (strcmp(tool, "scene_place") || !read_box(op, &shape) || !positive_box(&shape) ||
        !contained_box(&bounds, &shape)) return false;
    const char *kind = js(op, "kind");
    if (strcmp(kind, "object") && strcmp(kind, "wall")) return false;
    const Json *material = at(op, "material");
    const Json *optical = at(op, "optical");
    const char *material_id = js(op, "material_id");
    if (at(op, "material_id") && (!scene_name(material_id) || material || optical ||
        !at(at(settings, "materials3d"), material_id))) return false;
    double loss, reflectivity;
    double transmission, optical_reflection, optical_loss;
    if (material && !acoustic_properties(material, &loss, &reflectivity)) return false;
    if (optical && !optical_properties(optical, &transmission, &optical_reflection, &optical_loss)) return false;
    Json *copy = cJSON_CreateObject();
    set(copy, "name", name);
    set(copy, "kind", kind);
    put(copy, "min", cJSON_Duplicate(at(op, "min"), true));
    put(copy, "max", cJSON_Duplicate(at(op, "max"), true));
    if (*material_id) set(copy, "material_id", material_id);
    if (material) {
        Json *attributes = cJSON_CreateObject();
        put(attributes, "loss_db_per_m", cJSON_CreateNumber(loss));
        put(attributes, "reflectivity", cJSON_CreateNumber(reflectivity));
        put(copy, "material", attributes);
    }
    if (optical) {
        Json *attributes = cJSON_CreateObject();
        put(attributes,"transmission",cJSON_CreateNumber(transmission));
        put(attributes,"reflectivity",cJSON_CreateNumber(optical_reflection));
        put(attributes,"loss_db_per_m",cJSON_CreateNumber(optical_loss));
        put(copy,"optical",attributes);
    }
    if (object) {
        if (!cJSON_ReplaceItemViaPointer(objects, object, copy)) {
            cJSON_Delete(copy);
            return false;
        }
    } else cJSON_AddItemToArray(objects, copy);
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
#ifdef NEUTRINO_VULKAN_RT
static const char hardware_rt[] = "Vulkan hardware RT";
static RtBox rt_box(const Box *box, const double base[3])
{
    RtBox result;
    for (int axis = 0; axis < 3; ++axis) {
        result.lo[axis] = (float)(box->lo[axis] - base[axis]);
        result.hi[axis] = (float)(box->hi[axis] - base[axis]);
    }
    return result;
}
static RtRay rt_ray(const double origin[3], const double direction[3], const double base[3])
{
    RtRay result;
    for (int axis = 0; axis < 3; ++axis) {
        result.origin[axis] = (float)(origin[axis] - base[axis]);
        result.direction[axis] = (float)direction[axis];
    }
    return result;
}
#endif
static const char *trace_batch(Backend selected, const float *rays, size_t count, float *hits)
{
    if (!count) return "CPU";
#ifdef NEUTRINO_VULKAN
    if (selected == BACKEND_VULKAN || (selected == BACKEND_AUTO && count >= 32)) {
        const char *engine = ray_vulkan(rays, count, hits, selected == BACKEND_AUTO);
        if (engine) return engine;
    }
#endif
    if (selected == BACKEND_VULKAN || selected == BACKEND_RT) return nullptr;
    if ((selected == BACKEND_GPU || (selected == BACKEND_AUTO && count >= 32)) &&
        ray_gpu(rays, count, hits)) return "OpenCL GPU";
    if (selected == BACKEND_GPU && count) return nullptr;
    for (size_t i = 0; i < count; ++i) hits[i] = trace_cpu(rays + i * RAY_VALUES);
    return "CPU";
}

static const char *backend_unavailable(Backend selected)
{
    return selected == BACKEND_RT ? "Vulkan hardware RT unavailable for this session." :
        selected == BACKEND_VULKAN ? "Vulkan compute unavailable for this session." :
        "OpenCL GPU unavailable for this session.";
}
static bool rays(const Json *args, Text *out, PhysicsReading *reading)
{
    const Json *entries = at(args, "rays"), *entry;
    Box box;
    Backend selected = backend(args);
    int count = cJSON_GetArraySize(entries);
    if (!read_box(args, &box) || selected == BACKEND_INVALID ||
        !cJSON_IsArray(entries) || count < 1 || count > RAY_BATCH_MAX) return false;
    float rows[RAY_BATCH_MAX * RAY_VALUES], hits[RAY_BATCH_MAX];
#ifdef NEUTRINO_VULKAN_RT
    RtRay rt_rays[RAY_BATCH_MAX];
    RtHit rt_hits[RAY_BATCH_MAX];
#endif
    int index = 0;
    cJSON_ArrayForEach(entry, entries) {
        double o[3], d[3];
        if (!physics_vector(entry, "origin", o) || !physics_vector(entry, "direction", d)) return false;
        double norm = length3(d);
        if (norm < 1e-9) return false;
        for (int k = 0; k < 3; ++k) d[k] /= norm;
#ifdef NEUTRINO_VULKAN_RT
        if (selected == BACKEND_RT) rt_rays[index] = rt_ray(o, d, box.lo);
        else
#endif
            pack_ray(rows + index * RAY_VALUES, o, d, &box);
        ++index;
    }
    const char *engine = nullptr;
#ifdef NEUTRINO_VULKAN_RT
    if (selected == BACKEND_RT) {
        RtBox shape = rt_box(&box, box.lo);
        if (ray_vulkan_rt(rt_rays, (size_t)count, &shape, 1, rt_hits)) {
            engine = hardware_rt;
            for (int i = 0; i < count; ++i) hits[i] = rt_hits[i].distance;
        }
    } else
#endif
        engine = trace_batch(selected, rows, (size_t)count, hits);
    if (!engine) {
        if (reading) reading->unavailable = true;
        append(out, backend_unavailable(selected));
        return true;
    }
    format(out, "%s ray batch (%d) hit distances in m:", engine, count);
    int shown = count <= 128 ? count : 16;
    for (int i = 0; i < shown; ++i) {
        if (hits[i] < 0) append(out, " miss");
        else format(out, " %.5g", (double)hits[i]);
    }
    if (shown < count) {
        int total = 0;
        for (int i = 0; i < count; ++i) total += hits[i] >= 0;
        format(out, " (first %d; %d hits, %d misses total)", shown, total, count - total);
    }
    return true;
}
static bool strictly_inside(const Box *room, const double position[3])
{
    for (int k = 0; k < 3; ++k)
        if (position[k] <= room->lo[k] || position[k] >= room->hi[k]) return false;
    return true;
}

static bool scene_rays(const Json *args, const Json *scene, const Box *bounds,
                       Text *out, PhysicsReading *reading)
{
    double origin[3];
    if (!physics_vector(args, "origin", origin) || !strictly_inside(bounds, origin)) return false;
    const Json *directions = at(args, "directions"), *count_arg = at(args, "count");
    if (directions && (!cJSON_IsArray(directions) || count_arg)) return false;
    double generated = 0;
    if (!directions && (!physics_scalar(args, "count", 1, RAY_BATCH_MAX, &generated) ||
                        floor(generated) != generated)) return false;
    const int count = directions ? cJSON_GetArraySize(directions) : (int)generated;
    const Backend selected = backend(args);
    if (count < 1 || count > RAY_BATCH_MAX || selected == BACKEND_INVALID) return false;
    float rows[RAY_BATCH_MAX * RAY_VALUES], hits[RAY_BATCH_MAX];
    double vectors[RAY_BATCH_MAX][3];
    float nearest[RAY_BATCH_MAX];
    unsigned int winner[RAY_BATCH_MAX] = {0};
    const Json *direction = directions ? directions->child : nullptr;
    for (int i = 0; i < count; ++i) {
        double *d = vectors[i];
        if (directions) {
            if (!vector_value(direction, d)) return false;
            direction = direction->next;
            double norm = length3(d);
            if (norm < 1e-9) return false;
            for (int axis = 0; axis < 3; ++axis) d[axis] /= norm;
        } else {
            d[2] = 1 - 2 * (i + 0.5) / count;
            double radius = sqrt(1 - d[2] * d[2]), angle = i * 2.399963229728653;
            d[0] = radius * cos(angle);
            d[1] = radius * sin(angle);
        }
        double exit = DBL_MAX;
        for (int axis = 0; axis < 3; ++axis) {
            if (d[axis] > 0) exit = fmin(exit, (bounds->hi[axis] - origin[axis]) / d[axis]);
            if (d[axis] < 0) exit = fmin(exit, (bounds->lo[axis] - origin[axis]) / d[axis]);
        }
        nearest[i] = (float)exit;
    }
    const Json *objects = at(scene,"objects");
    const char *engine = nullptr;
    unsigned int base = 1;
#ifdef NEUTRINO_VULKAN_RT
    RtRay rt_rays[RAY_BATCH_MAX];
    if (selected == BACKEND_RT)
        for (int i=0; i<count; ++i) rt_rays[i] = rt_ray(origin,vectors[i],bounds->lo);
#endif
    for (const Json *object = objects ? objects->child : nullptr; object;) {
        if (selected == BACKEND_RT) {
#ifdef NEUTRINO_VULKAN_RT
            RtHit rt_hits[RAY_BATCH_MAX];
            RtBox boxes[ACOUSTIC_BLOCKERS];
            int batch = 0;
            for (; object && batch<ACOUSTIC_BLOCKERS; object=object->next) {
                Box shape;
                if (!read_box(object,&shape)) return false;
                boxes[batch++] = rt_box(&shape,bounds->lo);
            }
            if (!ray_vulkan_rt(rt_rays,(size_t)count,boxes,(size_t)batch,rt_hits)) goto unavailable;
            engine = hardware_rt;
            for (int i=0; i<count; ++i) {
                float hit = rt_hits[i].distance;
                if (hit >= 0 && (hit < nearest[i] || (!winner[i] && hit == nearest[i])) &&
                    rt_hits[i].box >= 0 && rt_hits[i].box < batch) {
                    nearest[i] = hit;
                    winner[i] = base + (unsigned int)rt_hits[i].box;
                }
            }
            base += (unsigned int)batch;
#else
            goto unavailable;
#endif
        } else {
            Box shape;
            if (!read_box(object,&shape)) return false;
            for (int i=0; i<count; ++i) pack_ray(rows+i*RAY_VALUES,origin,vectors[i],&shape);
            engine = trace_batch(selected,rows,count,hits);
            if (!engine) goto unavailable;
            for (int i=0; i<count; ++i) {
                if (hits[i] >= 0 && (hits[i] < nearest[i] || (!winner[i] && hits[i] == nearest[i]))) {
                    nearest[i] = hits[i];
                    winner[i] = base;
                }
            }
            ++base;
            object = object->next;
        }
    }
    unsigned int sample[12];
    memcpy(sample,winner,(size_t)(count<12 ? count : 12)*sizeof *sample);
    format(out,"3D scene raycast (%s): %d rays. First contacts:",engine ? engine : "analytic room boundary",count);
    for (int i=0; i<count; ++i) {
        int hits_here = 1;
        const unsigned int id = winner[i];
        if (id == UINT_MAX) continue;
        for (int j=i+1; j<count; ++j) if (winner[j] == id) ++hits_here;
        const char *name = id ? js(cJSON_GetArrayItem(objects,(int)(id-1)),"name") : "room boundary";
        format(out," %s=%d;",name,hits_here);
        if (out->len > 8000) { append(out," (additional contact groups omitted)"); break; }
        /* Only mark duplicate rays after recording the group's total. */
        for (int j=i+1; j<count; ++j) if (winner[j] == id) winner[j] = UINT_MAX;
    }
    append(out,"\nSample hit distances:");
    /* The first-contact IDs are needed again for the sample. Preserve them before grouping. */
    for (int i=0; i<count && i<12; ++i) {
        const unsigned int id = sample[i];
        const char *name = id ? js(cJSON_GetArrayItem(objects,(int)(id-1)),"name") : "room boundary";
        format(out," %s %.5g m;",name,(double)nearest[i]);
    }
    return true;

unavailable:
    if (reading) reading->unavailable = true;
    append(out,backend_unavailable(selected));
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

typedef struct {
    Box box;
    bool has_material;
    double loss_per_m, reflection, surface_loss;
} AcousticObject;

typedef struct {
    double bounce[3], reflection;
    int object, face;
} AcousticPath;

static bool reflection_point(AcousticPath *path, const Box *box, const double source[3], const double listener[3])
{
    int axis = path->face / 2;
    double wall = path->face % 2 ? box->hi[axis] : box->lo[axis];
    double from = source[axis] - wall, to = listener[axis] - wall;
    if (path->object >= 0 && (path->face % 2 ? from <= 0 || to <= 0 : from >= 0 || to >= 0))
        return false;
    double fraction = to / (from + to);
    for (int k = 0; k < 3; k++) {
        path->bounce[k] = k == axis ? wall : listener[k] + fraction * (source[k] - listener[k]);
        if (path->object >= 0 && (path->bounce[k] < box->lo[k] || path->bounce[k] > box->hi[k]))
            return false;
    }
    return true;
}

static bool segment_interval(const Box *box, const double a[3], const double b[3], double *enter, double *exit)
{
    *enter = -INFINITY;
    *exit = INFINITY;
    for (int k = 0; k < 3; k++) {
        double direction = b[k] - a[k];
        if (direction == 0) {
            if (a[k] <= box->lo[k] || a[k] >= box->hi[k]) return false;
            continue;
        }
        double near = (box->lo[k] - a[k]) / direction, far = (box->hi[k] - a[k]) / direction;
        *enter = fmax(*enter, fmin(near, far));
        *exit = fmin(*exit, fmax(near, far));
    }
    return fmax(0, *enter) < fmin(1, *exit);
}

typedef struct { double fraction, rate_change, surface_loss; int count_change; } AcousticCrossing;

static int crossing_order(const void *left, const void *right)
{
    const AcousticCrossing *a = left, *b = right;
    return (a->fraction > b->fraction) - (a->fraction < b->fraction);
}

static bool attenuate_path(const AcousticObject *objects, int count, int reflector,
                        const double a[3], const double b[3], double gas_loss,
                        const Box *room, const FlowField *field, double field_loss,
                        double *budget, int *transmissions)
{
    AcousticCrossing crossings[2 * ACOUSTIC_BLOCKERS + 1];
    int events = 0;
    for (int i = 0; i < count; i++) {
        const AcousticObject *obj = &objects[i];
        double enter, exit;
        if (i == reflector || !obj->has_material || !segment_interval(&obj->box, a, b, &enter, &exit))
            continue;
        crossings[events++] = (AcousticCrossing){fmax(0, enter), obj->loss_per_m,
                                                enter >= 0 ? obj->surface_loss : 0, 1};
        crossings[events++] = (AcousticCrossing){fmin(1, exit), -obj->loss_per_m,
                                                exit <= 1 ? obj->surface_loss : 0, -1};
        ++*transmissions;
    }
    crossings[events++] = (AcousticCrossing){.fraction = 1};
    qsort(crossings, (size_t)events, sizeof crossings[0], crossing_order);
    double previous = 0, rate = 0, length = separation(a, b);
    int active = 0;
    for (int i = 0; i < events; ++i) {
        const AcousticCrossing *event = &crossings[i];
        *budget -= (event->fraction - previous) * length * (active ? fmax(0, rate) : gas_loss);
        if (!active && field_loss)
            *budget -= field_loss * flow_column(field, a, b, room->lo, previous, event->fraction);
        *budget -= event->surface_loss;
        if (*budget <= 0) return false;
        previous = event->fraction;
        rate += event->rate_change;
        active += event->count_change;
        if (!active) rate = 0;
    }
    return true;
}
static const char *opaque_visibility(const AcousticObject *blockers, int obstacles, const AcousticPath *paths,
                                     int path_count, const Box *room, const double source[3], const double listener[3],
                                     Backend selected, bool blocked[PHYSICS_ACOUSTIC_PATHS], bool *tested)
{
#ifndef NEUTRINO_VULKAN_RT
    (void)room;
#endif
    enum { MAX_SEGMENTS = 1 + 2 * (PHYSICS_ACOUSTIC_PATHS - 1), MAX_TESTS = MAX_SEGMENTS * ACOUSTIC_BLOCKERS };
    size_t total = 0;
    const char *engine = nullptr;
    if (selected == BACKEND_RT) {
        int opaque = 0;
#ifdef NEUTRINO_VULKAN_RT
        RtBox boxes[ACOUSTIC_BLOCKERS];
#endif
        for (int i = 0; i < obstacles; ++i) {
            if (blockers[i].has_material) continue;
#ifdef NEUTRINO_VULKAN_RT
            boxes[opaque] = rt_box(&blockers[i].box, room->lo);
#endif
            ++opaque;
        }
        if (opaque) total = (size_t)(2 * path_count - 1);
#ifdef NEUTRINO_VULKAN_RT
        if (opaque) {
            RtRay rays[MAX_SEGMENTS];
            RtHit hits[MAX_SEGMENTS];
            for (int path = 0; path < path_count; ++path) {
                const double *end = path ? paths[path].bounce : listener;
                double outgoing[3], incoming[3];
                for (int axis = 0; axis < 3; ++axis) {
                    outgoing[axis] = end[axis] - source[axis];
                    incoming[axis] = listener[axis] - end[axis];
                }
                if (!path) rays[0] = rt_ray(source, outgoing, room->lo);
                else {
                    rays[2 * path - 1] = rt_ray(source, outgoing, room->lo);
                    rays[2 * path] = rt_ray(end, incoming, room->lo);
                }
            }
            if (ray_vulkan_rt(rays, total, boxes, (size_t)opaque, hits)) {
                engine = hardware_rt;
                for (size_t i = 0; i < total; ++i)
                    if (hits[i].distance >= 0 && hits[i].distance < 1.0f) blocked[(i + 1) / 2] = true;
            }
        }
#endif
    } else {
        float rows[MAX_TESTS * RAY_VALUES], hits[MAX_TESTS];
        int path_of[MAX_TESTS];
        for (int path = 0; path < path_count; ++path) {
            for (int b = 0; b < obstacles; ++b) {
                if (blockers[b].has_material) continue;
                const double *end = path ? paths[path].bounce : listener;
                pack_segment(rows + total * RAY_VALUES, source, end, &blockers[b].box);
                path_of[total++] = path;
                if (path) {
                    pack_segment(rows + total * RAY_VALUES, end, listener, &blockers[b].box);
                    path_of[total++] = path;
                }
            }
        }
        if (total) engine = trace_batch(selected, rows, total, hits);
        if (engine)
            for (size_t j = 0; j < total; ++j)
                if (hits[j] >= 0 && hits[j] < 1.0f) blocked[path_of[j]] = true;
    }
    *tested = total != 0;
    return engine;
}

static bool acoustic(const Json *args, const Json *settings, const FlowField *field, Text *out, PhysicsReading *reading)
{
    Box room;
    AcousticObject blockers[ACOUSTIC_BLOCKERS] = {0};
    const Json *boxes = at(args, "blockers"), *item;
    const Json *medium = at(args, "medium");
    int obstacles = cJSON_GetArraySize(boxes);
    double source[3], listener[3], gas_loss = 0, floor_db = -120;
    double speed = sqrt(1.4 * 287.05 * setting(settings, AMBIENT_KELVIN));
    const double field_loss = field ? field->config.sound_loss : 0;
    bool material_mode = medium || at(args, "min_level_db") || field_loss;
    Backend selected = backend(args);
    if (selected == BACKEND_INVALID || obstacles > ACOUSTIC_BLOCKERS ||
        (boxes && !cJSON_IsArray(boxes)) || !read_box(args, &room) ||
        !physics_vector(args, "source", source) || !physics_vector(args, "listener", listener) ||
        !strictly_inside(&room, source) || !strictly_inside(&room, listener) ||
        separation(source, listener) < 1e-6) return false;
    if (medium && (!cJSON_IsObject(medium) || !physics_scalar(medium, "loss_db_per_m", 0, 1e6, &gas_loss) ||
                  (at(medium, "speed_m_s") && !physics_scalar(medium, "speed_m_s", 1, 1e5, &speed)))) return false;
    if (at(args, "min_level_db") && !physics_scalar(args, "min_level_db", -300, 120, &floor_db)) return false;
    int i = 0;
    cJSON_ArrayForEach(item, boxes) {
        AcousticObject *obj = &blockers[i++];
        if (!read_box(item, &obj->box)) return false;
        const Json *material = at(item, "material");
        if (!material) continue;
        if (!cJSON_IsObject(material) || !physics_scalar(material, "loss_db_per_m", 0, 1e6, &obj->loss_per_m) ||
            !physics_scalar(material, "reflectivity", 0, 1, &obj->reflection)) return false;
        for (int k = 0; k < 3; k++) if (obj->box.lo[k] == obj->box.hi[k]) return false;
        obj->surface_loss = obj->reflection == 1 ? INFINITY : -10 * log1p(-obj->reflection * obj->reflection) / log(10);
        obj->has_material = material_mode = true;
    }
    const double reflection = setting(settings, WALL_REFLECTIVITY);
    AcousticPath paths[PHYSICS_ACOUSTIC_PATHS] = {{.reflection = 1, .object = -1}};
    int path_count = 1;
    for (int obj = -1; obj < obstacles; ++obj) {
        double ratio = obj < 0 ? reflection : blockers[obj].reflection;
        if (ratio == 0) continue;
        const Box *box = obj < 0 ? &room : &blockers[obj].box;
        for (int face = 0; face < 6; ++face) {
            AcousticPath path = {.reflection = ratio, .object = obj, .face = face};
            if (reflection_point(&path, box, source, listener)) paths[path_count++] = path;
        }
    }
    bool blocked[PHYSICS_ACOUSTIC_PATHS] = {0}, tested;
    const char *engine = opaque_visibility(blockers, obstacles, paths, path_count, &room, source, listener,
                                           selected, blocked, &tested);
    if (tested && !engine) {
        if (reading) reading->unavailable = true;
        append(out, backend_unavailable(selected));
        return true;
    }
    const char *const faces[] = {"X-min", "X-max", "Y-min", "Y-max", "Z-min", "Z-max"};
    format(out, "Acoustic paths (%s visibility, %.4g m/s). Levels relative to the direct source at 1 m.",
           tested ? engine : material_mode ? "CPU material" : "no blocker tests", speed);
    if (field_loss) format(out, " Gas attenuation: %s.", field->method);
    if (field && field->fallback) format(out, " Fallback: %s.", field->fallback);
    if (material_mode)
        format(out, " Audibility is the margin above the modeled cutoff of %+.4g dB.", floor_db);
    if (reading) {
        reading->kind = READING_ACOUSTIC;
        reading->material_acoustics = material_mode;
    }
    for (int path = 0; path < path_count; ++path) {
        const AcousticPath *candidate = &paths[path];
        const double *end = path ? candidate->bounce : listener;
        double distance = separation(source, end) + (path ? separation(end, listener) : 0);
        const double delay = 1000 * distance / speed;
        double level = 20 * (log10(candidate->reflection) - log10(distance));
        double budget = level - floor_db;
        bool faded = false;
        int transmissions = 0;
        if (material_mode && !blocked[path]) {
            faded = budget <= 0 || !attenuate_path(blockers, obstacles, candidate->object, source, end,
                                              gas_loss, &room, field, field_loss, &budget, &transmissions) ||
                    (path && !attenuate_path(blockers, obstacles, candidate->object, end, listener,
                                         gas_loss, &room, field, field_loss, &budget, &transmissions));
            level = faded ? floor_db : floor_db + budget;
        }
        if (reading) {
            reading->arrivals++;
            reading->arrival[path].blocked = blocked[path] || faded;
            reading->arrival[path].faded = faded;
            reading->arrival[path].delay_ms = delay;
            reading->arrival[path].level_db = level;
        }
        if (candidate->object >= 0) format(out, "\nObject %d %s echo", candidate->object + 1, faces[candidate->face]);
        else if (path) format(out, "\n%s echo", faces[candidate->face]);
        else append(out, "\nDirect");
        if (blocked[path]) append(out, " blocked.");
        else if (faded) append(out, " stopped (audibility 0).");
        else {
            format(out, " %.4g ms · %+.3g dB", delay, level);
            if (material_mode) format(out, " · audibility %.4g dB · %d material traversals", budget, transmissions);
        }
    }
    return true;
}
static bool light(const Json *args, const Json *scene, const Box *room, const FlowField *field, double air_loss,
                  Text *out, PhysicsReading *reading)
{
    const Backend selected = backend(args);
    double source[3], listener[3], wall;
    if (selected == BACKEND_INVALID || !physics_vector(args,"source",source) ||
        !physics_vector(args,"listener",listener) || !strictly_inside(room,source) ||
        !strictly_inside(room,listener) || separation(source,listener) < 1e-6 ||
        !physics_optional(args,"wall_reflectivity",0,1,0,&wall)) return false;
    AcousticObject optics[ACOUSTIC_BLOCKERS] = {0};
    const char *names[ACOUSTIC_BLOCKERS];
    const char *const faces[] = {"X-min","X-max","Y-min","Y-max","Z-min","Z-max"};
    int objects = 0;
    Json *item;
    cJSON_ArrayForEach(item, at(scene,"objects")) {
        if (objects == ACOUSTIC_BLOCKERS) return false;
        AcousticObject *obj = &optics[objects];
        const Json *properties = at(item,"optical");
        double transmission, reflectivity;
        if (!read_box(item,&obj->box) || !physics_optional(properties,"transmission",0,1,0,&transmission) ||
            !physics_optional(properties,"reflectivity",0,1,0,&reflectivity) ||
            !physics_optional(properties,"loss_db_per_m",0,1e6,0,&obj->loss_per_m)) return false;
        obj->has_material = transmission > 0;
        obj->surface_loss = transmission > 0 ? -5*log10(transmission) : 0;
        obj->reflection = sqrt(reflectivity);
        names[objects++] = js(item,"name");
    }
    AcousticPath paths[PHYSICS_ACOUSTIC_PATHS] = {{.reflection=1,.object=-1}};
    int count = 1, room_paths = 1;
    for (int object = -1; object < objects; ++object) {
        const Box *box = object < 0 ? room : &optics[object].box;
        double reflectivity = object < 0 ? sqrt(wall) : optics[object].reflection;
        if (reflectivity == 0) continue;
        for (int face = 0; face < 6; ++face) {
            AcousticPath path = {.reflection=reflectivity,.object=object,.face=face};
            if (reflection_point(&path,box,source,listener)) paths[count++] = path;
        }
        if (object < 0) room_paths = count;
    }
    bool blocked[PHYSICS_ACOUSTIC_PATHS] = {0}, tested;
    const char *engine = opaque_visibility(optics,objects,paths,room_paths,room,source,listener,
                                           selected,blocked,&tested);
    if (tested && !engine) {
        if (reading) reading->unavailable = true;
        append(out,backend_unavailable(selected));
        return true;
    }
    format(out,"3D geometric light (%s direct/room visibility", tested ? engine : "no opaque blockers");
    if (reading) reading->kind = READING_OPTICAL;
    if (count > room_paths) append(out,", CPU object reflections");
    append(out,"). Relative power at 1 m.");
    const double field_loss = field ? field->config.light_loss : 0;
    if (field_loss) format(out, " Gas extinction: %s.", field->method);
    if (field && field->fallback) format(out, " Fallback: %s.", field->fallback);
    for (int i = 0; i < count; ++i) {
        const AcousticPath *path = &paths[i];
        const double *end = i ? path->bounce : listener;
        if (path->object >= 0) {
            format(out,"\n%s %s reflection",names[path->object],faces[path->face]);
            for (int j = 0; j < objects; ++j) {
                if (j == path->object || optics[j].has_material) continue;
                double enter, exit;
                if (segment_interval(&optics[j].box,source,end,&enter,&exit) ||
                    segment_interval(&optics[j].box,end,listener,&enter,&exit)) {
                    blocked[i] = true;
                    break;
                }
            }
        } else if (i) format(out,"\n%s reflection",faces[path->face]);
        else append(out,"\nDirect");
        if (blocked[i]) {
            append(out," blocked.");
            continue;
        }
        const double path_length = separation(source,end) + (i ? separation(end,listener) : 0);
        const double floor_db = -300;
        double budget = 20*log10(path->reflection/path_length) - floor_db;
        int crossings = 0;
        if (budget <= 0 || !attenuate_path(optics,objects,path->object,source,end,air_loss,room,field,field_loss,&budget,&crossings) ||
            (i && !attenuate_path(optics,objects,path->object,end,listener,air_loss,room,field,field_loss,&budget,&crossings))) {
            append(out," below cutoff.");
            continue;
        }
        const double level = floor_db + budget;
        double power = pow(10, level / 10), delay = path_length / 0.299792458;
        if (reading) {
            if (!reading->optical_power || delay < reading->optical_first_ns) reading->optical_first_ns = delay;
            reading->optical_power = fmax(reading->optical_power, power);
        }
        format(out," %.7g m · %.7g ns · %+.7g dB · relative power %.7g.",
               path_length,delay,level,power);
    }
    return true;
}

static bool endpoint(const Json *args, const Json *objects, const char *id_key, const char *point_key,
                     double center[3])
{
    if (!at(args, id_key)) return physics_vector(args, point_key, center);
    Box box;
    if (at(args, point_key) || !scene_name(js(args, id_key)) ||
        !read_box(scene_object(objects, js(args, id_key)), &box)) return false;
    for (int axis = 0; axis < 3; ++axis) center[axis] = (box.lo[axis] + box.hi[axis]) / 2;
    return true;
}

static bool scene_query(const char *tool, const Json *args, const Json *settings, Text *out, PhysicsReading *reading)
{
    if (!strcmp(tool,"scene_catalog")) {
        double offset;
        if (!physics_optional(args,"offset",0,INT_MAX,0,&offset) || floor(offset) != offset) return false;
        const Json *rooms = at(settings,"rooms3d"), *entry;
        format(out,"Active 3D layout: %s. %d named layouts.",*js(settings,"active3d") ? js(settings,"active3d") :
               at(settings,"room3d") ? "default" : "none",cJSON_GetArraySize(rooms));
        int index = 0;
        cJSON_ArrayForEach(entry,rooms) {
            if (index++ < offset) continue;
            format(out,"\n%s",js(entry,"name"));
            if (index-offset >= 64) {
                if (index < cJSON_GetArraySize(rooms)) format(out,"\nNext offset: %d.",index);
                break;
            }
        }
        return true;
    }
    const Json *room = active_room(settings);
    Box bounds;
    if (!read_box(room, &bounds)) {
        if (strcmp(tool, "scene_inspect")) return false;
        append(out, "No 3D room has been defined.");
        return true;
    }
    if (!strcmp(tool, "scene_inspect")) {
        double offset;
        if (!physics_optional(args,"offset",0,INT_MAX,0,&offset) || floor(offset) != offset) return false;
        const Json *objects = at(room,"objects"), *item;
        Json *page = cJSON_CreateObject(), *entries = cJSON_CreateArray();
        if (*js(room,"name")) set(page,"name",js(room,"name"));
        put(page,"min",cJSON_Duplicate(at(room,"min"),true));
        put(page,"max",cJSON_Duplicate(at(room,"max"),true));
        if (at(room,"medium")) put(page,"medium",cJSON_Duplicate(at(room,"medium"),true));
        if (*js(room, "air")) set(page, "air", js(room, "air"));
        put(page,"objects",entries);
        int index = 0, shown = 0;
        cJSON_ArrayForEach(item,objects) {
            if (index++ < offset) continue;
            cJSON_AddItemToArray(entries,cJSON_Duplicate(item,true));
            if (++shown == ACOUSTIC_BLOCKERS) break;
        }
        const int total = cJSON_GetArraySize(objects);
        integer(page,"object_count",total);
        if (index < total) integer(page,"next_offset",index);
        char *json = cJSON_PrintUnformatted(page);
        cJSON_Delete(page);
        if (!json) abort();
        format(out, "3D room in metres (Z is up): %s", json);
        cJSON_free(json);
        return true;
    }
    if (!strcmp(tool, "scene_rays")) return scene_rays(args, room, &bounds, out, reading);
    if (!strcmp(tool, "scene_flow")) {
        FlowConfig config;
        double point[3];
        if (!at(room, "medium") || !room_flow(at(room, "medium"), &bounds, &config) ||
            !physics_vector(args, "probe", point) || !strictly_inside(&bounds, point)) return false;
        FlowField field = room_flow_solve(room, &bounds, &config);
        if (!field.grid && cJSON_IsTrue(at(at(room, "medium"), "solid_geometry"))) {
            if (reading) reading->unavailable = true;
            format(out, "Geometry-aware CFD unavailable. %s.", field.fallback ? field.fallback : "No grid");
        } else {
            for (int axis = 0; axis < 3; ++axis) point[axis] -= bounds.lo[axis];
            double concentration = flow_sample(&field, point);
            if (reading) {
                reading->kind = READING_FLOW;
                reading->tracer_concentration = concentration;
                reading->voxel_flow = field.solid != nullptr;
            }
            format(out, "%s. Probe concentration %.9g. RMS divergence %.9g s^-1. Mean speed %.9g m/s.",
                   field.method, concentration, field.divergence, field.speed);
            if (field.fallback) format(out, " Fallback: %s.", field.fallback);
        }
        flow_release(&field);
        return true;
    }
    const bool optical = !strcmp(tool,"scene_light");
    if (at(args, "min") || at(args, "max") || at(args, "blockers")) return false;
    const Json *include = at(args,"include"), *objects = at(room,"objects"), *entry;
    const char *source_id = js(args, "source_entity"), *receiver_id = js(args, "receiver_entity");
    double source[3], listener[3];
    if (!endpoint(args, objects, "source_entity", "source", source) ||
        !endpoint(args, objects, "receiver_entity", "listener", listener)) return false;
    if (include && (!cJSON_IsArray(include) || cJSON_GetArraySize(include) > ACOUSTIC_BLOCKERS)) return false;
    if (!include && cJSON_GetArraySize(objects) > ACOUSTIC_BLOCKERS) {
        if (reading) reading->unavailable = true;
        append(out,"This query needs an include list of up to 16 object names. Inspect the scene to select a region; the full room is still saved.");
        return true;
    }
    Json *filtered = nullptr;
    if (include) {
        Json *selection = cJSON_CreateArray();
        cJSON_ArrayForEach(entry,include) {
            if (!cJSON_IsString(entry)) { cJSON_Delete(selection); return false; }
            Json *match = scene_object(objects,entry->valuestring);
            if (!match || scene_object(selection,entry->valuestring)) {
                cJSON_Delete(selection);
                return false;
            }
            cJSON_AddItemToArray(selection,cJSON_Duplicate(match,true));
        }
        filtered = cJSON_CreateObject();
        put(filtered,"objects",selection);
        objects = selection;
        append(out,"Selected-object measurement. Unselected geometry is outside this calculation. ");
    }
    const Json *materials = at(settings, "materials3d");
    if ((materials || *source_id || *receiver_id) && !filtered) {
        filtered = cJSON_CreateObject();
        put(filtered, "objects", cJSON_Duplicate(objects, true));
        objects = at(filtered, "objects");
    }
    if (filtered) {
        Json *selection = at(filtered, "objects");
        for (Json *object = selection ? selection->child : nullptr, *next; object; object = next) {
            next = object->next;
            const char *name = js(object, "name");
            if ((*source_id && !strcmp(name, source_id)) || (*receiver_id && !strcmp(name, receiver_id))) {
                cJSON_Delete(cJSON_DetachItemViaPointer(selection, object));
                continue;
            }
            const Json *material = at(materials, js(object, "material_id"));
            if (at(material, "material")) put(object, "material", cJSON_Duplicate(at(material, "material"), true));
            if (at(material, "optical")) put(object, "optical", cJSON_Duplicate(at(material, "optical"), true));
        }
    }
    FlowConfig config;
    FlowField field = {0};
    const FlowField *gas = nullptr;
    if (at(room,"medium") && room_flow(at(room,"medium"),&bounds,&config) &&
        (optical ? config.light_loss : config.sound_loss) > 0) {
        field = room_flow_solve(room, &bounds, &config);
        if (!field.grid && cJSON_IsTrue(at(at(room, "medium"), "solid_geometry"))) {
            if (reading) reading->unavailable = true;
            format(out, "Geometry-aware CFD unavailable. %s.", field.fallback ? field.fallback : "No grid");
            cJSON_Delete(filtered);
            flow_release(&field);
            return true;
        }
        gas = &field;
    }
    const Json *air = at(materials, js(room, "air"));
    Json *query = cJSON_Duplicate(args, true);
    put(query, "source", cJSON_CreateDoubleArray(source, 3));
    put(query, "listener", cJSON_CreateDoubleArray(listener, 3));
    if (optical) {
        double air_loss = 0;
        physics_optional(at(air, "optical"), "loss_db_per_m", 0, 1e6, 0, &air_loss);
        bool valid = light(query, filtered ? filtered : room, &bounds, gas, air_loss, out, reading);
        cJSON_Delete(query);
        cJSON_Delete(filtered);
        flow_release(&field);
        return valid;
    }
    if (!at(query, "medium") && at(air, "material"))
        put(query, "medium", cJSON_Duplicate(at(air, "material"), true));
    put(query, "min", cJSON_Duplicate(at(room, "min"), true));
    put(query, "max", cJSON_Duplicate(at(room, "max"), true));
    put(query, "blockers", cJSON_Duplicate(objects, true));
    bool valid = acoustic(query, settings, gas, out, reading);
    cJSON_Delete(query);
    cJSON_Delete(filtered);
    flow_release(&field);
    return valid;
}

static bool fluid(const Json *args, const Json *settings, Text *out, PhysicsReading *reading)
{
    double height, tank, hole, seconds;
    if (!physics_scalar(args, "height", 0, 1000, &height) || !physics_scalar(args, "tank_area", 1e-6, 1e6, &tank) ||
        !physics_scalar(args, "outlet_area", 0, 1000, &hole) || !physics_scalar(args, "seconds", 0, 86400, &seconds) ||
        hole > tank * 0.1) return false;
    const double rate = setting(settings, OUTLET_COEFFICIENT) * hole * sqrt(2 * setting(settings, GRAVITY));
    const double remaining = fmax(0, sqrt(height) - rate * seconds / (2 * tank));
    const double level = remaining * remaining;
    const double drained = (height - level) * tank * 1000, flow = rate * remaining * 1000;
    format(out, "Tank drain estimate: final level %.5g m, drained %.5g L, final flow %.5g L/s. "
           "Elapsed drainage interval: %.9g seconds.", level, drained, flow, seconds);
    if (reading) *reading = (PhysicsReading){.kind = READING_FLUID, .seconds = seconds,
        .level_m = level, .drained_l = drained, .remaining_l = level * tank * 1000, .flow_lps = flow,
        .initial_flow_lps = rate * sqrt(height) * 1000};
    return true;
}
static bool gas(const Json *args, const Json *settings, Text *out)
{
    double moles, volume, leak, seconds, kelvin = setting(settings, AMBIENT_KELVIN);
    if (!physics_scalar(args, "moles", 0, 1e9, &moles) || !physics_scalar(args, "volume", 0.001, 1e9, &volume) ||
        (at(args, "kelvin") && !physics_scalar(args, "kelvin", 50, 2000, &kelvin)) || !physics_scalar(args, "leak", 0, 1e6, &leak) ||
        !physics_scalar(args, "seconds", 0, 86400, &seconds)) return false;
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
bool physics_run_reading(const char *name, const Json *args, const Json *settings, Text *out, PhysicsReading *reading)
{
    if (reading) *reading = (PhysicsReading){0};
    if (!strcmp(name, "scene_inspect") || !strcmp(name, "scene_rays") || !strcmp(name, "scene_acoustic") ||
        !strcmp(name, "scene_light") || !strcmp(name,"scene_catalog") || !strcmp(name, "scene_flow"))
        return scene_query(name, args, settings, out, reading);
    if (!strcmp(name, "ray")) return ray(args, out);
    if (!strcmp(name, "rays")) return rays(args, out, reading);
    if (!strcmp(name, "acoustic")) return acoustic(args, settings, nullptr, out, reading);
    if ((!strcmp(name,"fluid") || !strcmp(name,"gas")) && at(args,"method") &&
        (at(args,"size") || strcmp(js(args,"method"),"reduced"))) {
        Json *flow = cJSON_Duplicate(args,true);
        set(flow,"medium",!strcmp(name,"gas") ? "gas" : "liquid");
        bool valid = physics_model_run("flow3d",flow,setting(settings,GRAVITY),out,nullptr);
        cJSON_Delete(flow);
        return valid;
    }
    if (!strcmp(name, "fluid")) return fluid(args, settings, out, reading);
    if (!strcmp(name, "gas")) return gas(args, settings, out);
    if (!strcmp(name, "prop_load")) return prop_load(args, out);
    if (physics_model_tool(name))
        return physics_model_run(name, args, setting(settings, GRAVITY), out, reading ? &reading->unavailable : nullptr);
    return false;
}
bool physics_run(const char *name, const Json *args, const Json *settings, Text *out)
{
    return physics_run_reading(name, args, settings, out, nullptr);
}
