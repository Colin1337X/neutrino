#include "app.h"
#include "physics_models.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SIM_COMMAND_LIMIT = 512 * 1024 };
static const double largest_exact_minute = 9007199254740991.0;
static const char *const meters[] = {"hunger", "thirst", "bladder", "bowels", "mood"};
static const double hourly[] = {4, 7, 9, 3};

static bool text_field(const char *s, size_t limit)
{
    if (!*s || strlen(s) > limit) return false;
    for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return false;
    return true;
}
static bool bounded_int(const Json *op, const char *key, int lo, int hi, int *value)
{
    const Json *n = at(op, key);
    if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble) || n->valuedouble < lo ||
        n->valuedouble > hi || floor(n->valuedouble) != n->valuedouble) return false;
    *value = (int)n->valuedouble;
    return true;
}
static Json *find(const Json *array, const char *actor, const char *part, const char *kind)
{
    Json *item;
    cJSON_ArrayForEach(item, array)
        if (!strcmp(js(item, "actor"), actor) && !strcmp(js(item, "part"), part) &&
            !strcmp(js(item, "kind"), kind)) return item;
    return nullptr;
}
static Json *row(Json *array, const char *actor, const char *part, const char *kind)
{
    Json *entry = find(array, actor, part, kind);
    if (entry) return entry;
    entry = cJSON_CreateObject();
    set(entry, "actor", actor);
    if (*part) set(entry, "part", part);
    if (*kind) set(entry, "kind", kind);
    cJSON_AddItemToArray(array, entry);
    return entry;
}
static bool severity(const char *s)
{
    return !strcmp(s, "mild") || !strcmp(s, "moderate") || !strcmp(s, "severe") || !strcmp(s, "critical");
}
static double clock_value(const Json *state, const char *field)
{
    const Json *value = at(state,field);
    return cJSON_IsNumber(value) ? value->valuedouble : 0;
}
static bool apply_stat(Json *state, const Json *op)
{
    const char *tool = js(op, "neutrino_tool"), *actor = js(op, "actor");
    const char *metric = js(op, "metric"), *part = js(op, "part"), *kind = js(op, "kind");
    int value;
    if (!strcmp(tool, "tune")) return physics_tune(at(state, "physics"), op);
    if (!strcmp(tool, "scene_room") || !strcmp(tool, "scene_place") || !strcmp(tool, "scene_remove") ||
        !strcmp(tool, "scene_medium") || !strcmp(tool,"scene_select") || !strcmp(tool, "scene_material") ||
        !strcmp(tool, "scene_air") || !strcmp(tool, "scene_translate") || rigid_tool(tool))
        return physics_scene_edit(at(state, "physics"), op);
    if (!strcmp(tool, "set_time") || !strcmp(tool, "auto_time") || !strcmp(tool, "advance")) {
        const bool step = !strcmp(tool, "auto_time"), advancing = !strcmp(tool, "advance");
        double minutes;
        if (!physics_scalar(op,"minutes",0,largest_exact_minute,&minutes) || floor(minutes) != minutes) return false;
        if (advancing) {
            const double clock = clock_value(state,"clock");
            if (clock > largest_exact_minute-minutes) return false;
            put(state,"clock",cJSON_CreateNumber(clock+minutes));
            Json *person;
            cJSON_ArrayForEach(person, at(state, "cast")) {
                for (size_t i = 0; i < sizeof hourly / sizeof *hourly; i++) {
                    const Json *current = at(person, meters[i]);
                    double current_value = cJSON_IsNumber(current) ? current->valuedouble : 0;
                    put(person, meters[i], cJSON_CreateNumber(fmin(100, current_value + minutes * hourly[i] / 60)));
                }
            }
        } else put(state,step ? "step" : "clock",cJSON_CreateNumber(minutes));
        return true;
    }
    if (!text_field(actor, 70)) return false;
    if (!strcmp(tool, "stat")) {
        size_t i = 0;
        while (i < sizeof meters / sizeof *meters && strcmp(metric, meters[i])) i++;
        bool location = !strcmp(metric, "location");
        if (location ? !text_field(js(op, "location"), 100) :
            i == sizeof meters / sizeof *meters || !bounded_int(op, "value", 0, 100, &value)) return false;
        Json *person = row(at(state, "cast"), actor, "", "");
        if (!person) return false;
        if (location) set(person, metric, js(op, "location"));
        else integer(person, metric, value);
        return true;
    }
    if (!strcmp(tool, "bond")) {
        const char *other = js(op, "other");
        if (!text_field(other, 70) || !strcmp(actor, other) || !bounded_int(op, "value", -100, 100, &value)) return false;
        Json *bond = row(at(state, "bonds"), actor, other, "");
        if (!bond) return false;
        integer(bond, "value", value);
        return true;
    }
    if (!text_field(part, 70) || !text_field(kind, 70)) return false;
    Json *injuries = at(state, "injuries");
    if (!strcmp(tool, "injury")) {
        const char *grade = js(op, "severity");
        if (!severity(grade)) return false;
        Json *wound = row(injuries, actor, part, kind);
        if (!wound) return false;
        set(wound, "severity", grade);
        set(wound, "status", "untreated");
        return true;
    }
    if (!strcmp(tool, "treat") || !strcmp(tool, "heal")) {
        Json *wound = find(injuries, actor, part, kind);
        if (!wound || !strcmp(js(wound, "status"), "healed")) return false;
        set(wound, "status", !strcmp(tool, "heal") ? "healed" : "treated");
        return true;
    }
    return false;
}

static Json *state_at(const Json *story, int head)
{
    Json *state = cJSON_CreateObject(), *path = story_path(story, head), *item;
    integer(state, "clock", 480);
    integer(state, "step", 5);
    put(state, "cast", cJSON_CreateArray());
    put(state, "bonds", cJSON_CreateArray());
    put(state, "injuries", cJSON_CreateArray());
    put(state, "physics", cJSON_CreateObject());
    int count = cJSON_GetArraySize(at(story, "nodes"));
    bool *visible = calloc((size_t)count + 1, sizeof *visible);
    if (!visible) abort();
    visible[0] = true;
    cJSON_ArrayForEach(item, path) {
        const int id = number(item, "id");
        if (id > 0 && id <= count) visible[id] = true;
    }
    cJSON_ArrayForEach(item, at(story, "simLog")) {
        const int id = number(item, "anchor");
        if (id >= 0 && id <= count && visible[id]) apply_stat(state, item);
    }
    free(visible);
    cJSON_Delete(path);
    return state;
}
static Json *preview_state(const Preview *p)
{
    Json *state = state_at(p->story, p->parent), *pending;
    cJSON_ArrayForEach(pending, p->tool_pending) apply_stat(state, pending);
    return state;
}
static Json *preview_world(const Preview *p)
{
    Json *story = nullptr, *op;
    cJSON_ArrayForEach(op,p->tool_pending) {
        const char *name = js(op,"neutrino_tool");
        if (strcmp(name,"world") && strcmp(name,"inventory") && strcmp(name,"character")) continue;
        if (!story) story = cJSON_Duplicate(p->story,true);
        if (!strcmp(name,"world")) world_edit(story,p->parent,op);
        else if (!strcmp(name,"inventory")) world_inventory_tool(story,p->parent,op);
        else world_character_tool(story,p->parent,op);
    }
    return story;
}
static void log_stat(Json *story, int head, const Json *op)
{
    Json *log = at(story, "simLog");
    if (!cJSON_IsArray(log)) put(story, "simLog", log = cJSON_CreateArray());
    Json *copy = cJSON_Duplicate(op, true);
    integer(copy, "anchor", head);
    cJSON_AddItemToArray(log, copy);
}
static void clock_text(const Json *state, Text *out)
{
    const uint64_t clock = (uint64_t)clock_value(state,"clock"), step = (uint64_t)clock_value(state,"step");
    format(out,"Day %" PRIu64 ", %02u:%02u · ",clock/1440+1,(unsigned)(clock/60%24),(unsigned)(clock%60));
    if (step) format(out,"%" PRIu64 " min per reply\n",step);
    else append(out, "manual clock\n");
}
static void describe(const Json *story, int head, const Json *state, Text *out, bool bars)
{
    Text local = {0};
    world_room(story, head, &local);
    format(out, "Location: %s\n", local.len ? str(&local) : "unnamed scene");
    clock_text(state, out);
    physics_describe(at(state, "physics"), out);
    if (at(at(state,"physics"),"room3d") || at(at(state,"physics"),"rooms3d")) {
        physics_run("scene_inspect", nullptr, at(state, "physics"), out);
        append(out, "\n");
    }
    Json *item;
    cJSON_ArrayForEach(item, at(state, "cast")) {
        const char *actor = js(item, "actor"), *location = js(item, "location");
        Text world = {0};
        if (!*location) location = world_character_room(story, head, actor, &world);
        format(out, "%s", actor);
        if (*location) format(out, " · %s", location);
        for (size_t i = 0; i < sizeof meters / sizeof *meters; i++) {
            const Json *value = at(item, meters[i]);
            if (!cJSON_IsNumber(value)) continue;
            if (bars && i < sizeof hourly / sizeof *hourly) {
                int fill = (int)(value->valuedouble / 10);
                static const char full[] = "##########", empty[] = "----------";
                format(out, "\n  %-8s [%.*s%.*s] %.0f", meters[i], fill, full, 10 - fill, empty, value->valuedouble);
            } else format(out, " · %s %.0f", meters[i], value->valuedouble);
        }
        append(out, "\n");
        release(&world);
        if (out->len > 8500) break;
    }
    cJSON_ArrayForEach(item, at(state, "injuries")) {
        const char *grade = js(item, "severity"), *status = js(item, "status");
        const char *function = !strcmp(status, "healed") ? "recovered" :
            !strcmp(grade, "critical") ? "function lost" :
            !strcmp(grade, "severe") ? "function impaired" :
            !strcmp(grade, "moderate") ? "function restricted" : "minor discomfort";
        format(out, "%s · %s · %s · %s · %s · %s\n", js(item, "actor"), js(item, "part"),
               js(item, "kind"), grade, status, function);
        if (out->len > 10000) break;
    }
    cJSON_ArrayForEach(item, at(state, "bonds")) {
        format(out, "%s → %s · relationship %+d\n", js(item, "actor"), js(item, "part"), number(item, "value"));
        if (out->len > 12000) break;
    }
    release(&local);
}
bool simulation_enabled(const Json *story)
{
    return cJSON_IsTrue(at(story, "simEnabled"));
}
static const char tool_help[] =
    "Tools and fields\n"
    "stat: actor, metric (hunger/thirst/bladder/bowels/mood), value (0–100). For location use metric=location and location text.\n"
    "bond: actor, other, value (-100–100). This is directional.\n"
    "injury: actor, part, kind, severity (mild/moderate/severe/critical).\n"
    "treat or heal: actor, part, kind.\n"
    "set_time: minutes since start of day one. advance: minutes to elapse. auto_time: minutes per reply (0 disables).\n"
    "world_inspect: list the branch's rooms, connections, characters and items; optional offset for more.\n"
    "world: action room (name, description), remove_room (name), connect/disconnect (name, room), "
    "move (name of connected room), relocate (name for a scene transition), weather (description), "
    "npc/item (name, room, description for new entries), "
    "remove_npc/remove_item (name). Existing entities can be moved or edited.\n"
    "character: action create (name, description, optional room) or switch (name, or @default).\n"
    "inventory: action (add/edit/take/drop/wear/remove/give), name, description for add/edit, target for give. "
    "World and inventory actions can include actor (party member name or @default); otherwise use the active character. "
    "World actions say (description) and use (name, optional description) record events.\n";
static const char scenario_help[] =
    "scene_material registers a name with acoustic and/or optical property objects; scene_place can reference it with material_id.\n"
    "scene_air selects a material_id for room-medium attenuation. scene_translate moves a named box by an offset vector. "
    "Scene acoustic and light endpoints can use source_entity and receiver_entity instead of coordinates.\n"
    "For voxel CFD, add solid_geometry:true with method:cfd to scene_medium. scene_flow reads concentration at a probe point.\n"
    "Rigid bodies: scene_body attaches mass_kg (dry mass), optional liquid_l, restitution and velocity to a named object. "
    "scene_constraint takes name and mode fixed/free/rail; rail uses axis x/y/z. scene_step advances the room by seconds. "
    "Use scene_material mechanical:{fracture_j} to set an impact threshold for a brittle object. "
    "scene_impulse takes name, impulse_ns vector, and optional center-relative offset_m. "
    "Query entity.rotated, entity.moved, entity.contacts (with target), entity.structural_failure and fluid.leaves_container by name. "
    "fluid.spill_deposited and fluid.deposits_on (with target) give volume and footprint. scene_spill reads this by entity.\n"
    "sim_plan: name, mode (hypothetical/stage), actions (ordered tool objects), queries (id and predicate). "
    "Hypothetical plans leave the story unchanged. Predicates: entity.exists(entity), entity.overlaps(entity,target), "
    "fluid.volume_exceeds(step,threshold in L), fluid.level_exceeds(step,threshold in m), "
    "fluid.concentration_exceeds(step,relative tracer threshold), "
    "signal.reaches_receiver(step), signal.arrival_before(step,threshold in ms), "
    "signal.power_exceeds(step,relative optical power threshold). Steps are one-based. "
    "An action's optional when condition uses the same predicate fields as a query without id, "
    "and may specify expect:false to run on a false result.\n"
    "Valid minimal example (replace the example values with the scene's own): "
    "{\"neutrino_tool\":\"sim_plan\",\"name\":\"drain\",\"mode\":\"hypothetical\","
    "\"actions\":[{\"neutrino_tool\":\"fluid\",\"height\":1,\"tank_area\":2,\"outlet_area\":0.003,\"seconds\":10}],"
    "\"queries\":[{\"id\":\"spills\",\"predicate\":\"fluid.volume_exceeds\",\"step\":1,\"threshold\":80}]}. "
    "Use these exact field names and numeric values without unit suffixes. Send the tool object alone.\n";

static const char physics_help[] =
    "tune: setting and numeric value. gravity (0.1–30 m/s²), outlet_coefficient (0.1–1), ambient_kelvin (150–400 K), wall_reflectivity (0–1).\n"
    "ray: origin, direction, min, max (three-number arrays in metres, min/max are box corners).\n"
    "rays: min, max, rays (up to 4096 origin/direction objects), backend (auto/cpu/gpu/vulkan/rt). "
    "Use rt only when hardware ray queries are requested and available.\n"
    "scene_room: min/max room corners in metres, Z up, optional name to create or select a named layout. "
    "scene_select: name of saved layout (empty for the default); scene_catalog: optional offset lists layouts.\n"
    "scene_place: name, kind (object/wall), min/max box corners inside the room, optional acoustic material. "
    "Use the same name to move or update an object. An omitted material makes it opaque.\n"
    "scene_remove: name. scene_inspect: optional offset for the next page of geometry.\n"
    "scene_rays: origin inside the room, count (1–4096 evenly distributed directions) or directions (array of vectors), "
    "optional backend. Returns first contacts with named objects or the room boundary.\n"
    "scene_acoustic: source, listener, optional medium, min_level_db and backend. Uses the stored room and objects. "
    "For sound or light queries in a large room, include lists the object names in the calculation.\n"
    "scene_light: source, listener, optional wall_reflectivity and backend; traces direct and first-bounce light. "
    "scene_place accepts optical:{transmission,reflectivity,loss_db_per_m} for light.\n"
    "flow3d: size [x,y,z], medium (liquid/gas), optional seconds, wind, center, sigma_m, concentration, "
    "viscosity, diffusivity, vortex_m_s, resolution, probe, method (reduced/navier_stokes/cfd), backend (cpu/gpu), "
    "and memory (auto/mapped on Windows).\n"
    "scene_medium: save gas flow parameters from flow3d (size follows the room, center is room-relative) "
    "plus sound_loss and light_loss (dB per concentration-metre); off:true removes the field.\n"
    "elastic3d: size, force [x,y,z], young_pa, optional poisson or model (solid/cantilever). "
    "softbody3d: size, mass_kg, stiffness, optional force, damping, seconds, memory (auto/mapped on Windows), "
    "sample_node (zero-based), pin (base/none), initial_velocity and gravity [x,y,z]. "
    "Specify nodes (exact count, at least 8) or resolution (a count per axis "
    "or a vector [x,y,z], at least 2 per axis); available memory and runtime determine feasible sizes.\n"
    "trajectory3d: origin, velocity, seconds, optional wind and linear_drag. "
    "thermal3d: kelvin, ambient_kelvin, heat_capacity_j_k, seconds, optional conductance_w_k, power_w, moles and volume_m3. "
    "For spatial conduction also provide size, probe, variation_k [x,y,z] and thermal_diffusivity.\n"
    "acoustic: min/max room corners, source, listener, optional blockers (min/max boxes) and backend. Reports direct path and reflections.\n"
    "For acoustic material transmission set a blocker's material to {loss_db_per_m, reflectivity (0–1 amplitude)}. "
    "Optional medium has loss_db_per_m and speed_m_s; min_level_db sets the relative cutoff.\n"
    "fluid: height (m), tank_area (m²), outlet_area (m²), seconds.\n"
    "gas: moles, volume (m³), leak (mol/s), seconds, optional kelvin (defaults to scene ambient).\n"
    "prop_load: qualitative scenery load. strength (fragile/ordinary/reinforced), load (light/steady/heavy).\n"
    "ignition3d: source and site [x,y,z]; fictional normalized activation with optional reach, drive, initial, "
    "activate, reset, pulse_ticks, cool_ticks and response.\n";

void simulation_prompt(const Preview *p, Text *system)
{
    if (!simulation_enabled(p->story)) return;
    Json *state = preview_state(p);
    Json *staged = preview_world(p);
    Text context = {0};
    append(system, "\n\nTracked fiction state. Actor locations here are the tracked locations. "
           "Needs from 0 to 100 show urgency, and mood runs from low to high.\n");
    if (staged) {
        Text world = {0};
        append(system,"\nStaged world state for this reply (supersedes the starting world state):\n");
        world_context(staged,p->parent,&world);
        append(system,str(&world));
        release(&world);
    }
    describe(staged ? staged : p->story, p->parent, state, &context, false);
    append(system, str(&context));
    release(&context);
    cJSON_Delete(state);
    cJSON_Delete(staged);
    if (!p->tool_prose_only) {
        format(system, "\nChoose tools yourself when their results would affect what happens next, even if the player "
               "did not ask to run a calculation. For spatial scenes, initialize the 3D room and place established walls "
               "and props, then inspect or measure the room before deciding what characters can perceive or reach. "
               "You control the simulated world: set or change its geometry, time, props, "
               "inventory and character state as consequences of the scene. "
               "You have %d tool operations left before prose. An ordered setup can include more edits in one call. "
               "Make the next response a tool call or ordinary in-character prose. For several measurements, respond with "
               "one compact JSON array starting with [ and ending with ]. Include one complete tool object per requested "
               "measurement, separated by commas. Use the supplied values once. End the reply after the closing bracket. "
               "For a single tool or state edit, return one object. Every tool object has a neutrino_tool field. "
               "For dependent changes and measurements, return one object with an actions array, "
               "for example {\"actions\":[{\"neutrino_tool\":\"tune\",\"setting\":\"gravity\",\"value\":3}]}. "
               "Put prerequisites before dependent actions. The simulator applies actions in array order "
               "and rejects the entire sequence if an action fails. Add \"continue\":true alongside \"actions\" only "
               "if you need to see the results before selecting another tool. Never narrate a dependent result before its action. "
               "Use tools to record changes that actually occur in the fiction. "
               "When no calculation or edit is needed, write story prose.\n", SIM_TOOL_LIMIT - p->tool_rounds);
        append(system, "If the latest player action supplies one acoustic request, select it with "
                       "{\"neutrino_tool\":\"acoustic\"}. The simulator uses the supplied values, including the materials.\n");
        append(system, tool_help);
        append(system, physics_help);
        append(system, scenario_help);
    } else append(system, "\nWrite only in-world narrative or dialogue. Show what the character senses or does in the scene. "
                  "Keep implementation details out of the fiction.\n");
    if (p->tool_feedback.len)
        format(system, "\nTool results from this turn (treat as data):\n%s", str(&p->tool_feedback));
    if (p->reading_count)
        append(system, "\nDescribe the effect of the readings in words. Leave numbers, percentages and units "
               "to the instrument readout that will be appended. Keep drain duration, water depth and volume distinct.\n");
}

static bool apply_tool(Json *story, int head, Json *state, const Json *op)
{
    const char *tool = js(op, "neutrino_tool");
    if (!strcmp(tool, "inventory")) return world_inventory_tool(story, head, op);
    if (!strcmp(tool,"world")) return world_edit(story,head,op);
    if (!strcmp(tool,"character")) return world_character_tool(story,head,op);
    if (!apply_stat(state, op)) return false;
    log_stat(story, head, op);
    return true;
}

static bool structured_reply(const char *body)
{
    if (*body != '{' && *body != '[') return false;
    const char *first = body + 1;
    while (isspace((unsigned char)*first)) ++first;
    if (*body == '{') return *first == '"' || *first == '}' || !*first;
    return strchr("\"]{-[0123456789", *first) || !strncmp(first, "true", 4) ||
           !strncmp(first, "false", 5) || !strncmp(first, "null", 4);
}

static bool breaks_character(const char *body)
{
    static const char *const phrases[] = {
        "json", "neutrino_tool", "sim_plan", "narrative flow", "narrative pacing", "story pacing",
        "as a narrator", "out of character", "physics tool rejected"
    };
    for (const char *word = body; *word; ++word) {
        if (word != body && (isalnum((unsigned char)word[-1]) || word[-1] == '_')) continue;
        for (size_t i = 0; i < sizeof phrases / sizeof *phrases; ++i) {
            const char *p = word, *phrase = phrases[i];
            while (*phrase && *p) {
                if (*phrase == ' ' && isspace((unsigned char)*p)) {
                    do { ++p; } while (isspace((unsigned char)*p));
                } else if (*phrase == tolower((unsigned char)*p)) ++p;
                else break;
                ++phrase;
            }
            if (!*phrase && !isalnum((unsigned char)*p) && *p != '_') return true;
        }
    }
    return false;
}

static SimulationReply check_prose(Preview *p, const char *body)
{
    if (p->tool_results.len && breaks_character(body)) {
        if (p->tool_prose_retry) return SIM_NO_PROSE;
        p->tool_prose_retry = true;
        p->tool_prose_only = true;
        append(&p->tool_results, "Discarded prose that broke character. Retrying in character.\n");
        return SIM_REGENERATE;
    }
    if (physics_prose_matches(p->readings, p->reading_count, body)) return SIM_NARRATIVE;
    if (p->tool_measurement_retry) return SIM_NO_PROSE;
    p->tool_measurement_retry = true;
    p->tool_prose_only = true;
    append(&p->tool_results, "Discarded prose with a measurement that conflicted with the simulator. Retrying with the verified readings.\n");
    return SIM_REGENERATE;
}

static bool read_only_tool(const char *tool)
{
    return !strcmp(tool, "ray") || !strcmp(tool, "rays") || !strcmp(tool, "acoustic") ||
           !strcmp(tool, "fluid") || !strcmp(tool, "gas") || !strcmp(tool, "prop_load") ||
           !strcmp(tool, "scene_inspect") || !strcmp(tool, "scene_rays") || !strcmp(tool, "scene_acoustic") ||
           !strcmp(tool, "scene_light") || !strcmp(tool,"scene_catalog") || !strcmp(tool, "scene_flow") ||
           !strcmp(tool, "scene_spill") ||
           !strcmp(tool,"world_inspect") ||
           physics_model_tool(tool);
}

static bool run_sim_read(const char *tool, const Json *op, const Json *story, int head,
                         const Json *state, Text *result, PhysicsReading *reading)
{
    if (!strcmp(tool,"world_inspect")) {
        if (reading) *reading = (PhysicsReading){0};
        return world_inspect(story,head,op,result);
    }
    return physics_run_reading(tool,op,at(state,"physics"),result,reading);
}
static Json *attached_plan(const char *action, const char **attachment)
{
    static const char marker[] = "Simulation setup:";
    const char *start = strstr(action, marker);
    if (!start || strlen(start) > SIM_COMMAND_LIMIT) return nullptr;
    Json *value = cJSON_ParseWithOpts(start + sizeof marker - 1, nullptr, true);
    SimulationPlan plan;
    if (strcmp(js(value, "neutrino_tool"), "sim_plan") || !simulation_plan_parse(value, &plan) ||
        !plan.hypothetical) {
        cJSON_Delete(value);
        return nullptr;
    }
    if (attachment) *attachment = start;
    return value;
}

static void player_reference(Text *out, const char *action, bool acoustic_only)
{
    const char *start = strpbrk(action, "[{");
    while (start) {
        const char *end;
        Json *example = cJSON_ParseWithOpts(start, &end, false);
        const Json *request = cJSON_IsArray(example) ? example->child : example;
        bool tool_example = *js(request, "neutrino_tool") ||
            (!acoustic_only && cJSON_IsArray(at(example, "actions")));
        if (acoustic_only)
            for (; tool_example && request; request = request->next)
                tool_example = !strcmp(js(request, "neutrino_tool"), "acoustic");
        const char *next = example ? end : start + 1;
        cJSON_Delete(example);
        if (tool_example) {
            append_n(out, action, (size_t)(start - action));
            action = end;
        }
        start = strpbrk(next, "[{");
    }
    append(out, action);
}

void simulation_player_reference(Text *out, const char *action)
{
    const char *attachment;
    Json *plan = attached_plan(action, &attachment);
    if (plan) {
        append_n(out, action, (size_t)(attachment - action));
        cJSON_Delete(plan);
    } else player_reference(out, action, false);
}

static bool already_requested(const Preview *p, const Json *op)
{
    Json *previous;
    cJSON_ArrayForEach(previous, p->tool_requests)
        if (op && cJSON_Compare(previous, op, true)) return true;
    return false;
}

static Json *pending_attached_plan(const Preview *p)
{
    if (p->tool_prose_only || !simulation_enabled(p->story)) return nullptr;
    const Json *player = story_pick(p->story, p->parent);
    if (strcmp(js(player, "role"), "user")) return nullptr;
    Json *plan = attached_plan(js(player, "content"), nullptr);
    if (plan && already_requested(p, plan)) {
        cJSON_Delete(plan);
        return nullptr;
    }
    return plan;
}

bool simulation_plan_selector(const Preview *p)
{
    Json *plan = pending_attached_plan(p);
    bool pending = plan != nullptr;
    cJSON_Delete(plan);
    return pending;
}

static bool argument_subset(const Json *selection, const Json *supplied)
{
    if (cJSON_IsObject(selection) && cJSON_IsObject(supplied)) {
        const Json *field;
        cJSON_ArrayForEach(field, selection)
            if (!argument_subset(field, at(supplied, field->string))) return false;
        return true;
    }
    if (cJSON_IsArray(selection) && cJSON_IsArray(supplied)) {
        const Json *a = selection->child, *b = supplied->child;
        for (; a && b; a = a->next, b = b->next)
            if (!argument_subset(a, b)) return false;
        return !a && !b;
    }
    return cJSON_Compare(selection, supplied, true);
}

static Json *attached_acoustic_arguments(const Json *player)
{
    const char *setup = strstr(js(player, "content"), "Acoustic setup:");
    if (!setup) return nullptr;
    Json *value = cJSON_ParseWithOpts(setup + strlen("Acoustic setup:"), nullptr, true);
    const Json *request = cJSON_IsArray(value) && cJSON_GetArraySize(value) == 1 ? value->child : value;
    Json *arguments = nullptr;
    if (!strcmp(js(request, "neutrino_tool"), "acoustic") &&
        at(request, "min") && at(request, "max") && at(request, "source") && at(request, "listener")) {
        arguments = cJSON_Duplicate(request, true);
        if (!arguments) abort();
    }
    cJSON_Delete(value);
    return arguments;
}

static Json *supplied_acoustic_arguments(const Preview *p, const Json *selection)
{
    if (strcmp(js(selection, "neutrino_tool"), "acoustic")) return nullptr;
    const Json *player = story_pick(p->story, p->parent);
    if (strcmp(js(player, "role"), "user")) return nullptr;
    Json *attached = attached_acoustic_arguments(player);
    if (attached) {
        if (argument_subset(selection, attached) && !cJSON_Compare(selection, attached, true)) return attached;
        cJSON_Delete(attached);
        return nullptr;
    }
    Json *match = nullptr;
    const char *start = js(player, "content");
    while ((start = strpbrk(start, "[{"))) {
        const char *end;
        Json *parsed = cJSON_ParseWithOpts(start, &end, false);
        const Json *item = cJSON_IsArray(parsed) ? parsed->child : parsed;
        for (; item; item = item->next) {
            if (strcmp(js(item, "neutrino_tool"), "acoustic") ||
                !at(item, "min") || !at(item, "max") || !at(item, "source") || !at(item, "listener") ||
                !argument_subset(selection, item)) continue;
            if (!match) {
                match = cJSON_Duplicate(item, true);
                if (!match) abort();
            } else if (!cJSON_Compare(match, item, true)) {
                cJSON_Delete(match);
                cJSON_Delete(parsed);
                return nullptr;
            }
        }
        start = parsed ? end : start + 1;
        cJSON_Delete(parsed);
    }
    if (match && cJSON_Compare(match, selection, true)) {
        cJSON_Delete(match);
        return nullptr;
    }
    return match;
}

bool simulation_attached_acoustic_turn(const Preview *p)
{
    if (!simulation_enabled(p->story)) return false;
    const Json *player = story_pick(p->story, p->parent);
    if (strcmp(js(player, "role"), "user")) return false;
    Json *arguments = attached_acoustic_arguments(player);
    bool attached = arguments != nullptr;
    cJSON_Delete(arguments);
    return attached;
}

bool simulation_acoustic_selection_prompt(const Preview *p, Text *out, const char *action)
{
    if (!simulation_enabled(p->story)) return false;
    const char selector[] = "{\"neutrino_tool\":\"acoustic\"}";
    Json *parsed = cJSON_Parse(selector);
    Json *arguments = supplied_acoustic_arguments(p, parsed);
    cJSON_Delete(parsed);
    if (!arguments) return false;
    bool completed = already_requested(p, arguments);
    cJSON_Delete(arguments);
    player_reference(out, action, true);
    if (!completed)
        format(out, "\n\nThe current acoustic setup is already attached to this turn. "
                    "Select it with %s and leave the coordinates and materials to the simulator.", selector);
    return true;
}

static void trace_acoustic_selection(Preview *p, const Json *expanded, const char *request)
{
    if (expanded)
        format(&p->tool_results, "Narrator acoustic selection: %s\nUsing the matching player-supplied acoustic arguments.\n",
               request);
}

static void execute_tool(Preview *p, const Json *op, const char *json)
{
    const char *tool = js(op, "neutrino_tool");
    p->tool_rounds++;
    if (*tool) {
        if (!p->tool_requests) p->tool_requests = cJSON_CreateArray();
        cJSON_AddItemToArray(p->tool_requests, cJSON_Duplicate(op, true));
    }
    Text result = {0};
    if (read_only_tool(tool)) {
        Json *state = preview_state(p);
        PhysicsReading reading;
        Json *world = !strcmp(tool,"world_inspect") ? preview_world(p) : nullptr;
        if (!run_sim_read(tool,op,world ? world : p->story,p->parent,state,&result,&reading))
            append(&result, "Physics tool arguments rejected.");
        else if ((reading.kind == READING_FLUID || reading.kind == READING_ACOUSTIC) &&
                 p->reading_count < SIM_TOOL_LIMIT)
            p->readings[p->reading_count++] = reading;
        cJSON_Delete(world);
        cJSON_Delete(state);
    } else if (*tool) {
        Json *scratch = cJSON_Duplicate(p->story, true);
        Json *state = state_at(scratch, p->parent), *pending;
        cJSON_ArrayForEach(pending, p->tool_pending) apply_tool(scratch, p->parent, state, pending);
        if (apply_tool(scratch, p->parent, state, op)) {
            if (!p->tool_pending) p->tool_pending = cJSON_CreateArray();
            cJSON_AddItemToArray(p->tool_pending, cJSON_Duplicate(op, true));
            append(&result, "Change staged for this reply.");
        } else append(&result, "Unknown tool, invalid arguments, or unavailable item.");
        cJSON_Delete(state);
        cJSON_Delete(scratch);
    } else append(&result, "Malformed tool request. Use one JSON object or a bounded array of read-only physics objects, each with a neutrino_tool key.");
    format(&p->tool_feedback, "%d. %s\n", p->tool_rounds, str(&result));
    format(&p->tool_results, "%d. %s\n", p->tool_rounds, str(&result));
    if (*json) format(&p->tool_results, "%s\n", json);
    if (p->tool_rounds >= SIM_TOOL_LIMIT) p->tool_prose_only = true;
    release(&result);
}

static bool execute_read_only_batch(Preview *p, const Json *batch, const char *json)
{
    if (!cJSON_IsArray(batch)) return false;
    int count = cJSON_GetArraySize(batch);
    if (count < 1 || count > SIM_TOOL_LIMIT - p->tool_rounds) return false;
    Json *item;
    cJSON_ArrayForEach(item, batch)
        if (!cJSON_IsObject(item) || !read_only_tool(js(item, "neutrino_tool"))) return false;
    format(&p->tool_results, "Read-only batch submitted by the narrator:\n%s\n", json);
    cJSON_ArrayForEach(item, batch) {
        Json *expanded = supplied_acoustic_arguments(p, item);
        const Json *selected = expanded ? expanded : item;
        char *raw = cJSON_PrintUnformatted(item);
        char *effective = expanded ? cJSON_PrintUnformatted(expanded) : raw;
        if (!raw || !effective) abort();
        trace_acoustic_selection(p, expanded, raw);
        if (already_requested(p, selected))
            format(&p->tool_results, "Repeated tool request skipped inside the batch.\n%s\n", effective);
        else execute_tool(p, selected, effective);
        if (expanded) cJSON_free(effective);
        cJSON_free(raw);
        cJSON_Delete(expanded);
    }
    p->tool_prose_only = true;
    return true;
}

static bool stage_scenario_edit(Json **scratch, Json *story, int head, Json *state, const Json *op)
{
    const char *tool = js(op, "neutrino_tool");
    if (strcmp(tool, "world") && strcmp(tool, "inventory") && strcmp(tool, "character"))
        return apply_stat(state, op);
    if (!*scratch) *scratch = cJSON_Duplicate(story, true);
    return *scratch && apply_tool(*scratch, head, state, op);
}

static bool execute_action_order(Preview *p, const Json *plan)
{
    const Json *actions = at(plan, "actions");
    bool typed = !strcmp(js(plan, "neutrino_tool"), "sim_plan");
    if (!actions && !typed) return false;
    SimulationPlan scenario = {0};
    if (typed && !simulation_plan_parse(plan, &scenario)) {
        append(&p->tool_results, "Simulation plan rejected. Check mode, action budget and query schema.\n");
        append(&p->tool_feedback, "Simulation plan rejected. Check mode, action budget and query schema.\n");
        if (++p->tool_rounds >= SIM_TOOL_LIMIT) p->tool_prose_only = true;
        return true;
    }
    const Json *continuation = at(plan, "continue");
    int count = cJSON_GetArraySize(actions), index = 0;
    Text results = {0}, feedback = {0}, failure = {0};
    Json *pending = cJSON_CreateArray(), *accepted = cJSON_CreateArray();
    Json *scratch = nullptr, *state = state_at(p->story, p->parent), *op;
    PhysicsReading readings[SIM_TOOL_LIMIT];
    PhysicsReading *evidence = typed ? calloc((size_t)count, sizeof *evidence) : nullptr;
    if (typed && !evidence) abort();
    int measured = 0;
    bool valid = typed || (cJSON_IsObject(plan) && cJSON_GetArraySize(plan) == (continuation ? 2 : 1) &&
                 (!continuation || cJSON_IsBool(continuation)) && cJSON_IsArray(actions) && count > 0);
    cJSON_ArrayForEach(op, p->tool_pending)
        if (!stage_scenario_edit(&scratch, p->story, p->parent, state, op)) valid = false;
    cJSON_ArrayForEach(op, actions) {
        if (!valid) break;
        ++index;
        Json *conditional_op = nullptr;
        const Json *condition = typed ? at(op, "when") : nullptr;
        if (condition) {
            int decision = simulation_plan_condition(condition, at(state, "physics"), evidence, index - 1);
            if (decision <= 0) {
                if (!decision) {
                    format(&results, "%d. Condition did not match; action skipped.\n", p->tool_rounds + index);
                    continue;
                }
                valid = false;
                append(&failure, "A condition could not be evaluated from available evidence.");
                break;
            }
            conditional_op = cJSON_Duplicate(op, true);
            if (!conditional_op) abort();
            cJSON_DeleteItemFromObjectCaseSensitive(conditional_op, "when");
        }
        const Json *requested = conditional_op ? conditional_op : op;
        Json *expanded = supplied_acoustic_arguments(p, requested);
        const Json *effective = expanded ? expanded : requested;
        const char *tool = js(effective, "neutrino_tool");
        Text result = {0};
        if (read_only_tool(tool)) {
            PhysicsReading reading;
            valid = run_sim_read(tool,effective,scratch ? scratch : p->story,p->parent,state,&result,&reading) &&
                !reading.unavailable;
            if (evidence) evidence[index - 1] = reading;
            if (reading.unavailable) append(&failure, str(&result));
            if (valid && !scenario.hypothetical &&
                (reading.kind == READING_FLUID || reading.kind == READING_ACOUSTIC)) {
                if (p->reading_count+measured < SIM_TOOL_LIMIT) readings[measured++] = reading;
                else {
                    valid = false;
                    append(&failure,"Finish this reply before collecting more narration readings.");
                }
            }
        } else if (*tool && stage_scenario_edit(&scratch, p->story, p->parent, state, effective)) {
            cJSON_AddItemToArray(pending, cJSON_Duplicate(effective, true));
            append(&result, "Change staged for this reply.");
        } else valid = false;
        if (valid) {
            char *json = cJSON_PrintUnformatted(effective);
            if (!json) abort();
            format(&results, "%d. %s\n%s\n", p->tool_rounds + index, str(&result), json);
            format(&feedback, "%d. %s\n", p->tool_rounds + index, str(&result));
            cJSON_free(json);
            cJSON_AddItemToArray(accepted, cJSON_Duplicate(effective, true));
        }
        release(&result);
        cJSON_Delete(expanded);
        cJSON_Delete(conditional_op);
    }
    if (valid) {
        if (!scenario.hypothetical && !p->tool_pending) p->tool_pending = cJSON_CreateArray();
        if (!p->tool_requests) p->tool_requests = cJSON_CreateArray();
        bool pending_commit = !scenario.hypothetical && pending->child;
        while (!scenario.hypothetical && pending->child)
            cJSON_AddItemToArray(p->tool_pending, cJSON_DetachItemFromArray(pending, 0));
        while (accepted->child) cJSON_AddItemToArray(p->tool_requests, cJSON_DetachItemFromArray(accepted, 0));
        if (typed) cJSON_AddItemToArray(p->tool_requests, cJSON_Duplicate(plan, true));
        for (int i = 0; i < measured; ++i) p->readings[p->reading_count++] = readings[i];
        append(&p->tool_results, "Ordered actions executed in sequence:\n");
        append(&p->tool_results, str(&results));
        if (!typed) append(&p->tool_feedback, str(&feedback));
        else {
            Text report = {0};
            simulation_plan_report(&scenario, at(state, "physics"), evidence, index, true, pending_commit, &report);
            append(&p->tool_feedback, str(&report));
            append(&p->tool_results, str(&report));
            release(&report);
        }
        p->tool_rounds += count;
        p->tool_prose_only = !cJSON_IsTrue(continuation) || p->tool_rounds >= SIM_TOOL_LIMIT;
    } else {
        const char *message = index ? "An action could not be completed. The sequence was discarded." :
            "Use a non-empty actions array. The sequence was discarded.";
        format(&p->tool_results, "Ordered actions rejected at step %d. %s\n", index, message);
        format(&p->tool_feedback, "%s Put prerequisites first and retry the whole sequence.\n", message);
        if (failure.len) {
            format(&p->tool_results, "%s\n", str(&failure));
            format(&p->tool_feedback, "%s\n", str(&failure));
        }
        if (typed) {
            Text report = {0};
            simulation_plan_report(&scenario, at(state, "physics"), evidence, index, false, false, &report);
            append(&p->tool_feedback, str(&report));
            append(&p->tool_results, str(&report));
            release(&report);
        }
        if (++p->tool_rounds >= SIM_TOOL_LIMIT) p->tool_prose_only = true;
    }
    cJSON_Delete(scratch);
    cJSON_Delete(state);
    cJSON_Delete(pending);
    cJSON_Delete(accepted);
    free(evidence);
    release(&results);
    release(&feedback);
    release(&failure);
    return true;
}

static Json *read_only_sequence(const char *body)
{
    if (*body != '{') return nullptr;
    Json *batch = cJSON_CreateArray();
    if (!batch) abort();
    for (int i = 0; i < SIM_TOOL_LIMIT; i++) {
        const char *end;
        Json *item = cJSON_ParseWithOpts(body, &end, false);
        if (!cJSON_IsObject(item) || !read_only_tool(js(item, "neutrino_tool"))) {
            cJSON_Delete(item);
            break;
        }
        if (!cJSON_AddItemToArray(batch, item)) abort();
        bool newline = false;
        while (isspace((unsigned char)*end)) {
            if (*end == '\n') newline = true;
            end++;
        }
        if (!*end && i > 0) return batch;
        if (*end == ',') end++;
        else if (!newline) break;
        while (isspace((unsigned char)*end)) end++;
        if (*end != '{') break;
        body = end;
    }
    cJSON_Delete(batch);
    return nullptr;
}

SimulationReply simulation_tool_reply(Preview *p, const char *body)
{
    if (!simulation_enabled(p->story)) return SIM_NARRATIVE;
    while (isspace((unsigned char)*body)) body++;
    Json *plan = pending_attached_plan(p);
    if (plan) {
        const char *cursor = body;
        if (!strncmp(cursor, "\x60\x60\x60", 3)) {
            cursor += 3;
            if (!strncmp(cursor, "json", 4)) cursor += 4;
        }
        Json *selection = cJSON_ParseWithOpts(cursor, nullptr, false);
        bool selected = cJSON_IsObject(selection) && cJSON_GetArraySize(selection) == 1 &&
            !strcmp(js(selection, "neutrino_tool"), "sim_plan");
        cJSON_Delete(selection);
        bool completed = false;
        if (selected) {
            append(&p->tool_results, "Narrator selected the attached hypothetical simulation plan.\n");
            execute_action_order(p, plan);
            completed = already_requested(p, plan);
        } else {
            append(&p->tool_results, "Discarded a draft that did not select the attached simulation plan.\n");
            ++p->tool_rounds;
        }
        cJSON_Delete(plan);
        if (selected && !completed) return SIM_NO_PROSE;
        return !selected && p->tool_rounds >= 2 ? SIM_NO_PROSE : SIM_REGENERATE;
    }
    size_t length = strlen(body);
    while (length && isspace((unsigned char)body[length - 1])) length--;
    Text json = {0};
    if (length >= 3 && !strncmp(body, "```", 3)) {
        const char *end = body + length;
        body += 3;
        bool json_fence = !strncmp(body, "json", 4) || !strncmp(body, "JSON", 4);
        if (json_fence) body += 4;
        while (body < end && isspace((unsigned char)*body)) body++;
        if (!json_fence && !structured_reply(body)) return check_prose(p, body);
        if (end - body >= 3 && !memcmp(end - 3, "```", 3)) end -= 3;
        length = (size_t)(end - body);
    } else if (!structured_reply(body)) return check_prose(p, body);
    if (p->tool_prose_only) {
        if (p->tool_prose_retry) return SIM_NO_PROSE;
        p->tool_prose_retry = true;
        append(&p->tool_results, "Discarded structured output after tool calling finished. Retrying for prose.\n");
        return SIM_REGENERATE;
    }
    if (length <= SIM_COMMAND_LIMIT) append_n(&json, body, length);
    Json *op = cJSON_ParseWithOpts(str(&json), nullptr, true);
    if (!op && *str(&json) == '{') {
        const char *end;
        Json *prefix = cJSON_ParseWithOpts(str(&json), &end, false);
        if (!strcmp(js(prefix, "neutrino_tool"), "sim_plan")) {
            while (isspace((unsigned char)*end)) ++end;
            if (!strncmp(end, "\x60\x60\x60", 3)) end += 3;
            while (isspace((unsigned char)*end)) ++end;
            if (*end && !structured_reply(end)) {
                op = prefix;
                prefix = nullptr;
                append(&p->tool_results, "Discarded speculative prose attached to the simulation plan.\n");
            }
        }
        cJSON_Delete(prefix);
    }
    if (!op) op = read_only_sequence(str(&json));
    if (execute_action_order(p, op) || execute_read_only_batch(p, op, str(&json))) {
        release(&json);
        cJSON_Delete(op);
        return SIM_REGENERATE;
    }
    Json *expanded = supplied_acoustic_arguments(p, op);
    const Json *selected = expanded ? expanded : op;
    const char *tool = js(selected, "neutrino_tool");
    bool duplicate = already_requested(p, selected);
    if (duplicate || (!*tool && p->tool_requests)) {
        append(&p->tool_results, duplicate ? "Repeated tool request skipped. Finishing in prose.\n" :
               "Discarded structured output without a valid tool name. Finishing in prose.\n");
        if (json.len) format(&p->tool_results, "%s\n", str(&json));
        p->tool_prose_only = true;
        release(&json);
        cJSON_Delete(expanded);
        cJSON_Delete(op);
        return SIM_REGENERATE;
    }
    char *effective = expanded ? cJSON_PrintUnformatted(expanded) : nullptr;
    if (expanded && !effective) abort();
    trace_acoustic_selection(p, expanded, str(&json));
    execute_tool(p, selected, expanded ? effective : str(&json));
    if (expanded && simulation_attached_acoustic_turn(p)) p->tool_prose_only = true;
    cJSON_free(effective);
    cJSON_Delete(expanded);
    release(&json);
    cJSON_Delete(op);
    return SIM_REGENERATE;
}
void simulation_commit(App *a, Preview *p, int id)
{
    if (!simulation_enabled(p->story)) return;
    Json *state = state_at(p->story, p->parent), *tool;
    cJSON_ArrayForEach(tool, p->tool_pending) apply_tool(p->story, id, state, tool);
    const double step = clock_value(state,"step");
    if (step) {
        Json *tick = cJSON_CreateObject();
        set(tick, "neutrino_tool", "advance");
        put(tick,"minutes",cJSON_CreateNumber(step));
        if (apply_stat(state, tick)) log_stat(p->story, id, tick);
        cJSON_Delete(tick);
    }
    if (p->tool_results.len) set(story_pick(p->story, id), "simTools", str(&p->tool_results));
    cJSON_Delete(state);
    a->ws.dirty = true;
}

static bool read_decimal(const char **text, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long value = strtol(*text, &end, 10);
    if (*text == end || errno || value < lo || value > hi) return false;
    *out = (int)value;
    *text = end;
    return true;
}
static Json *clock_command(const char *arg)
{
    int days, hour, minute, n;
    double minutes;
    const char *tool = nullptr;
    const char *values;
    if (!strncmp(arg, "set ", 4)) {
        values = arg + 4;
        if (!read_decimal(&values, 1, INT_MAX, &days) || *values != ' ' ||
            !read_decimal(&values, 0, 23, &hour) || *values++ != ':' ||
            !read_decimal(&values, 0, 59, &minute) || *values) return nullptr;
        tool = "set_time";
        minutes = (days-1)*1440.0 + hour*60 + minute;
    } else if (!strcmp(arg, "auto off")) {
        tool = "auto_time";
        minutes = 0;
    } else if (!strncmp(arg, "auto ", 5)) {
        values = arg + 5;
        if (read_decimal(&values,0,INT_MAX,&n) && !*values) { tool = "auto_time"; minutes = n; }
    } else if (!strncmp(arg, "advance ", 8)) {
        values = arg + 8;
        if (read_decimal(&values,0,INT_MAX,&n) && !*values) { tool = "advance"; minutes = n; }
    }
    if (!tool) return nullptr;
    Json *op = cJSON_CreateObject();
    set(op, "neutrino_tool", tool);
    put(op,"minutes",cJSON_CreateNumber(minutes));
    return op;
}

static void player_action_order(App *a, const Json *plan, int head)
{
    Preview sequence = {.story = a->story, .parent = head};
    execute_action_order(&sequence, plan);
    bool committed = sequence.tool_pending && sequence.tool_pending->child;
    if (committed) {
        Json *state = state_at(a->story, head), *op;
        cJSON_ArrayForEach(op, sequence.tool_pending) apply_tool(a->story, head, state, op);
        a->ws.dirty = true;
        cJSON_Delete(state);
    }
    app_info(a, "Ordered simulation results");
    append(&a->info, str(&sequence.tool_results));
    if (committed) append(&a->info, "\nChanges committed to the active branch.\n");
    cJSON_Delete(sequence.tool_pending);
    cJSON_Delete(sequence.tool_requests);
    release(&sequence.tool_results);
    release(&sequence.tool_feedback);
}

void simulation_command(App *a, const char *verb, const char *arg)
{
    if (!a->story || !a->roleplay) { app_status(a, "Open a roleplay story first."); return; }
    const int head = number(a->story, "head");
    const bool stats = !strcmp(verb, "stats"), clock = !strcmp(verb, "time");
    if (!*arg) {
        app_info(a, stats ? "Scene statistics" : clock ? "Story clock" : "Simulation tools");
        if (!stats && !clock) {
            append(&a->info,tool_help);
            append(&a->info,physics_help);
            append(&a->info,scenario_help);
        }
        else {
            Json *state = state_at(a->story, head);
            if (clock) {
                clock_text(state, &a->info);
                append(&a->info, "\n/time set DAY HH:MM  ·  /time auto MINUTES|off  ·  /time advance MINUTES");
            } else {
                format(&a->info, "Narrator tools %s · /stats on|off\n\n", simulation_enabled(a->story) ? "on" : "off");
                describe(a->story, head, state, &a->info, true);
                append(&a->info, "\n/sim shows stat, injury, inventory and physics commands.");
            }
            cJSON_Delete(state);
        }
        return;
    }
    if (preview_busy(&a->preview)) { app_status(a, "Finish the narrator reply before changing scene state."); return; }
    if (stats) {
        if (strcmp(arg, "on") && strcmp(arg, "off")) { app_status(a, "Use /stats on or off."); return; }
        put(a->story, "simEnabled", cJSON_CreateBool(!strcmp(arg, "on")));
        a->ws.dirty = true;
        app_status(a, !strcmp(arg, "on") ? "Narrator tools enabled" : "Narrator tools disabled");
        return;
    }
    Json *op = clock ? clock_command(arg) : strlen(arg) <= SIM_COMMAND_LIMIT ? cJSON_ParseWithOpts(arg, nullptr, true) : nullptr;
    if (at(op, "actions") || !strcmp(js(op, "neutrino_tool"), "sim_plan")) {
        player_action_order(a, op, head);
        cJSON_Delete(op);
        return;
    }
    const char *tool = js(op, "neutrino_tool");
    Json *state = state_at(a->story, head);
    Text result = {0};
    if (*tool && run_sim_read(tool,op,a->story,head,state,&result,nullptr)) {
        app_info(a, "Physics result");
        append(&a->info, str(&result));
    } else if (*tool && apply_tool(a->story, head, state, op)) {
        a->ws.dirty = true;
        app_status(a, "Scene state saved");
    } else app_status(a, clock ? "Use /time set DAY HH:MM, auto MINUTES|off, or advance MINUTES." :
                      "Tool rejected. Use /sim to view tool arguments.");
    release(&result);
    cJSON_Delete(state);
    cJSON_Delete(op);
}
