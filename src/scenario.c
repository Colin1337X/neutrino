#include "app.h"
#include "physics_models.h"

#include <float.h>
#include <math.h>
#include <string.h>

static bool label(const char *value)
{
    size_t n = strlen(value);
    if (!n || n > 64) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)value[i] < 32 || value[i] == 127) return false;
    return true;
}

static bool query_shape(const Json *query, bool condition)
{
    if (!cJSON_IsObject(query) || !label(js(query, "predicate")) ||
        (!condition && !label(js(query, "id")))) return false;
    const Json *field;
    cJSON_ArrayForEach(field, query) {
        const char *key = field->string;
        if (!key || (strcmp(key, "predicate") && strcmp(key, "step") &&
            strcmp(key, "threshold") && strcmp(key, "entity") && strcmp(key, "target") &&
            (condition ? strcmp(key, "expect") : strcmp(key, "id")))) return false;
        if (at(query, key) != field) return false;
    }
    return !condition || !at(query, "expect") || cJSON_IsBool(at(query, "expect"));
}

bool simulation_plan_parse(const Json *plan, SimulationPlan *parsed)
{
    const Json *field, *queries = at(plan, "queries");
    const Json *actions = at(plan, "actions"), *budget = at(plan, "max_actions");
    const char *mode = js(plan, "mode"), *name = js(plan, "name");
    int count = cJSON_GetArraySize(actions), limit = SCENARIO_ACTION_LIMIT;
    if (budget) {
        if (!cJSON_IsNumber(budget) || !isfinite(budget->valuedouble) || budget->valuedouble < 1 ||
            budget->valuedouble > limit || floor(budget->valuedouble) != budget->valuedouble) return false;
        limit = budget->valueint;
    }
    if ((strcmp(mode, "hypothetical") && strcmp(mode, "stage")) || !label(name) ||
        !cJSON_IsArray(actions) || count < 1 || count > limit ||
        !cJSON_IsArray(queries) || cJSON_GetArraySize(queries) > 16 ||
        (at(plan, "continue") && !cJSON_IsBool(at(plan, "continue")))) return false;
    cJSON_ArrayForEach(field, plan) {
        const char *key = field->string;
        if (!key || (strcmp(key, "neutrino_tool") && strcmp(key, "name") && strcmp(key, "mode") &&
            strcmp(key, "actions") && strcmp(key, "queries") && strcmp(key, "continue") &&
            strcmp(key, "max_actions"))) return false;
        if (at(plan, key) != field) return false;
    }
    const Json *query;
    cJSON_ArrayForEach(query, queries) {
        const char *id = js(query, "id");
        if (!query_shape(query, false)) return false;
        for (const Json *prior = queries->child; prior != query; prior = prior->next)
            if (!strcmp(js(prior, "id"), id)) return false;
    }
    const Json *action;
    cJSON_ArrayForEach(action, actions)
        if (!cJSON_IsObject(action) || !label(js(action, "neutrino_tool")) ||
            (at(action, "when") && !query_shape(at(action, "when"), true))) return false;
    *parsed = (SimulationPlan){.queries = queries, .name = name, .hypothetical = !strcmp(mode, "hypothetical")};
    return true;
}

typedef enum { ENTITY_EXISTS, ENTITY_OVERLAPS, ENTITY_MOVED, ENTITY_CONTACTS, ENTITY_FRACTURED, FLUID_RELEASED,
               FLUID_DRAINED, FLUID_LEVEL, FLOW_TRACER,
               SIGNAL_REACHES, SIGNAL_ARRIVES, SIGNAL_POWER } Predicate;
static const struct { const char *name; Predicate kind; } predicates[] = {
    {"entity.exists", ENTITY_EXISTS}, {"entity.overlaps", ENTITY_OVERLAPS},
    {"entity.moved", ENTITY_MOVED}, {"entity.contacts", ENTITY_CONTACTS},
    {"entity.structural_failure", ENTITY_FRACTURED}, {"fluid.leaves_container", FLUID_RELEASED},
    {"fluid.volume_exceeds", FLUID_DRAINED}, {"fluid.level_exceeds", FLUID_LEVEL},
    {"fluid.concentration_exceeds", FLOW_TRACER},
    {"signal.reaches_receiver", SIGNAL_REACHES}, {"signal.arrival_before", SIGNAL_ARRIVES},
    {"signal.power_exceeds", SIGNAL_POWER}
};

static void field_evidence(Json *evidence, const Json *body, const char *key)
{
    const Json *value = at(body, key);
    if (cJSON_IsNumber(value)) put(evidence, key, cJSON_Duplicate(value, false));
}

static const char *rigid_result(Predicate kind, const Json *query, const Json *settings, Json *evidence)
{
    const char *entity = js(query, "entity"), *target = js(query, "target");
    if (!label(entity)) return "INSUFFICIENT_DATA";
    const Json *body = physics_scene_body(settings, entity);
    if (!body || !at(body, "translation_m")) return "INSUFFICIENT_DATA";
    set(evidence, "solver", "swept translational AABB rigid bodies");
    if (kind == FLUID_RELEASED) set(evidence, "approximation", "released volume only; flow pattern unmodeled");
    else set(evidence, "approximation", "translational dynamics; rotation unmodeled");
    if (kind == ENTITY_CONTACTS) {
        if (!label(target) || (strcmp(target, "@room") && physics_scene_relation(settings, target, nullptr) != 1))
            return "INSUFFICIENT_DATA";
        const Json *item;
        cJSON_ArrayForEach(item, at(body, "contacts"))
            if (!strcmp(js(item, "target"), target)) {
                field_evidence(evidence, item, "peak_impact_j");
                field_evidence(evidence, item, "peak_impact_time_s");
                return "TRUE";
            }
        return "FALSE";
    }
    if (kind == ENTITY_FRACTURED) {
        field_evidence(evidence, body, "fracture_energy_j");
        field_evidence(evidence, body, "fracture_time_s");
        return cJSON_IsTrue(at(body, "fractured")) ? "TRUE" : "FALSE";
    }
    const char *key = kind == FLUID_RELEASED ? "released_l" : "translation_m";
    const Json *value = at(body, key);
    if (!cJSON_IsNumber(value)) return "INSUFFICIENT_DATA";
    put(evidence, "value", cJSON_Duplicate(value, false));
    set(evidence, "units", kind == FLUID_RELEASED ? "L" : "m");
    return value->valuedouble > 1e-9 ? "TRUE" : "FALSE";
}

static const char *evaluate(const Json *query, const Json *settings, const PhysicsReading readings[],
                            int steps, Json *evidence)
{
    const char *name = js(query, "predicate");
    size_t i = 0;
    while (i < sizeof predicates / sizeof *predicates && strcmp(name, predicates[i].name)) ++i;
    if (i == sizeof predicates / sizeof *predicates) return "UNSUPPORTED";
    Predicate kind = predicates[i].kind;
    if (kind == ENTITY_MOVED || kind == ENTITY_CONTACTS || kind == ENTITY_FRACTURED || kind == FLUID_RELEASED)
        return rigid_result(kind, query, settings, evidence);
    if (kind == ENTITY_EXISTS || kind == ENTITY_OVERLAPS) {
        const char *entity = js(query, "entity"), *target = js(query, "target");
        if (!label(entity) || (kind == ENTITY_OVERLAPS && !label(target))) return "INSUFFICIENT_DATA";
        int status = physics_scene_relation(settings, entity, kind == ENTITY_OVERLAPS ? target : nullptr);
        set(evidence, "solver", "AABB geometry");
        if (status < 0) return "INSUFFICIENT_DATA";
        return status ? "TRUE" : "FALSE";
    }
    double step;
    if (!physics_scalar(query, "step", 1, steps, &step) || floor(step) != step) return "INSUFFICIENT_DATA";
    const PhysicsReading *reading = &readings[(int)step - 1];
    integer(evidence, "step", (int)step);
    if (reading->unavailable) return "SIMULATION_FAILED";
    double value, threshold;
    const char *unit;
    if (kind == SIGNAL_POWER) {
        if (reading->kind != READING_OPTICAL) return "INSUFFICIENT_DATA";
        value = reading->optical_power;
        unit = "relative power";
        set(evidence, "solver", "geometric optics");
    } else if (kind == FLOW_TRACER) {
        if (reading->kind != READING_FLOW) return "INSUFFICIENT_DATA";
        value = reading->tracer_concentration;
        unit = "relative tracer";
        set(evidence, "solver", reading->voxel_flow ? "voxel-obstacle CFD" : "room medium");
    } else if (kind == FLUID_DRAINED || kind == FLUID_LEVEL) {
        if (reading->kind != READING_FLUID) return "INSUFFICIENT_DATA";
        value = kind == FLUID_DRAINED ? reading->drained_l : reading->level_m;
        unit = kind == FLUID_DRAINED ? "L" : "m";
        set(evidence, "solver", "analytical tank drain");
    } else {
        if (reading->kind != READING_ACOUSTIC && reading->kind != READING_OPTICAL) return "INSUFFICIENT_DATA";
        value = DBL_MAX;
        if (reading->kind == READING_ACOUSTIC) {
            set(evidence, "solver", "geometric acoustics");
            for (int j = 0; j < reading->arrivals; ++j)
                if (!reading->arrival[j].blocked && reading->arrival[j].delay_ms < value)
                    value = reading->arrival[j].delay_ms;
        } else {
            set(evidence, "solver", "geometric optics");
            if (reading->optical_power > 0) value = reading->optical_first_ns / 1e6;
        }
        if (kind == SIGNAL_ARRIVES && !physics_scalar(query, "threshold", 0, DBL_MAX, &threshold))
            return "INSUFFICIENT_DATA";
        if (value == DBL_MAX) return "FALSE";
        put(evidence, "value", cJSON_CreateNumber(value));
        set(evidence, "units", "ms");
        if (kind == SIGNAL_REACHES) return "TRUE";
        put(evidence, "threshold", cJSON_CreateNumber(threshold));
        return value < threshold ? "TRUE" : "FALSE";
    }
    if (!physics_scalar(query, "threshold", 0, DBL_MAX, &threshold)) return "INSUFFICIENT_DATA";
    put(evidence, "value", cJSON_CreateNumber(value));
    put(evidence, "threshold", cJSON_CreateNumber(threshold));
    set(evidence, "units", unit);
    return value > threshold ? "TRUE" : "FALSE";
}

int simulation_plan_condition(const Json *query, const Json *settings,
                              const PhysicsReading readings[], int preceding_steps)
{
    Json *evidence = cJSON_CreateObject();
    const char *status = evaluate(query, settings, readings, preceding_steps, evidence);
    cJSON_Delete(evidence);
    bool expected = !cJSON_IsFalse(at(query, "expect"));
    if (!strcmp(status, "TRUE")) return expected;
    if (!strcmp(status, "FALSE")) return !expected;
    return -1;
}

void simulation_plan_report(const SimulationPlan *plan, const Json *settings,
                            const PhysicsReading readings[], int steps, bool complete,
                            bool pending_commit, Text *out)
{
    Json *report = cJSON_CreateObject(), *results = cJSON_CreateArray();
    set(report, "simulation", plan->name);
    set(report, "mode", plan->hypothetical ? "hypothetical" : "stage");
    set(report, "status", complete ? "completed" : "failed");
    put(report, "pending_commit", cJSON_CreateBool(pending_commit));
    integer(report, "steps", steps);
    put(report, "consequences", results);
    const Json *query;
    cJSON_ArrayForEach(query, plan->queries) {
        Json *result = cJSON_CreateObject();
        set(result, "id", js(query, "id"));
        set(result, "predicate", js(query, "predicate"));
        set(result, "result", complete ? evaluate(query, settings, readings, steps, result) : "SIMULATION_FAILED");
        cJSON_AddItemToArray(results, result);
    }
    char *json = cJSON_PrintUnformatted(report);
    if (json) format(out, "%s\n", json);
    cJSON_free(json);
    cJSON_Delete(report);
}
