#include "app.h"
#include "ray.h"
#include "physics_models.h"
#include "spill.h"
#include "runoff.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void app_info(App *app, const char *title) { (void)app; (void)title; }
void app_status(App *app, const char *message) { (void)app; (void)message; }

static void free_protocol(Preview *p)
{
    cJSON_Delete(p->story);
    cJSON_Delete(p->tool_pending);
    cJSON_Delete(p->tool_requests);
    release(&p->tool_feedback);
    release(&p->tool_results);
}

static bool rigid_actions(Json *settings, const char *actions)
{
    Json *array = cJSON_Parse(actions), *op;
    bool valid = cJSON_IsArray(array);
    cJSON_ArrayForEach(op, array)
        if (!physics_scene_edit(settings, op)) { valid = false; break; }
    cJSON_Delete(array);
    return valid;
}

static double rigid_value(const Json *body, const char *key)
{
    const Json *value = at(body, key);
    return cJSON_IsNumber(value) ? value->valuedouble : NAN;
}

static bool rigid_dynamics(void)
{
    const char *room =
        "[{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[8,6,4]},"
        "{\"neutrino_tool\":\"scene_material\",\"name\":\"ceramic\",\"mechanical\":{\"fracture_j\":30}},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"floor\",\"kind\":\"wall\",\"min\":[0,0,0],\"max\":[8,6,0.25]},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"jug\",\"kind\":\"object\",\"min\":[2,2,2],\"max\":[2.5,2.5,2.5],"
        "\"material_id\":\"ceramic\"},"
        "{\"neutrino_tool\":\"scene_body\",\"name\":\"jug\",\"mass_kg\":2,\"liquid_l\":1},"
        "{\"neutrino_tool\":\"scene_constraint\",\"name\":\"jug\",\"mode\":\"fixed\"},"
        "{\"neutrino_tool\":\"scene_step\",\"seconds\":0.25}]";
    const char *release_and_drop =
        "[{\"neutrino_tool\":\"scene_constraint\",\"name\":\"jug\",\"mode\":\"free\"},"
        "{\"neutrino_tool\":\"scene_step\",\"seconds\":0.85}]";
    Json *settings = cJSON_CreateObject();
    bool ok = rigid_actions(settings, room);
    const Json *held = physics_scene_body(settings, "jug");
    ok &= rigid_value(held, "translation_m") == 0;

    Json *blocked = cJSON_Duplicate(settings, true);
    ok &= rigid_actions(blocked,
        "[{\"neutrino_tool\":\"scene_place\",\"name\":\"jug#2\",\"kind\":\"object\",\"min\":[4,4,1],\"max\":[5,5,2]},"
        "{\"neutrino_tool\":\"scene_constraint\",\"name\":\"jug\",\"mode\":\"free\"}]");
    Json *before_failure = cJSON_Duplicate(blocked, true);
    bool drop_rejected = !rigid_actions(blocked, "[{\"neutrino_tool\":\"scene_step\",\"seconds\":0.85}]");
    ok &= drop_rejected && cJSON_Compare(blocked, before_failure, true);
    cJSON_Delete(before_failure);
    cJSON_Delete(blocked);

    ok &= rigid_actions(settings, release_and_drop);
    const Json *jug = physics_scene_body(settings, "jug"), *piece = physics_scene_body(settings, "jug#2");
    const Json *contacts = at(jug, "contacts");
    ok &= cJSON_IsTrue(at(jug, "fractured")) && !strcmp(js(piece, "fragment_of"), "jug") &&
        fabs(rigid_value(jug, "fracture_energy_j") - 3 * 9.80665 * 1.75) < 0.001 &&
        fabs(rigid_value(jug, "fracture_time_s") - (0.25 + sqrt(3.5 / 9.80665))) < 1e-5 &&
        fabs(rigid_value(jug, "translation_m") - 1.75) < 1e-8 &&
        rigid_value(jug, "released_l") == 1 && rigid_value(jug, "liquid_l") == 0 &&
        rigid_value(jug, "mass_kg") + rigid_value(piece, "mass_kg") == 2 &&
        !strcmp(js(cJSON_GetArrayItem(contacts, 0), "target"), "floor");
    ok &= rigid_actions(settings, "[{\"neutrino_tool\":\"scene_step\",\"seconds\":0.1}]");
    jug = physics_scene_body(settings, "jug");
    ok &= cJSON_IsTrue(at(jug, "fractured")) && rigid_value(jug, "released_l") == 1;
    cJSON_Delete(settings);

    settings = cJSON_CreateObject();
    ok &= rigid_actions(settings,
        "[{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[8,6,4]},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"a\",\"kind\":\"object\",\"min\":[1,1,1],\"max\":[2,2,2]},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"b\",\"kind\":\"object\",\"min\":[3,1,1],\"max\":[4,2,2]},"
        "{\"neutrino_tool\":\"scene_body\",\"name\":\"a\",\"mass_kg\":1,\"restitution\":1,\"velocity\":[4,3,2]},"
        "{\"neutrino_tool\":\"scene_body\",\"name\":\"b\",\"mass_kg\":1,\"restitution\":1},"
        "{\"neutrino_tool\":\"scene_constraint\",\"name\":\"a\",\"mode\":\"rail\",\"axis\":\"x\"},"
        "{\"neutrino_tool\":\"scene_constraint\",\"name\":\"b\",\"mode\":\"rail\",\"axis\":\"x\"},"
        "{\"neutrino_tool\":\"scene_step\",\"seconds\":0.5}]");
    double a[3], b[3];
    const Json *left = physics_scene_body(settings, "a"), *right = physics_scene_body(settings, "b");
    ok &= physics_vector(left, "velocity", a) && physics_vector(right, "velocity", b) &&
        fabs(a[0]) < 1e-8 && fabs(b[0] - 4) < 1e-8 && a[1] == 0 && a[2] == 0 &&
        fabs(rigid_value(left, "translation_m") - 1) < 1e-8 &&
        fabs(rigid_value(right, "translation_m") - 1) < 1e-8;
    cJSON_Delete(settings);
    return ok;
}

static bool rotation_and_spill(void)
{
    Json *settings = cJSON_CreateObject();
    bool ok = rigid_actions(settings,
        "[{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[8,6,8]},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"rotor\",\"kind\":\"object\",\"min\":[2,2,4],\"max\":[4,3,5]},"
        "{\"neutrino_tool\":\"scene_body\",\"name\":\"rotor\",\"mass_kg\":2},"
        "{\"neutrino_tool\":\"scene_impulse\",\"name\":\"rotor\",\"offset_m\":[1,0,0],\"impulse_ns\":[0,0.125,0]},"
        "{\"neutrino_tool\":\"scene_impulse\",\"name\":\"rotor\",\"offset_m\":[-1,0,0],\"impulse_ns\":[0,-0.125,0]},"
        "{\"neutrino_tool\":\"scene_step\",\"seconds\":0.5}]");
    const Json *rotor = physics_scene_body(settings, "rotor"), *orientation = at(rotor, "orientation");
    const Json *angle = cJSON_GetArrayItem(orientation, 3);
    // The balanced pushes give Lz=.25 kg m²/s; rectangular prism Iz=5/6 kg m².
    const double expected = .25 / (5.0/6) * .5;
    ok &= fabs(rigid_value(rotor, "rotation_rad")-expected) < 1e-10 &&
        cJSON_IsNumber(angle) && fabs(angle->valuedouble-sin(expected/2)) < 1e-10 &&
        fabs(rigid_value(rotor, "translation_m") - .5*9.80665*.5*.5) < 1e-9;
    ok &= rigid_actions(settings, "[{\"neutrino_tool\":\"scene_step\",\"seconds\":0.25}]");
    rotor = physics_scene_body(settings, "rotor");
    angle = cJSON_GetArrayItem(at(rotor, "orientation"), 3);
    ok &= cJSON_IsNumber(angle) && fabs(angle->valuedouble-sin(expected*.75)) < 1e-10;
    cJSON_Delete(settings);

    SpillSet spills = {0}, reloaded = {0};
    Spill *water = spill_add(&spills);
    if (!water) return false;
    strcpy(water->source, "jug");
    water->litres = 1.25;
    for (int i = 0; i < SPILL_PARCELS; ++i) {
        water->parcels[i].point[0] = i < 12 ? 1 : .1;
        water->parcels[i].point[1] = 1;
        water->parcels[i].point[2] = 2;
        water->parcels[i].velocity[0] = 1;
    }
    PhysicsBox room = {{0,0,0},{4,4,4}};
    SpillSurface solid[] = {{{{1.3,0,.25},{2,2,1}}, "tray"}, {{{0,0,0},{4,4,.25}}, "floor"}};
    for (int tick = 0; tick < 100; ++tick)
        spill_advance(&spills, &room, solid, 2, 10, tick*.0025, (tick+1)*.0025, nullptr);
    Json *state = cJSON_CreateObject(), *evidence = cJSON_CreateObject();
    spill_store(&spills, state);
    double litres;
    ok &= spill_evidence(state, "jug", "tray", evidence, &litres) &&
        litres == 0 && fabs(rigid_value(evidence, "airborne_l")-1.25) < 1e-12 &&
        spill_load(&reloaded, state);
    for (int tick = 100; tick < 400; ++tick)
        spill_advance(&reloaded, &room, solid, 2, 10, tick*.0025, (tick+1)*.0025, nullptr);
    if (reloaded.count == 1) {
        spill_store(&reloaded, state);
        ok &= spill_evidence(state, "jug", "tray", evidence, &litres) &&
            fabs(litres-.6) < 1e-12 && fabs(rigid_value(evidence, "deposited_l")-1.25) < 1e-12 &&
            rigid_value(evidence, "airborne_l") == 0;
        double center[3];
        ok &= physics_vector(evidence, "centroid_m", center) &&
            fabs(center[0] - (1+sqrt(.2))) < 1e-5 && center[2] == 1;
        ok &= spill_evidence(state, "jug", "floor", evidence, &litres) && fabs(litres-.65) < 1e-12;
    } else ok = false;
    cJSON_Delete(evidence);
    cJSON_Delete(state);
    free(spills.items);
    free(reloaded.items);
    return ok;
}

static bool runoff_conservation(void)
{
    Json *room = cJSON_Parse("{\"objects\":[]}");
    Json *args = cJSON_Parse("{\"min_xy\":[0,0],\"max_xy\":[2,1],\"cells\":[2,1],\"conductivity_m2_s\":0.1}");
    PhysicsBox bounds = {{0,0,0},{2,1,2}};
    Runoff runoff = {0};
    SpillSet staged = {0}, reloaded = {0};
    bool ok = runoff_configure(room, &bounds, args) && runoff_open(&runoff, room, &bounds);
    Spill *jug = spill_add(&staged), *cup = spill_add(&staged);
    if (jug && cup) {
        strcpy(jug->source, "jug");
        strcpy(cup->source, "cup");
        jug->litres = 1;
        cup->litres = .25;
        for (int j = 0; j < SPILL_PARCELS; ++j) {
            SpillParcel settled = {.point={.5,.5,0}, .surface="@room"};
            jug->parcels[j] = cup->parcels[j] = settled;
        }
        // Unit-area neighbors at the same elevation transfer 5% of the head difference in half a second.
        ok &= runoff_advance(&runoff, &staged, .5) && jug->film && cup->film &&
            fabs(jug->film[0]-.95) < 1e-12 && fabs(jug->film[1]-.05) < 1e-12 &&
            fabs(cup->film[0]-.2375) < 1e-12 && fabs(cup->film[1]-.0125) < 1e-12;
        spill_store(&staged, room);
        ok &= !runoff_configure(room, &bounds, args) && spill_load(&reloaded, room);
        Json *evidence = cJSON_CreateObject();
        double litres, depth;
        ok &= runoff_evidence(room, &bounds, "jug", "@room", evidence, &litres, &depth) &&
            fabs(litres-1) < 1e-12 && fabs(depth-.0011875) < 1e-12;
        cJSON_Delete(evidence);
        if (reloaded.count == 2) {
            // Raise the west cell 20 mm. Both tracers drain east, with no loss or second seeding.
            runoff.bed[0].height = .020;
            ok &= runoff_advance(&runoff, &reloaded, .5) && runoff_advance(&runoff, &reloaded, .5) &&
                reloaded.items[0].film[0] < 1e-12 && fabs(reloaded.items[0].film[1]-1) < 1e-12 &&
                reloaded.items[1].film[0] < 1e-12 && fabs(reloaded.items[1].film[1]-.25) < 1e-12;
        } else ok = false;
    } else ok = false;
    spill_release(&staged);
    spill_release(&reloaded);
    runoff_release(&runoff);
    cJSON_Delete(room);
    cJSON_Delete(args);

    // A seam inside a cell is assigned to the adjacent cell on the actual landing surface.
    room = cJSON_Parse("{\"objects\":[{\"name\":\"west\",\"min\":[0,0,0],\"max\":[0.8,1,0.2]},"
                      "{\"name\":\"east\",\"min\":[0.8,0,0],\"max\":[2,1,0.1]}]}");
    args = cJSON_Parse("{\"min_xy\":[0,0],\"max_xy\":[2,1],\"cells\":[2,1]}");
    ok &= runoff_configure(room, &bounds, args) && runoff_open(&runoff, room, &bounds);
    jug = spill_add(&staged);
    if (jug) {
        strcpy(jug->source, "jug");
        jug->litres = 1;
        for (int j = 0; j < SPILL_PARCELS; ++j)
            jug->parcels[j] = (SpillParcel){.point={.9,.5,.1}, .surface="east"};
        ok &= runoff_advance(&runoff, &staged, 0) && jug->film &&
            jug->film[0] == 0 && fabs(jug->film[1]-1) < 1e-12;
    } else ok = false;
    spill_release(&staged);
    runoff_release(&runoff);
    cJSON_Delete(room);
    cJSON_Delete(args);
    return ok;
}

static int rigid_timing(void)
{
    const int sizes[] = {1, 8, 32, 64, 128};
    puts("bodies,seconds,repetition,mean_ms");
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        Json *settings = cJSON_CreateObject();
        bool valid = rigid_actions(settings, "[{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[8,40,40]}]");
        for (int j = 0; valid && j < sizes[i]; ++j) {
            int y = 1 + 2 * (j % 16), z = 1 + 2 * (j / 16);
            Text operations = {0};
            format(&operations,
                "[{\"neutrino_tool\":\"scene_place\",\"name\":\"p%d\",\"kind\":\"object\",\"min\":[1,%d,%d],\"max\":[2,%d,%d]},"
                "{\"neutrino_tool\":\"scene_body\",\"name\":\"p%d\",\"mass_kg\":1,\"velocity\":[0.5,0,0]},"
                "{\"neutrino_tool\":\"scene_constraint\",\"name\":\"p%d\",\"mode\":\"rail\",\"axis\":\"x\"}]",
                j, y, z, y + 1, z + 1, j, j);
            valid = rigid_actions(settings, str(&operations));
            release(&operations);
        }
        Json *step = cJSON_Parse("{\"neutrino_tool\":\"scene_step\",\"seconds\":0.25}");
        for (int sample = 0; valid && sample <= 5; ++sample) {
            const int batches = 16;
            clock_t elapsed = 0;
            for (int batch = 0; valid && batch < batches; ++batch) {
                Json *scratch = cJSON_Duplicate(settings, true);
                clock_t start = clock();
                valid = scratch && physics_scene_edit(scratch, step);
                clock_t finish = clock();
                elapsed += finish - start;
                valid &= start != (clock_t)-1 && finish != (clock_t)-1;
                cJSON_Delete(scratch);
            }
            if (valid && sample) printf("%d,0.25,%d,%.6f\n", sizes[i], sample,
                                       1000.0 * elapsed / (CLOCKS_PER_SEC * (double)batches));
        }
        cJSON_Delete(step);
        cJSON_Delete(settings);
        if (!valid) return 1;
    }
    return 0;
}

static bool rigid_branch_replay(void)
{
    const char *stage =
        "{\"neutrino_tool\":\"sim_plan\",\"name\":\"drop\",\"mode\":\"stage\",\"actions\":["
        "{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[8,6,4]},"
        "{\"neutrino_tool\":\"scene_material\",\"name\":\"ceramic\",\"mechanical\":{\"fracture_j\":1}},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"box\",\"kind\":\"object\",\"min\":[1,1,1],\"max\":[2,2,2],\"material_id\":\"ceramic\"},"
        "{\"neutrino_tool\":\"scene_body\",\"name\":\"box\",\"mass_kg\":1,\"liquid_l\":0.5},"
        "{\"neutrino_tool\":\"scene_runoff\",\"min_xy\":[0,0],\"max_xy\":[8,6],\"cells\":[8,6]},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"rotor\",\"kind\":\"object\",\"min\":[4,3,2],\"max\":[5,4,3]},"
        "{\"neutrino_tool\":\"scene_body\",\"name\":\"rotor\",\"mass_kg\":2},"
        "{\"neutrino_tool\":\"scene_impulse\",\"name\":\"rotor\",\"offset_m\":[0.5,0,0],\"impulse_ns\":[0,0.25,0]},"
        "{\"neutrino_tool\":\"scene_step\",\"seconds\":0.5}],"
        "\"queries\":[{\"id\":\"landed\",\"predicate\":\"entity.contacts\",\"entity\":\"box\",\"target\":\"@room\"}]}";
    const char *inspect =
        "{\"neutrino_tool\":\"sim_plan\",\"name\":\"inspect\",\"mode\":\"hypothetical\","
        "\"actions\":[{\"neutrino_tool\":\"scene_catalog\"}],"
        "\"queries\":[{\"id\":\"landed\",\"predicate\":\"entity.contacts\",\"entity\":\"box\",\"target\":\"@room\"}]}";
    Workspace ws = {.root = cJSON_CreateObject()};
    Preview placed = {.story = cJSON_Parse("{\"simEnabled\":true,\"simCheckpointInterval\":1,\"nodes\":[]}")};
    bool ok = simulation_tool_reply(&placed, stage) == SIM_REGENERATE &&
        cJSON_GetArraySize(placed.tool_pending) == 9 && strstr(str(&placed.tool_feedback), "\"result\":\"TRUE\"") &&
        !at(placed.story, "simCheckpoints") && !at(placed.story, "simLog");
    int accepted = story_push(&ws, placed.story, 0, "assistant", "The box lands.");
    App app = {0};
    simulation_commit(&app, &placed, accepted);
    ok &= cJSON_GetArraySize(at(at(placed.story, "simCheckpoints"), "entries")) == 1;
    int sibling = story_push(&ws, placed.story, 0, "assistant", "The room stays empty.");
    char *serialized = cJSON_PrintUnformatted(placed.story);
    Preview replay = {.story = cJSON_Parse(serialized), .parent = accepted};
    Json *restored = simulation_state(replay.story, accepted);
    Json *replay_only = cJSON_Duplicate(replay.story, true);
    cJSON_DeleteItemFromObjectCaseSensitive(replay_only, "simCheckpoints");
    Json *reconstructed = simulation_state(replay_only, accepted);
    const Json *physics = at(restored, "physics");
    ok &= cJSON_Compare(restored, reconstructed, true) &&
        cJSON_IsTrue(at(physics_scene_body(physics, "box"), "fractured")) &&
        rigid_value(physics_scene_body(physics, "rotor"), "rotation_rad") > 0;
    cJSON_Delete(restored);
    cJSON_Delete(reconstructed);
    cJSON_Delete(replay_only);
    ok &= simulation_tool_reply(&replay, inspect) == SIM_REGENERATE &&
        strstr(str(&replay.tool_feedback), "\"result\":\"TRUE\"");
    free_protocol(&replay);
    replay = (Preview){.story = cJSON_Parse(serialized), .parent = sibling};
    ok &= simulation_tool_reply(&replay, inspect) == SIM_REGENERATE &&
        strstr(str(&replay.tool_feedback), "\"result\":\"INSUFFICIENT_DATA\"");
    free_protocol(&replay);
    cJSON_free(serialized);
    cJSON_Delete(ws.root);
    free_protocol(&placed);
    return ok;
}

static bool checkpoint_order_and_siblings(void)
{
    App app = {.roleplay = true, .story = cJSON_Parse("{\"simCheckpointInterval\":1,\"nodes\":[]}")};
    int shared = story_push(&app.ws, app.story, 0, "assistant", "An empty room.");
    simulation_command(&app, "time", "set 1 00:00");
    int left = story_push(&app.ws, app.story, shared, "assistant", "The first branch.");
    simulation_command(&app, "time", "advance 10");
    int right = story_push(&app.ws, app.story, shared, "assistant", "The other branch.");
    simulation_command(&app, "time", "advance 20");
    integer(app.story, "head", shared);
    simulation_command(&app, "time", "set 1 01:00");
    integer(app.story, "head", left);
    simulation_command(&app, "time", "advance 1");
    int nested = story_push(&app.ws, app.story, left, "assistant", "A nested branch.");
    simulation_command(&app, "time", "advance 2");

    const int heads[] = {shared, left, right, nested};
    const int clocks[] = {60, 61, 60, 63};
    bool ok = cJSON_GetArraySize(at(at(app.story, "simCheckpoints"), "entries")) == 4;
    for (size_t i = 0; i < sizeof heads / sizeof *heads; ++i) {
        Json *cached = simulation_state(app.story, heads[i]);
        integer(app.story, "simCheckpointInterval", 0);
        Json *full = simulation_state(app.story, heads[i]);
        integer(app.story, "simCheckpointInterval", 1);
        ok &= cJSON_Compare(cached, full, true) && number(cached, "clock") == clocks[i];
        cJSON_Delete(full);
        cJSON_Delete(cached);
    }
    simulation_command(&app, "sim", "checkpoint off");
    ok &= !at(app.story, "simCheckpoints") && number(app.story, "simCheckpointInterval") == 0;
    release(&app.info);
    cJSON_Delete(app.story);
    return ok;
}

static bool cumulative_plan_budget(void)
{
    App app = {.roleplay = true, .story = cJSON_Parse("{\"simEnabled\":true,\"simCheckpointInterval\":1,\"nodes\":[]}")};
    simulation_command(&app, "sim", "budget rays 3");
    const char *plan =
        "{\"neutrino_tool\":\"sim_plan\",\"mode\":\"stage\",\"name\":\"two ray batches\",\"actions\":["
        "{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[4,4,4]},"
        "{\"neutrino_tool\":\"scene_rays\",\"origin\":[1,1,1],\"count\":2},"
        "{\"neutrino_tool\":\"scene_rays\",\"origin\":[2,2,2],\"count\":2}],\"queries\":[]}";
    Preview p = {.story = app.story};
    bool ok = simulation_tool_reply(&p, plan) == SIM_REGENERATE &&
        strstr(str(&p.tool_feedback), "budget exceeded (rays): 4 requested, 3 allowed") &&
        !p.tool_pending && !at(app.story, "simLog") && !at(app.story, "simCheckpoints");
    p.story = nullptr;
    free_protocol(&p);
    simulation_command(&app, "sim", "budget ram_bytes 1024");
    const char *soft =
        "{\"neutrino_tool\":\"softbody3d\",\"size\":[1,1,1],\"mass_kg\":1,\"stiffness\":1,"
        "\"nodes\":100000,\"seconds\":0.01}";
    p = (Preview){.story = app.story};
    ok &= simulation_tool_reply(&p, soft) == SIM_REGENERATE &&
        strstr(str(&p.tool_feedback), "budget exceeded (ram_bytes): 7200000 requested, 1024 allowed");
    p.story = nullptr;
    free_protocol(&p);
    simulation_command(&app, "sim", "budget reset");
    p = (Preview){.story = app.story};
    ok &= !at(app.story, "simBudget") && simulation_tool_reply(&p, plan) == SIM_REGENERATE &&
        cJSON_GetArraySize(p.tool_pending) == 1 &&
        strstr(str(&p.tool_feedback), "\"pending_commit\":true") && !at(app.story, "simCheckpoints");
    free_protocol(&p);
    release(&app.info);
    return ok;
}

static bool character_knowledge_isolation(void)
{
    App a = {.roleplay = true, .story = cJSON_Parse("{\"nodes\":[],\"simEnabled\":true,"
        "\"card\":{\"description\":\"PRIVATE_CARD\"},\"authorNote\":\"PRIVATE_NOTE\"}")};
    char room[] = "Relay | PRIVATE_ROOM_PROP";
    char mara[] = "Mara | PRIVATE_NPC_PROFILE";
    char tessa[] = "Tessa | A second character.";
    world_command(&a, "room", room);
    world_command(&a, "npc", mara);
    world_command(&a, "npc", tessa);
    int common = story_push(&a.ws, a.story, 0, "assistant", "PRIVATE_OBJECTIVE_RECAP");
    char private_tessa[] = "Tessa | PRIVATE_TESSA_BELIEF";
    world_command(&a, "knowledge", private_tessa);
    int tessa_turn = story_push(&a.ws, a.story, common, "user", "PRIVATE_TESSA_SPEECH");
    set(story_pick(a.story, tessa_turn), "addressed", "Tessa");
    int sibling = story_push(&a.ws, a.story, tessa_turn, "assistant", "A different outcome.");
    char other_branch[] = "Mara | PRIVATE_SIBLING_BELIEF";
    world_command(&a, "knowledge", other_branch);
    int mara_cue = story_push(&a.ws, a.story, tessa_turn, "user", "AUTHORIZED_MARA_CUE");
    set(story_pick(a.story, mara_cue), "addressed", "Mara");
    char shared_mara[] = "Mara | AUTHORIZED_MARA_HEARSAY";
    world_command(&a, "knowledge", shared_mara);
    Preview p = {.story = a.story, .parent = mara_cue};
    append(&p.speaker, "Mara");
    append(&p.world_context, "PRIVATE_GEOMETRY");
    append(&p.tool_feedback, "PRIVATE_ACTUAL_RESULT");
    append(&p.messages_context, "PRIVATE_LORE");
    Json *messages = cJSON_CreateArray();
    world_character_messages(&p, messages);
    char *payload = cJSON_PrintUnformatted(messages);
    bool ok = sibling > 0 && payload && strstr(payload, "AUTHORIZED_MARA_CUE") &&
        strstr(payload, "AUTHORIZED_MARA_HEARSAY") && !strstr(payload, "PRIVATE_") &&
        simulation_tool_reply(&p, "{\"neutrino_tool\":\"scene_catalog\"}") == SIM_NO_PROSE &&
        !p.tool_requests && !p.tool_pending;
    cJSON_free(payload);
    cJSON_Delete(messages);
    release(&p.speaker);
    release(&p.world_context);
    release(&p.messages_context);
    release(&a.info);
    free_protocol(&p);
    return ok;
}

static bool plan_cfd_reuse(void)
{
    const char *plan =
        "{\"neutrino_tool\":\"sim_plan\",\"mode\":\"hypothetical\",\"name\":\"fog field\",\"actions\":["
        "{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[3,3,3]},"
        "{\"neutrino_tool\":\"scene_medium\",\"method\":\"cfd\",\"resolution\":6,\"seconds\":0,"
        "\"center\":[1.75,1.75,1.75],\"light_loss\":2},"
        "{\"neutrino_tool\":\"scene_flow\",\"probe\":[1.75,1.75,1.75]},"
        "{\"neutrino_tool\":\"scene_light\",\"source\":[0.5,1.75,1.75],\"listener\":[2.5,1.75,1.75]},"
        "{\"neutrino_tool\":\"scene_medium\",\"method\":\"cfd\",\"resolution\":6,\"seconds\":0,"
        "\"center\":[1.75,1.75,1.75],\"concentration\":0.3},"
        "{\"neutrino_tool\":\"scene_flow\",\"probe\":[1.75,1.75,1.75]}],"
        "\"queries\":[{\"id\":\"before\",\"predicate\":\"fluid.concentration_exceeds\",\"step\":3,\"threshold\":0.5},"
        "{\"id\":\"after\",\"predicate\":\"fluid.concentration_exceeds\",\"step\":6,\"threshold\":0.5}]}";
    Preview p = {.story = cJSON_Parse("{\"nodes\":[],\"simEnabled\":true}")};
    const bool regenerated = simulation_tool_reply(&p, plan) == SIM_REGENERATE;
    const char *reuse = strstr(str(&p.tool_results), "Reused CFD field.");
    const char *once = reuse ? strstr(reuse + 1, "Reused CFD field.") : nullptr;
    Json *report = cJSON_Parse(str(&p.tool_feedback));
    const Json *results = at(report, "consequences");
    bool ok = regenerated && reuse && !once && !p.tool_pending && !at(p.story, "simLog") &&
        !strcmp(js(cJSON_GetArrayItem(results, 0), "result"), "TRUE") &&
        !strcmp(js(cJSON_GetArrayItem(results, 1), "result"), "FALSE");
    cJSON_Delete(report);
    free_protocol(&p);
    return ok;
}

static bool persisted_cfd(void)
{
    App a = {.roleplay = true, .story = cJSON_Parse("{\"nodes\":[],\"simEnabled\":true,\"simCheckpointInterval\":1}")};
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_room\",\"name\":\"Hall\",\"min\":[0,0,0],\"max\":[6,6,6]}");
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_medium\",\"method\":\"cfd\",\"resolution\":12,"
                       "\"seconds\":0.5,\"wind\":[1,0,0],\"center\":[2.75,2.75,2.75],\"diffusivity\":0,\"viscosity\":0,"
                       "\"light_loss\":8,\"sound_loss\":5}");
    int fork = story_push(&a.ws, a.story, 0, "assistant", "The tracer is ready.");
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_flow_step\",\"seconds\":0.25}");
    const char *experiment = "{\"neutrino_tool\":\"sim_plan\",\"mode\":\"hypothetical\",\"name\":\"second step\","
        "\"actions\":[{\"neutrino_tool\":\"scene_flow_step\",\"seconds\":0.25},"
        "{\"neutrino_tool\":\"scene_flow\",\"probe\":[3.75,2.75,2.75]}],"
        "\"queries\":[{\"id\":\"clock\",\"predicate\":\"flow.time_exceeds\",\"step\":2,\"threshold\":0.25}]}";
    Preview p = {.story = a.story, .parent = fork};
    bool ok = simulation_tool_reply(&p, experiment) == SIM_REGENERATE && !p.tool_pending &&
        strstr(str(&p.tool_feedback), "\"value\":0.5") && strstr(str(&p.tool_feedback), "\"result\":\"TRUE\"");
    p.story = nullptr;
    free_protocol(&p);
    p = (Preview){.story = a.story, .parent = fork};
    ok &= simulation_tool_reply(&p, "{\"neutrino_tool\":\"scene_flow_step\",\"seconds\":0.25}\n\nSpeculative prose.") ==
        SIM_REGENERATE && p.tool_prose_only && cJSON_GetArraySize(p.tool_pending) == 1 &&
        strstr(str(&p.tool_results), "Discarded speculative prose") &&
        !strstr(str(&p.tool_results), "Speculative prose.") && !strstr(str(&p.tool_feedback), "Speculative prose.");
    p.story = nullptr;
    free_protocol(&p);
    int left = story_push(&a.ws, a.story, fork, "assistant", "The tracer moves farther.");
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_flow_step\",\"seconds\":0.25}");
    int right = story_push(&a.ws, a.story, fork, "assistant", "Time pauses here.");
    char *serialized = cJSON_PrintUnformatted(a.story);
    Json *loaded = cJSON_Parse(serialized);
    cJSON_free(serialized);
    Json *probe = cJSON_Parse("{\"probe\":[3.75,2.75,2.75]}");
    Json *full = simulation_state(loaded, left), *half = simulation_state(loaded, right);
    PhysicsReading a_reading = {0}, b_reading = {0}, reference = {0};
    Text result = {0};
    ok &= physics_run_reading("scene_flow", probe, at(full, "physics"), &result, &a_reading) &&
        strstr(str(&result), "Restored CFD field at 0.5 s");
    clear(&result);
    ok &= physics_run_reading("scene_flow", probe, at(half, "physics"), &result, &b_reading) &&
        strstr(str(&result), "Restored CFD field at 0.25 s");
    Json *fresh = simulation_state(loaded, 0);
    clear(&result);
    ok &= physics_run_reading("scene_flow", probe, at(fresh, "physics"), &result, &reference) &&
        !reference.stored_flow && a_reading.stored_flow && b_reading.stored_flow &&
        a_reading.flow_time_s == .5 && b_reading.flow_time_s == .25 &&
        a_reading.tracer_concentration == reference.tracer_concentration &&
        a_reading.tracer_concentration != b_reading.tracer_concentration;
    Json *signal = cJSON_Parse("{\"source\":[3.75,2.75,2.75],\"listener\":[4.75,2.75,2.75],\"max_bounces\":0}");
    ok &= physics_run_reading("scene_light", signal, at(full, "physics"), &result, &a_reading) &&
        physics_run_reading("scene_light", signal, at(half, "physics"), &result, &b_reading) &&
        a_reading.optical_power > 0 && a_reading.optical_power < b_reading.optical_power;
    ok &= physics_run_reading("scene_acoustic", signal, at(full, "physics"), &result, &a_reading) &&
        physics_run_reading("scene_acoustic", signal, at(half, "physics"), &result, &b_reading) &&
        a_reading.arrivals == 1 && b_reading.arrivals == 1 &&
        !a_reading.arrival[0].blocked && a_reading.arrival[0].level_db < b_reading.arrival[0].level_db;
    cJSON_Delete(signal);

    integer(loaded, "simCheckpointInterval", 0);
    Json *replayed = simulation_state(loaded, left);
    ok &= cJSON_Compare(full, replayed, true);
    simulation_command(&a, "sim", "budget field_bytes 0");
    const int before = cJSON_GetArraySize(at(a.story, "simLog"));
    release(&a.info);
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_flow_step\",\"seconds\":0.25}");
    ok &= cJSON_GetArraySize(at(a.story, "simLog")) == before &&
        strstr(str(&a.info), "budget exceeded (field_bytes)");
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"_flow_frame\",\"layout\":\"Hall\",\"frame\":{}}");
    ok &= cJSON_GetArraySize(at(a.story, "simLog")) == before;
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_flow_reset\"}");
    Json *reset = simulation_state(a.story, right);
    ok &= physics_run_reading("scene_flow", probe, at(reset, "physics"), &result, &b_reading) && !b_reading.stored_flow;
    integer(a.story, "head", left);
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_room\",\"name\":\"Hall\",\"min\":[0,0,0],\"max\":[7,6,6]}");
    Json *resized = simulation_state(a.story, left);
    ok &= physics_run_reading("scene_flow", probe, at(resized, "physics"), &result, &b_reading) && !b_reading.stored_flow;
    cJSON_Delete(resized);

    const double dimensions[] = {6,6,6};
    Json *config = cJSON_Parse("{\"method\":\"cfd\",\"resolution\":7,\"seconds\":0.21,\"wind\":[1,0,0]}");
    FlowConfig native;
    ok &= flow_parse(config, dimensions, &native);
    FlowField original = flow_solve(&native);
    char *text = flow_encode(&original);
    ok &= text && strlen(text) == flow_frame_bytes(7);
    native.seconds = 0;
    FlowField restored = flow_resume_domain(&native, nullptr, 0, text ? text : "");
    ok &= restored.grid && original.grid && !memcmp(original.grid, restored.grid, 4 * 7 * 7 * 7 * sizeof(double));
    if (text) text[0] = '*';
    FlowField invalid = flow_resume_domain(&native, nullptr, 0, text ? text : "");
    ok &= !invalid.grid && invalid.fallback && strstr(invalid.fallback, "Invalid saved");
    free(text);
    flow_release(&invalid);
    flow_release(&restored);
    flow_release(&original);
    cJSON_Delete(config);
    cJSON_Delete(reset);
    cJSON_Delete(replayed);
    cJSON_Delete(fresh);
    cJSON_Delete(half);
    cJSON_Delete(full);
    cJSON_Delete(probe);
    cJSON_Delete(loaded);
    cJSON_Delete(a.story);
    release(&a.info);
    release(&result);
    return ok;
}

static const char relay_geometry[] =
    "[{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[10,6,4]},"
    "{\"neutrino_tool\":\"scene_place\",\"kind\":\"wall\",\"name\":\"Lower baffle\","
    "\"min\":[4.9,0,0],\"max\":[5.1,3.5,4]},"
    "{\"neutrino_tool\":\"scene_place\",\"kind\":\"wall\",\"name\":\"Upper baffle\","
    "\"min\":[4.9,4.5,0],\"max\":[5.1,6,4]}]";

static bool recursive_relay(const char *backend)
{
    Json *settings = cJSON_CreateObject();
    Json *args = cJSON_Parse("{\"source\":[2,2,2],\"listener\":[8,2,2],\"wall_reflectivity\":0.8,\"min_level_db\":-45}");
    set(args, "backend", backend);
    Text result = {0};
    bool ok = rigid_actions(settings, relay_geometry);
    for (int depth = 0; depth <= 4; ++depth) {
        integer(args, "max_bounces", depth);
        PhysicsReading sound, light;
        clear(&result);
        ok &= physics_run_reading("scene_acoustic", args, settings, &result, &sound) &&
            sound.arrival[0].blocked && !sound.arrival[0].faded && strstr(str(&result), "Direct blocked.");
        double earliest = INFINITY, db = 0;
        for (int i = 0; i < sound.arrivals; ++i) {
            if (sound.arrival[i].blocked || sound.arrival[i].delay_ms >= earliest) continue;
            earliest = sound.arrival[i].delay_ms;
            db = sound.arrival[i].level_db;
        }
        /* Unfolding X-min then Y-max puts the source at [-2,10,2]. */
        const double path = sqrt(164.0);
        const double ms = 1000 * path / sqrt(1.4 * 287.05 * 293.15);
        ok &= depth < 2 ? !isfinite(earliest) :
            fabs(earliest - ms) < 1e-9 && fabs(db - 20 * log10(.65 * .65 / path)) < 1e-9 &&
            sound.recursive_arrivals >= 2 && strstr(str(&result), "2-bounce echo");
        clear(&result);
        ok &= physics_run_reading("scene_light", args, settings, &result, &light) &&
            light.optical_direct_blocked && strstr(str(&result), "Direct blocked.");
        ok &= depth < 2 ? light.optical_power == 0 :
            light.optical_first_bounces == 2 && fabs(light.optical_first_ns - path / .299792458) < 1e-9 &&
            fabs(light.optical_power - .8 * .8 / 164) < 1e-10 && strstr(str(&result), "2-bounce reflection");
    }
    put(args, "min_power", cJSON_CreateNumber(.01));
    put(args, "min_level_db", cJSON_CreateNumber(-20));
    PhysicsReading cutoff;
    clear(&result);
    ok &= physics_run_reading("scene_light", args, settings, &result, &cutoff) &&
        !cutoff.recursive_arrivals && !cutoff.optical_power && strstr(str(&result), "stopped at cutoff");
    clear(&result);
    ok &= physics_run_reading("scene_acoustic", args, settings, &result, &cutoff) &&
        !cutoff.recursive_arrivals && strstr(str(&result), "stopped at cutoff");
    ok &= rigid_actions(settings,
        "[{\"neutrino_tool\":\"scene_material\",\"name\":\"Haze\",\"acoustic\":{\"loss_db_per_m\":1,\"reflectivity\":0},"
        "\"optical\":{\"loss_db_per_m\":1}},{\"neutrino_tool\":\"scene_air\",\"material_id\":\"Haze\"},"
        "{\"neutrino_tool\":\"scene_place\",\"kind\":\"object\",\"name\":\"Pane\",\"min\":[4.99,3.5,0],\"max\":[5.01,4.5,4],"
        "\"material\":{\"loss_db_per_m\":12,\"reflectivity\":0},"
        "\"optical\":{\"transmission\":0.5,\"loss_db_per_m\":12}}]");
    integer(args, "max_bounces", 2);
    put(args, "min_power", cJSON_CreateNumber(1e-10));
    put(args, "min_level_db", cJSON_CreateNumber(-60));
    const double path = sqrt(164.0), pane = .02 * path / 10;
    const double loss = path + 11 * pane;
    clear(&result);
    ok &= physics_run_reading("scene_light", args, settings, &result, &cutoff) &&
        fabs(cutoff.optical_power - .5 * .8 * .8 / 164 * pow(10, -loss / 10)) < 1e-10 &&
        strstr(str(&result), "1 material traversals");
    clear(&result);
    ok &= physics_run_reading("scene_acoustic", args, settings, &result, &cutoff) &&
        !cutoff.recursive_limited && strstr(str(&result), "1 material traversals") &&
        fabs(cutoff.arrival[7].level_db - (20 * log10(.65 * .65 / path) - loss)) < 1e-9;
    integer(args, "max_bounces", 5);
    ok &= !physics_run_reading("scene_acoustic", args, settings, &result, &cutoff);
    release(&result);
    cJSON_Delete(args);
    cJSON_Delete(settings);
    return ok;
}

static bool sensory_branch_state(void)
{
    App a = {.roleplay = true, .story = cJSON_Parse("{\"nodes\":[],\"simEnabled\":true,\"simCheckpointInterval\":1}")};
    char room[] = "Relay | The shuttered signal room.", mara[] = "Mara | Keeps watch.",
        iona[] = "Iona | Listens.", tessa[] = "Tessa | Takes down readings.";
    char quarters[] = "Quarters | Beyond the scene.", nova[] = "Nova | Off-duty watchkeeper.";
    world_command(&a, "room", room);
    world_command(&a, "room", quarters);
    world_command(&a, "npc", mara);
    world_command(&a, "npc", iona);
    world_command(&a, "npc", tessa);
    world_command(&a, "npc", nova);
    Text geometry = {0};
    int root = story_push(&a.ws, a.story, 0, "assistant", "The baffles are in place.");
    format(&geometry, "{\"actions\":%s}", relay_geometry);
    simulation_command(&a, "sim", str(&geometry));
    release(&geometry);
    release(&a.info);
    simulation_command(&a, "sim",
        "{\"neutrino_tool\":\"scene_sensor\",\"name\":\"Mara\",\"listener\":[8,2,2],\"min_level_db\":-45}");
    simulation_command(&a, "sim",
        "{\"neutrino_tool\":\"scene_sensor\",\"name\":\"Iona\",\"listener\":[8,2,2],\"min_level_db\":-45,\"min_power\":0.01}");
    simulation_command(&a, "sim",
        "{\"neutrino_tool\":\"scene_sensor\",\"name\":\"Tessa\",\"listener\":[8,2,2],\"min_level_db\":-20,\"min_power\":0.01}");
    simulation_command(&a, "sim",
        "{\"neutrino_tool\":\"scene_sensor\",\"name\":\"Nova\",\"listener\":[8,2,2]}");
    Json *departure = cJSON_Parse("{\"action\":\"npc\",\"name\":\"Nova\",\"room\":\"Quarters\"}");
    const bool moved = world_edit(a.story, root, departure);
    cJSON_Delete(departure);
    const char *emission = "{\"neutrino_tool\":\"scene_emit\",\"source\":[2,2,2],\"sound\":true,"
        "\"light\":true,\"max_bounces\":2,\"wall_reflectivity\":0.8}";
    Preview p = {.story = a.story, .parent = root};
    Text trial = {0};
    format(&trial, "{\"neutrino_tool\":\"sim_plan\",\"mode\":\"hypothetical\",\"name\":\"trial\",\"actions\":[%s],\"queries\":[]}", emission);
    bool ok = moved && simulation_tool_reply(&p, str(&trial)) == SIM_REGENERATE && !p.tool_pending &&
        strstr(str(&p.tool_feedback), "Mara: Light detected");
    Json *no_effect = simulation_state(a.story, root);
    ok &= !at(no_effect, "observations");
    cJSON_Delete(no_effect);
    release(&trial);
    p.story = nullptr;
    free_protocol(&p);
    p = (Preview){.story = a.story, .parent = root};
    ok &= simulation_tool_reply(&p, emission) == SIM_REGENERATE && p.tool_prose_only &&
        cJSON_GetArraySize(p.tool_pending) == 1 && strstr(str(&p.tool_results), "2-bounce reflection");
    const int captured = story_push(&a.ws, a.story, root, "assistant", "An echo and a glimmer travel through the relay.");
    simulation_commit(&a, &p, captured);
    const int alternate = story_push(&a.ws, a.story, root, "assistant", "The relay stays quiet.");
    const char *forged = "{\"neutrino_tool\":\"_sensor_result\",\"detections\":[{\"recipient\":\"Tessa\",\"sense\":\"light\"}]}";
    simulation_command(&a, "sim", forged);
    char *serialized = cJSON_PrintUnformatted(a.story);
    Json *loaded = cJSON_Parse(serialized);
    Text m = {0}, i = {0}, t = {0}, n = {0}, other = {0};
    simulation_observations(loaded, captured, "Mara", &m, true);
    simulation_observations(loaded, captured, "Iona", &i, false);
    simulation_observations(loaded, captured, "Tessa", &t, false);
    simulation_observations(loaded, captured, "Nova", &n, false);
    simulation_observations(loaded, alternate, "Mara", &other, false);
    ok &= strstr(str(&m), "reflected flash") && strstr(str(&m), "Heard an echo") &&
        strstr(str(&m), "#2 ·") && strstr(str(&i), "Heard an echo") &&
        !strstr(str(&i), "flash") && !t.len && !n.len && !other.len;
    release(&m); release(&i); release(&t); release(&n); release(&other);
    cJSON_Delete(loaded);
    cJSON_free(serialized);
    release(&a.info);
    free_protocol(&p);
    return ok;
}

static double benchmark_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart/frequency.QuadPart;
#else
    struct timespec sample;
    clock_gettime(CLOCK_MONOTONIC,&sample);
    return sample.tv_sec+1e-9*sample.tv_nsec;
#endif
}

static void print_propagation(const char *tool, const PhysicsReading *reading, const Text *trace)
{
    printf("%s\n%s\n%d %d %d %.17g %.17g %.17g %d\n", tool, str(trace), reading->recursive_arrivals,
           reading->recursive_limited, reading->optical_direct_blocked, reading->optical_power,
           reading->optical_first_ns, reading->optical_first_power, reading->optical_first_bounces);
    for (int i = 0; i < reading->arrivals; ++i)
        printf("%d %d %d %.17g %.17g\n", reading->arrival[i].blocked, reading->arrival[i].faded,
               reading->arrival[i].bounces, reading->arrival[i].delay_ms, reading->arrival[i].level_db);
}

static int propagation_timing(bool dense, bool proof)
{
    Json *settings = cJSON_CreateObject();
    Json *args = cJSON_Parse("{\"source\":[2,2,2],\"listener\":[8,2,2],\"wall_reflectivity\":0.8,\"backend\":\"cpu\"}");
    bool valid = rigid_actions(settings, relay_geometry);
    Text out = {0};
    if (dense) {
        valid &= rigid_actions(settings, "[{\"neutrino_tool\":\"scene_material\",\"name\":\"reflector\","
            "\"acoustic\":{\"reflectivity\":0.9,\"loss_db_per_m\":0},\"optical\":{\"reflectivity\":0.9}}]");
        for (int i = 0; valid && i < 12; ++i) {
            clear(&out);
            const double x = 1 + 1.25*(i%6), y = .25 + 3*(i/6);
            format(&out, "[{\"neutrino_tool\":\"scene_place\",\"name\":\"reflector%d\",\"kind\":\"object\","
                "\"material_id\":\"reflector\",\"min\":[%.2f,%.2f,0],\"max\":[%.2f,%.2f,0.5]}]",
                i,x,y,x+.5,y+.5);
            valid = rigid_actions(settings, str(&out));
        }
    }
    if (!proof) puts("signal,depth,repetition,batches,mean_us,received_recursive_paths,workers");
    for (int optical = 0; valid && optical < 2; ++optical) {
        const char *tool = optical ? "scene_light" : "scene_acoustic";
        for (int depth = dense ? 4 : 0; valid && depth <= 4; ++depth) {
            integer(args, "max_bounces", depth);
            for (int rep = 0; valid && rep <= (proof ? 0 : 5); ++rep) {
                PhysicsReading reading;
                const int batches = proof ? 1 : dense ? 400 : depth < 2 ? 4000 : 800;
                const double start = benchmark_seconds();
                for (int j = 0; valid && j < batches; ++j) {
                    clear(&out);
                    valid = physics_run_reading(tool, args, settings, &out, &reading) && !reading.unavailable;
                }
                const double end = benchmark_seconds();
                if (valid && proof) print_propagation(tool,&reading,&out);
                if (valid && rep) printf("%s,%d,%d,%d,%.6f,%d,%u\n", tool, depth, rep, batches,
                    1e6 * (end-start)/batches, reading.recursive_arrivals, reading.recursive_workers);
            }
        }
    }
    release(&out);
    cJSON_Delete(args);
    cJSON_Delete(settings);
    return !valid;
}

static int timing_node(Json *story, int parent)
{
    Json *nodes = at(story, "nodes"), *node = cJSON_CreateObject();
    int id = nodes->child ? number(nodes->child->prev, "id") + 1 : 1;
    integer(node, "id", id);
    integer(node, "parent", parent);
    set(node, "role", "assistant");
    set(node, "content", "");
    cJSON_AddItemToArray(nodes, node);
    return id;
}

static void timing_event(Json *story, int index, int head)
{
    Json *op = cJSON_CreateObject();
    set(op, "neutrino_tool", "advance");
    integer(op, "minutes", 1);
    integer(op, "anchor", head);
    integer(op, "simSequence", index);
    cJSON_AddItemToArray(at(story, "simLog"), op);
}

static int checkpoint_timing(void)
{
    const int sizes[] = {10, 100, 1000, 10000, 100000};
    puts("events,method,repetition,mean_ms,checkpoint_json_bytes");
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        const int count = sizes[i];
        App app = {.roleplay = true, .story = cJSON_Parse("{\"nodes\":[],\"simLog\":[],\"simCheckpointInterval\":1}")};
        int head = 0, expected_clock = 480;
        for (int j = 1; j < count; ++j) {
            if (head && j % 11 == 0) timing_event(app.story, j, timing_node(app.story, head / 2));
            else {
                head = timing_node(app.story, head);
                timing_event(app.story, j, head);
                ++expected_clock;
            }
        }
        head = timing_node(app.story, head);
        integer(app.story, "head", head);
        simulation_command(&app, "time", "advance 1");
        ++expected_clock;
        const int child = timing_node(app.story, head);
        const int other = timing_node(app.story, head);
        const Json *cache = at(app.story, "simCheckpoints");
        char *serialized = cJSON_PrintUnformatted(cache);
        size_t size = serialized ? strlen(serialized) : 0;
        cJSON_free(serialized);
        bool valid = size > 0;
        const int order[] = {1, 0, 2};
        for (int index = 0; valid && index < 3; ++index) {
            const int method = order[index];
            if (index == 1) {
                timing_event(app.story, count + 1, child);
                timing_event(app.story, count + 2, other);
            }
            integer(app.story, "simCheckpointInterval", method != 0);
            const char *names[] = {"full", "checkpoint", "checkpoint_plus_1"};
            const int batches = method ? 4000 : count >= 10000 ? 32 : 512;
            for (int sample = 0; valid && sample <= 5; ++sample) {
                clock_t start = clock();
                for (int j = 0; valid && j < batches; ++j) {
                    Json *state = simulation_state(app.story, method == 1 ? head : child);
                    valid = number(state, "clock") == expected_clock + (method != 1);
                    cJSON_Delete(state);
                }
                clock_t finish = clock();
                if (valid && sample) printf("%d,%s,%d,%.6f,%zu\n", count, names[method], sample,
                    1000.0 * (finish - start) / (CLOCKS_PER_SEC * (double)batches), size);
            }
        }
        cJSON_Delete(app.story);
        if (!valid) return 1;
    }
    return 0;
}

static int cfd_reuse_timing(void)
{
    Json *settings = cJSON_CreateObject(), *probe = cJSON_Parse("{\"probe\":[2,2,2]}");
    bool valid = rigid_actions(settings,
        "[{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[6,6,6]},"
        "{\"neutrino_tool\":\"scene_medium\",\"method\":\"cfd\",\"seconds\":0.5,"
        "\"resolution\":24,\"wind\":[1,1,1]}]");
    PhysicsReading reference;
    Text result = {0};
    valid = valid && physics_run_reading("scene_flow", probe, settings, &result, &reference) &&
        reference.kind == READING_FLOW && !reference.unavailable;
    puts("method,repetition,mean_ms");
    for (int warm = 0; valid && warm <= 1; ++warm) {
        PhysicsCache cache = {0};
        for (int sample = 0; valid && sample <= 5; ++sample) {
            const int batches = warm ? 4000 : 8;
            const clock_t start = clock();
            for (int j = 0; valid && j < batches; ++j) {
                clear(&result);
                PhysicsReading reading;
                valid = (warm ? physics_run_cached("scene_flow", probe, settings, &result, &reading, &cache) :
                    physics_run_reading("scene_flow", probe, settings, &result, &reading)) &&
                    reading.kind == READING_FLOW && !reading.unavailable &&
                    reading.tracer_concentration == reference.tracer_concentration;
            }
            const clock_t end = clock();
            if (valid && sample) printf("%s,%d,%.6f\n", warm ? "reused" : "cold", sample,
                1000.0 * (end - start) / (CLOCKS_PER_SEC * (double)batches));
        }
        physics_cache_release(&cache);
    }
    release(&result);
    cJSON_Delete(probe);
    cJSON_Delete(settings);
    return !valid;
}

static int persistent_cfd_timing(void)
{
    const int sizes[] = {6, 12, 24};
    const double dimensions[] = {6,6,6}, point[] = {2,2,2};
    puts("resolution,method,repetition,batches,mean_ms,encoded_bytes");
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        Json *setup = cJSON_Parse("{\"method\":\"cfd\",\"seconds\":0.5,\"wind\":[1,1,1]}");
        integer(setup, "resolution", sizes[i]);
        FlowConfig config;
        bool valid = flow_parse(setup, dimensions, &config);
        cJSON_Delete(setup);
        if (!valid) return 1;
        FlowField reference = flow_solve(&config);
        char *encoded = flow_encode(&reference);
        const double expected = flow_sample(&reference, point);
        flow_release(&reference);
        if (!encoded) return 1;
        FlowConfig restore = config;
        restore.seconds = 0;
        for (int saved = 0; valid && saved <= 1; ++saved) {
            const int batches = saved ? 500 : sizes[i] == 24 ? 12 : 100;
            for (int rep = 0; valid && rep <= 5; ++rep) {
                const clock_t start = clock();
                for (int j = 0; valid && j < batches; ++j) {
                    FlowField field = saved ? flow_resume_domain(&restore, nullptr, 0, encoded) : flow_solve(&config);
                    valid = field.grid && flow_sample(&field, point) == expected;
                    flow_release(&field);
                }
                const clock_t end = clock();
                if (valid && rep) printf("%d,%s,%d,%d,%.6f,%zu\n", sizes[i], saved ? "restore" : "resimulate", rep,
                    batches, 1000.0 * (end - start) / (CLOCKS_PER_SEC * (double)batches), strlen(encoded));
            }
        }
        free(encoded);
        if (!valid) return 1;
    }
    return 0;
}

static bool attached_rigid_plan(void)
{
    Workspace ws = {.root = cJSON_CreateObject()};
    Preview p = {.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    const char *input = "Calculate a hypothetical experiment.\nSimulation setup: "
        "{\"neutrino_tool\":\"sim_plan\",\"name\":\"attached\",\"mode\":\"hypothetical\",\"actions\":["
        "{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[8,6,4]}],\"queries\":[]}";
    p.parent = story_push(&ws, p.story, 0, "user", input);
    bool ok = simulation_plan_selector(&p) &&
        simulation_tool_reply(&p, "{\"neutrino_tool\":\"tune\",\"setting\":\"gravity\",\"value\":1}") == SIM_REGENERATE &&
        simulation_plan_selector(&p) && !p.tool_pending &&
        simulation_tool_reply(&p, "{\"neutrino_tool\":\"sim_plan\"}") == SIM_REGENERATE &&
        !simulation_plan_selector(&p) && !p.tool_pending &&
        strstr(str(&p.tool_feedback), "\"simulation\":\"attached\"");
    cJSON_Delete(ws.root);
    free_protocol(&p);
    return ok;
}

static bool scenario_protocol(void)
{
    const char *experiment =
        "{\"neutrino_tool\":\"sim_plan\",\"name\":\"relay\",\"mode\":\"hypothetical\","
        "\"actions\":["
        "{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[10,4,4]},"
        "{\"neutrino_tool\":\"scene_material\",\"name\":\"pane\","
            "\"acoustic\":{\"loss_db_per_m\":20,\"reflectivity\":0},\"optical\":{\"transmission\":0.5}},"
        "{\"neutrino_tool\":\"scene_material\",\"name\":\"dense air\","
            "\"acoustic\":{\"loss_db_per_m\":6,\"reflectivity\":0},\"optical\":{\"loss_db_per_m\":100}},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"pane\",\"kind\":\"wall\","
            "\"min\":[2.5,1,1],\"max\":[3.5,3,3],\"material_id\":\"pane\"},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"bell\",\"kind\":\"object\","
            "\"min\":[0.9,1.9,1.9],\"max\":[1.1,2.1,2.1]},"
        "{\"neutrino_tool\":\"scene_acoustic\",\"source_entity\":\"bell\",\"listener\":[5,2,2],\"min_level_db\":-50},"
        "{\"neutrino_tool\":\"scene_light\",\"source_entity\":\"bell\",\"listener\":[5,2,2]},"
        "{\"neutrino_tool\":\"scene_air\",\"material_id\":\"dense air\"},"
        "{\"neutrino_tool\":\"scene_acoustic\",\"source_entity\":\"bell\",\"listener\":[5,2,2],\"min_level_db\":-50},"
        "{\"neutrino_tool\":\"scene_light\",\"source_entity\":\"bell\",\"listener\":[5,2,2]}],"
        "\"queries\":[{\"id\":\"beforeSound\",\"predicate\":\"signal.arrival_before\",\"step\":6,\"threshold\":12},"
        "{\"id\":\"beforeLight\",\"predicate\":\"signal.arrival_before\",\"step\":7,\"threshold\":0.000014},"
        "{\"id\":\"afterSound\",\"predicate\":\"signal.reaches_receiver\",\"step\":9},"
        "{\"id\":\"afterLight\",\"predicate\":\"signal.reaches_receiver\",\"step\":10},"
        "{\"id\":\"missing\",\"predicate\":\"entity.exists\",\"entity\":\"missing\"},"
        "{\"id\":\"unknown\",\"predicate\":\"thermal.phase_changed\"}]}";
    Preview p = {.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    bool ok = simulation_tool_reply(&p, experiment) == SIM_REGENERATE && !p.tool_pending &&
        !p.reading_count && p.tool_prose_only && !at(p.story, "simLog");
    Json *report = cJSON_Parse(str(&p.tool_feedback));
    const Json *results = at(report, "consequences");
    const char *const expected[] = {"TRUE", "TRUE", "FALSE", "FALSE", "FALSE", "UNSUPPORTED"};
    for (int i = 0; i < 6; ++i)
        ok &= !strcmp(js(cJSON_GetArrayItem(results, i), "result"), expected[i]);
    const Json *delay = at(cJSON_GetArrayItem(results, 0), "value");
    ok &= cJSON_IsNumber(delay) && fabs(delay->valuedouble - 1000 * 4 / sqrt(1.4 * 287.05 * 293.15)) < 1e-9;
    cJSON_Delete(report);
    free_protocol(&p);

    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    const char *stage =
        "{\"neutrino_tool\":\"sim_plan\",\"mode\":\"stage\",\"name\":\"gravity\",\"actions\":["
        "{\"neutrino_tool\":\"tune\",\"setting\":\"gravity\",\"value\":1},"
        "{\"neutrino_tool\":\"fluid\",\"height\":1,\"tank_area\":2,\"outlet_area\":0.003,\"seconds\":10}],"
        "\"queries\":[{\"id\":\"drain\",\"predicate\":\"fluid.volume_exceeds\",\"step\":2,\"threshold\":25}]}";
    ok &= simulation_tool_reply(&p, stage) == SIM_REGENERATE && cJSON_GetArraySize(p.tool_pending) == 1 &&
        p.reading_count == 1 && strstr(str(&p.tool_feedback), "\"pending_commit\":true") &&
        !at(p.story, "simLog");
    report = cJSON_Parse(str(&p.tool_feedback));
    const Json *volume = at(cJSON_GetArrayItem(at(report, "consequences"), 0), "value");
    double remaining = 1 - 0.62 * 0.003 * sqrt(2) * 10 / 4;
    ok &= cJSON_IsNumber(volume) && fabs(volume->valuedouble - 2000 * (1 - remaining * remaining)) < 1e-10;
    cJSON_Delete(report);
    Workspace ws = {.root = cJSON_CreateObject()};
    int id = story_push(&ws, p.story, 0, "assistant", "The water trickles out.");
    int other = story_push(&ws, p.story, 0, "assistant", "The water rushes out.");
    App a = {0};
    simulation_commit(&a, &p, id);
    const Json *log = at(p.story, "simLog");
    ok &= id > 0 && other != id && cJSON_GetArraySize(log) == 2;
    for (const Json *op = log ? log->child : nullptr; op; op = op->next)
        ok &= number(op, "anchor") == id;
    cJSON_Delete(ws.root);
    free_protocol(&p);

    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    ok &= simulation_tool_reply(&p,
        "{\"neutrino_tool\":\"sim_plan\",\"name\":\"failure\",\"mode\":\"stage\","
        "\"actions\":[{\"neutrino_tool\":\"tune\",\"setting\":\"gravity\",\"value\":1},"
        "{\"neutrino_tool\":\"scene_translate\",\"name\":\"missing\",\"offset\":[1,0,0]}],"
        "\"queries\":[{\"id\":\"missing\",\"predicate\":\"entity.exists\",\"entity\":\"missing\"}]}") == SIM_REGENERATE &&
        !p.tool_pending && strstr(str(&p.tool_feedback), "SIMULATION_FAILED");
    free_protocol(&p);

    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    ok &= simulation_tool_reply(&p,
        "{\"neutrino_tool\":\"sim_plan\",\"name\":\"condition\",\"mode\":\"stage\","
        "\"max_actions\":2,\"actions\":["
        "{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[8,8,8]},"
        "{\"neutrino_tool\":\"scene_remove\",\"name\":\"missing\","
        "\"when\":{\"predicate\":\"entity.exists\",\"entity\":\"missing\"}}],\"queries\":[]}") == SIM_REGENERATE &&
        cJSON_GetArraySize(p.tool_pending) == 1 && strstr(str(&p.tool_results), "action skipped");
    free_protocol(&p);

    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    ok &= simulation_tool_reply(&p,
        "{\"neutrino_tool\":\"sim_plan\",\"name\":\"budget\",\"mode\":\"stage\","
        "\"max_actions\":1,\"actions\":[{\"neutrino_tool\":\"tune\",\"setting\":\"gravity\",\"value\":1},"
        "{\"neutrino_tool\":\"tune\",\"setting\":\"gravity\",\"value\":2}],\"queries\":[]}") == SIM_REGENERATE &&
        !p.tool_pending && strstr(str(&p.tool_results), "action budget");
    free_protocol(&p);
    return ok;
}

static bool tool_protocol(void)
{
    const char *fluid = "{\"neutrino_tool\":\"fluid\",\"height\":1,\"tank_area\":2,\"outlet_area\":0.003,\"seconds\":10}";
    const char *acoustic = "{\"neutrino_tool\":\"acoustic\",\"min\":[0,0,0],\"max\":[6,5,4],"
                           "\"source\":[1,2,2],\"listener\":[4,2,2]}";
    const char *acoustic_again = "{\"listener\":[4,2,2], \"source\":[1,2,2], \"max\":[6,5,4], "
                                 "\"min\":[0,0,0], \"neutrino_tool\":\"acoustic\"}";
    const char *misnamed = "{\"fluid_tool\":\"fluid\",\"height\":1}";
    Preview p = {.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    bool ok = simulation_tool_reply(&p, fluid) == SIM_REGENERATE &&
              simulation_tool_reply(&p, acoustic) == SIM_REGENERATE &&
              strstr(str(&p.tool_feedback), "81.525 L") && strstr(str(&p.tool_feedback), "Direct 8.74 ms") &&
              !strstr(str(&p.tool_feedback), "\"neutrino_tool\"") &&
              simulation_tool_reply(&p, acoustic_again) == SIM_REGENERATE &&
              p.tool_prose_only && cJSON_GetArraySize(p.tool_requests) == 2 &&
              strstr(str(&p.tool_results), "Repeated tool request skipped") &&
              simulation_tool_reply(&p, "```json\n[1,2]\n```") == SIM_REGENERATE &&
              simulation_tool_reply(&p, misnamed) == SIM_NO_PROSE &&
              simulation_tool_reply(&p, "[She waits for the bell.]") == SIM_NARRATIVE &&
              simulation_tool_reply(&p, "She hears the bell echo against the walls.") == SIM_NARRATIVE;
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    ok = ok && simulation_tool_reply(&p, fluid) == SIM_REGENERATE &&
         simulation_tool_reply(&p, misnamed) == SIM_REGENERATE &&
         p.tool_prose_only && p.tool_rounds == 1 &&
         simulation_tool_reply(&p, "Water rushes down the drain.") == SIM_NARRATIVE;
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    ok = ok && simulation_tool_reply(&p, misnamed) == SIM_REGENERATE &&
         simulation_tool_reply(&p, fluid) == SIM_REGENERATE &&
         strstr(str(&p.tool_feedback), "81.525 L");
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    Text batch = {0};
    format(&batch, "[%s,%s,%s]", fluid, acoustic, acoustic_again);
    ok = ok && simulation_tool_reply(&p, str(&batch)) == SIM_REGENERATE &&
        p.tool_rounds == 2 && p.tool_prose_only && p.reading_count == 2 &&
        strstr(str(&p.tool_results), "Repeated tool request skipped inside the batch");
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    ok = ok && simulation_tool_reply(&p, str(&batch)) == SIM_REGENERATE &&
         simulation_tool_reply(&p, "Iona studies the [JsOn] output.") == SIM_REGENERATE &&
         strstr(str(&p.tool_results), "Discarded prose that broke character.") &&
         simulation_tool_reply(&p, "The neutrino_tool output showed a result.") == SIM_NO_PROSE &&
         simulation_tool_reply(&p, "Iona leans around the cabinet and listens.") == SIM_NARRATIVE;
    Text player_action = {0}, reference = {0};
    format(&player_action, "Iona watches the drain. %s Then she listens. [Move closer.]", str(&batch));
    simulation_player_reference(&reference, str(&player_action));
    ok = ok && !strcmp(str(&reference), "Iona watches the drain.  Then she listens. [Move closer.]");
    release(&player_action);
    release(&reference);
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    clear(&batch);
    format(&batch, "[%s,{\"neutrino_tool\":\"tune\",\"setting\":\"gravity\",\"value\":1}]", fluid);
    ok = ok && simulation_tool_reply(&p, str(&batch)) == SIM_REGENERATE &&
        p.reading_count == 0 && !p.tool_pending && strstr(str(&p.tool_results), "Malformed tool");
    free_protocol(&p);
    const char *const separators[] = {"\n\n", ","};
    for (int i = 0; i < 2; i++) {
        p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
        clear(&batch);
        format(&batch, "%s%s%s", acoustic, separators[i], fluid);
        ok = ok && simulation_tool_reply(&p, str(&batch)) == SIM_REGENERATE &&
             p.tool_rounds == 2 && p.tool_prose_only && p.reading_count == 2;
        free_protocol(&p);
    }
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    append(&batch, "\nThe instruments showed a result.");
    ok = ok && simulation_tool_reply(&p, str(&batch)) == SIM_REGENERATE &&
         p.reading_count == 0 && strstr(str(&p.tool_results), "Malformed tool");
    free_protocol(&p);
    release(&batch);
    return ok;
}

static bool ordered_protocol(void)
{
    const char *fluid = "{\"neutrino_tool\":\"fluid\",\"height\":1,\"tank_area\":2,\"outlet_area\":0.003,\"seconds\":10}";
    const char *gravity = "{\"neutrino_tool\":\"tune\",\"setting\":\"gravity\",\"value\":1}";
    const char *treat = "{\"neutrino_tool\":\"treat\",\"actor\":\"Iona\",\"part\":\"hand\",\"kind\":\"cut\"}";
    const char *injury = "{\"neutrino_tool\":\"injury\",\"actor\":\"Iona\",\"part\":\"hand\",\"kind\":\"cut\",\"severity\":\"mild\"}";
    Text plan = {0};
    Preview p = {.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    format(&plan, "{\"actions\":[%s,%s,%s]}", fluid, gravity, fluid);
    bool ok = simulation_tool_reply(&p, str(&plan)) == SIM_REGENERATE &&
        p.tool_prose_only && p.tool_rounds == 3 && p.reading_count == 2 &&
        p.readings[0].drained_l > p.readings[1].drained_l &&
        cJSON_GetArraySize(p.tool_pending) == 1 && !at(p.story, "simLog") &&
        !strstr(str(&p.tool_feedback), "neutrino_tool");
    free_protocol(&p);
    clear(&plan);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    format(&plan, "{\"actions\":[%s,%s,%s]}", gravity, treat, fluid);
    ok = ok && simulation_tool_reply(&p, str(&plan)) == SIM_REGENERATE &&
        !p.tool_pending && !p.reading_count && !p.tool_prose_only &&
        strstr(str(&p.tool_results), "rejected at step 2");
    clear(&plan);
    format(&plan, "{\"actions\":[%s,%s]}", injury, treat);
    ok = ok && simulation_tool_reply(&p, str(&plan)) == SIM_REGENERATE &&
        cJSON_GetArraySize(p.tool_pending) == 2 && !at(p.story, "simLog");
    free_protocol(&p);
    release(&plan);
    return ok;
}

static bool material_transport(void)
{
    const char *porous = "{\"method\":\"cfd\",\"resolution\":8,\"seconds\":1,\"viscosity\":0.01,\"diffusivity\":0,"
        "\"porous\":{\"porosity\":0.4,\"permeability_m2\":0.01,\"drive_m_s2\":[0.5,0,0]}}";
    Json *args = cJSON_Parse(porous);
    const double size[] = {2,2,2};
    FlowConfig config = {0};
    bool ok = flow_parse(args, size, &config);
    if (ok) {
        FlowField field = flow_solve(&config);
        ok &= field.grid && field.porous_cells == 512 && fabs(field.pore_volume - 3.2) < 1e-12 &&
            fabs(field.porous_speed - .5*(1-exp(-1))) < 1e-12 && field.divergence < 1e-12;
        flow_release(&field);
        config.porous_max[0] = .01;
        field = flow_solve(&config);
        ok &= !field.grid && field.fallback && strstr(field.fallback, "no fluid cells");
        flow_release(&field);
        config.porous_max[0] = 2;
        config.porous_max[1] = 1;
        config.porous_drive[0] = 0;
        config.wind[0] = .5;
        field = flow_solve(&config);
        ok &= field.grid && field.porous_cells == 256 && fabs(field.pore_volume - 1.6) < 1e-12 &&
            field.porous_speed < field.speed && field.speed < .5 && field.divergence < 1e-12;
        flow_release(&field);
    }
    set(args, "method", "reduced");
    ok &= !flow_parse(args, size, &config);
    cJSON_Delete(args);

    struct { const char *tool, *json; double final, transferred; } models[] = {
        {"gas_liquid_absorption","{\"gas_volume_m3\":2,\"liquid_volume_m3\":1,\"gas_kg_m3\":2,"
             "\"partition\":2,\"kla_s_inv\":0.25,\"seconds\":2}", 2*(1-exp(-1)), 2*(1-exp(-1))},
        {"gas_liquid_absorption","{\"gas_volume_m3\":1,\"liquid_volume_m3\":1,\"gas_kg_m3\":0,"
             "\"liquid_kg_m3\":1,\"partition\":0,\"kla_s_inv\":1,\"seconds\":1}", exp(-1), exp(-1)-1},
        {"vapor_sorption","{\"dry_mass_kg\":2,\"relative_humidity\":0.5,\"capacity_kg_per_kg\":0.2,"
             "\"affinity\":2,\"rate_s_inv\":0.5,\"seconds\":2}", .2*(1-exp(-1)), .2*(1-exp(-1))},
        {"vapor_sorption","{\"dry_mass_kg\":2,\"relative_humidity\":0,\"capacity_kg_per_kg\":0.2,"
             "\"initial_kg_per_kg\":0.1,\"affinity\":2,\"rate_s_inv\":1,\"seconds\":1}", .2*exp(-1), .2*(exp(-1)-1)},
        {"polymer_moisture_uptake","{\"dry_mass_kg\":2,\"thickness_m\":0.002,\"diffusivity_m2_s\":1e-10,"
             "\"surface_kg_per_kg\":0.05,\"seconds\":10000}", .0931259678463, .0931259678463},
        {"polymer_moisture_uptake","{\"dry_mass_kg\":2,\"thickness_m\":0.002,\"diffusivity_m2_s\":1e-10,"
             "\"surface_kg_per_kg\":0.05,\"seconds\":1}", .0011283791671, .0011283791671},
        {"polymer_moisture_uptake","{\"dry_mass_kg\":2,\"thickness_m\":0.002,\"diffusivity_m2_s\":1e-10,"
             "\"surface_kg_per_kg\":0.05,\"seconds\":0}", 0, 0},
    };
    for (size_t i = 0; i < sizeof models/sizeof *models; ++i) {
        MassTransfer mass = {0};
        args = cJSON_Parse(models[i].json);
        bool valid = mass_transfer_run(models[i].tool, args, &mass) &&
            fabs(mass.final_kg-models[i].final) < 1e-8 && fabs(mass.transferred_kg-models[i].transferred) < 1e-8 &&
            (!mass.closed || fabs(mass.gas_kg+mass.final_kg-(i == 0 ? 4 : 1)) < 1e-12);
        if (!valid) fprintf(stderr, "%s reference: got %.10g kg, expected %.10g kg.\n",
                            models[i].tool, mass.final_kg, models[i].final);
        ok &= valid;
        cJSON_Delete(args);
    }
    return ok;
}

static bool solid_cfd(void)
{
    FlowConfig config = {.length={8,8,8},.center={4.5,4.5,4.5},.sigma=.3,.density=1,
                         .diffusivity=.1,.resolution=8,.cfd=true};
    PhysicsBox walls[] = {{{2,0,0},{3,8,8}},{{6,0,0},{7,8,8}}};
    FlowField initial = flow_solve_domain(&config, walls, 2);
    config.seconds = 10;
    FlowField closed = flow_solve_domain(&config, walls, 2), open = flow_solve(&config);
    double solid_point[] = {6.5,4.5,4.5}, isolated[] = {7.5,4.5,4.5};
    bool ok = initial.grid && closed.grid && closed.solid && open.grid &&
        flow_sample(&closed, solid_point) == 0 && flow_sample(&closed, isolated) < 1e-8 &&
        flow_sample(&open, isolated) > 1e-4 && fabs(closed.tracer_mean - initial.tracer_mean) < 1e-12 &&
        closed.divergence == 0 && closed.speed == 0;
    if (!ok) fprintf(stderr, "Voxel CFD open %.9g, isolated %.9g, mass delta %.9g\n",
        flow_sample(&open, isolated), flow_sample(&closed, isolated), closed.tracer_mean-initial.tracer_mean);
    flow_release(&initial);
    flow_release(&closed);
    flow_release(&open);
    return ok;
}

static bool geometry_aware_flow_frames(void)
{
    App a = {.roleplay = true, .story = cJSON_Parse("{\"nodes\":[],\"simCheckpointInterval\":1}")};
    simulation_command(&a, "sim", "{\"actions\":["
        "{\"neutrino_tool\":\"scene_room\",\"name\":\"Hall\",\"min\":[0,0,0],\"max\":[8,8,8]},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"Baffle\",\"kind\":\"wall\",\"min\":[2.1,0,0],\"max\":[2.2,8,8]},"
        "{\"neutrino_tool\":\"scene_medium\",\"method\":\"cfd\",\"resolution\":8,\"seconds\":0,"
        "\"solid_geometry\":true,\"raster\":\"conservative\",\"boundary\":\"closed\",\"center\":[4.5,4.5,4.5]},"
        "{\"neutrino_tool\":\"scene_flow_step\",\"seconds\":0.25}]}");
    int fork = story_push(&a.ws, a.story, 0, "assistant", "The baffle separates the fog.");
    int nearby = story_push(&a.ws, a.story, fork, "assistant", "The baffle shifts slightly.");
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_translate\",\"name\":\"Baffle\",\"offset\":[0.3,0,0]}");
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_room\",\"name\":\"Hall\",\"min\":[0,0,0],\"max\":[8,8,8]}");
    int moved = story_push(&a.ws, a.story, fork, "assistant", "The baffle slides to the next cell.");
    simulation_command(&a, "sim", "{\"neutrino_tool\":\"scene_translate\",\"name\":\"Baffle\",\"offset\":[1,0,0]}");
    char *snapshot = cJSON_PrintUnformatted(a.story);
    Json *loaded = cJSON_Parse(snapshot);
    cJSON_free(snapshot);
    Json *probe = cJSON_Parse("{\"probe\":[2.5,4.5,4.5]}");
    const int heads[] = {fork, nearby, moved};
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        Json *state = simulation_state(loaded, heads[i]);
        PhysicsReading reading = {0};
        Text out = {0};
        ok &= physics_run_reading("scene_flow", probe, at(state, "physics"), &out, &reading) &&
            !reading.unavailable && reading.closed_flow && !reading.unresolved_obstacles && reading.stored_flow == (i < 2) &&
            (i < 2 ? reading.flow_time_s == 0.25 && reading.tracer_concentration == 0 :
                     reading.tracer_concentration > 0);
        release(&out);
        cJSON_Delete(state);
    }
    cJSON_Delete(probe);
    cJSON_Delete(loaded);
    cJSON_Delete(a.story);
    release(&a.info);

    const PhysicsBox boxes[] = {{{2.1,0,0},{2.2,8,8}},{{6,0,0},{7,8,8}}};
    FlowConfig c = {.length={8,8,8}, .center={4.5,4.5,4.5}, .sigma=.3, .density=1, .resolution=8,
                    .cfd=true, .seconds=10, .diffusivity=.1};
    const double thin_wall[] = {2.5,4.5,4.5}, behind[] = {1.5,4.5,4.5};
    FlowField center = flow_solve_domain(&c, boxes, 2);
    c.conservative = true;
    FlowField conservative = flow_solve_domain(&c, boxes, 2);
    ok &= center.grid && conservative.grid && center.unresolved_boxes == 1 && conservative.unresolved_boxes == 0 &&
        flow_sample(&center, thin_wall) > 0 && flow_sample(&conservative, thin_wall) == 0 &&
        flow_sample(&center, behind) > 1e-4 && flow_sample(&conservative, behind) < 1e-12;
    flow_release(&center);
    flow_release(&conservative);
    return ok;
}

static bool closed_cfd(void)
{
    const double dimensions[] = {8,8,8}, beyond[] = {7.5,4.5,4.5};
    Json *args = cJSON_Parse("{\"method\":\"cfd\",\"resolution\":8,\"seconds\":2,\"center\":[0.5,4.5,4.5],"
                            "\"diffusivity\":0.1,\"boundary\":\"closed\",\"backend\":\"gpu\"}");
    FlowConfig closed;
    if (!flow_parse(args, dimensions, &closed)) { cJSON_Delete(args); return false; }
    cJSON_Delete(args);
    FlowConfig initial = closed, periodic = closed;
    initial.seconds = 0;
    periodic.closed = periodic.gpu = false;
    FlowField start = flow_solve(&initial), sealed = flow_solve(&closed), wrapped = flow_solve(&periodic);
    bool ok = start.grid && sealed.grid && wrapped.grid &&
        flow_sample(&sealed, beyond) < 1e-15 && flow_sample(&wrapped, beyond) > 0.01 &&
        fabs(start.tracer_mean - sealed.tracer_mean) < 1e-14 &&
        sealed.fallback && strstr(sealed.fallback, "used CPU for closed exterior");
    flow_release(&wrapped);
    flow_release(&sealed);
    flow_release(&start);
    closed.gpu = false;
    closed.wind[0] = 1;
    initial = closed;
    initial.seconds = 0;
    start = flow_solve(&initial);
    sealed = flow_solve(&closed);
    const size_t cells = 8 * 8 * 8;
    ok &= start.grid && sealed.grid && sealed.divergence < start.divergence &&
        sealed.grid[cells] == 0 && sealed.grid[cells + 8 * (4 * 8 + 4)] == 0;
    flow_release(&sealed);
    flow_release(&start);
    return ok;
}

static const char ventilated_room[] =
    "{\"method\":\"cfd\",\"resolution\":8,\"seconds\":2,\"wind\":[0.5,0,0],"
    "\"sigma_m\":1000,\"concentration\":1,\"viscosity\":0.04,\"diffusivity\":0.01,\"boundary\":\"no_slip\","
    "\"boundaries\":{\"x_min\":{\"type\":\"inflow\",\"speed_m_s\":0.5,\"tracer\":0},"
    "\"x_max\":{\"type\":\"outflow\"}}}";

static bool ventilated_cfd(void)
{
    const double length[] = {4,3,2};
    Json *args = cJSON_Parse(ventilated_room);
    FlowConfig no_slip, free_slip;
    if (!flow_parse(args, length, &no_slip)) { cJSON_Delete(args); return false; }
    set(args, "boundary", "closed");
    if (!flow_parse(args, length, &free_slip)) { cJSON_Delete(args); return false; }
    FlowConfig initial = no_slip;
    initial.seconds = 0;
    FlowField start = flow_solve(&initial), vent = flow_solve(&no_slip), slip = flow_solve(&free_slip);
    bool ok = start.grid && vent.grid && slip.grid && vent.inflow_m3_s == 3 &&
        vent.outflow_m3_s == 3 && vent.tracer_in == 0 && vent.tracer_out > 5 &&
        fabs(24 * (start.tracer_mean - vent.tracer_mean) - vent.tracer_out) < 1e-10 &&
        vent.divergence < 0.1 && vent.grid[512 + (4*8)*8 + 4] < slip.grid[512 + (4*8)*8 + 4] &&
        vent.grid[512 + (4*8+4)*8] == .5 && vent.grid[2*512 + (4*8)*8 + 4] == 0;
    if (!ok && vent.grid && slip.grid)
        fprintf(stderr, "Ventilation m³/s %g/%g, tracer out %g, balance error %g, divergence %g, wall velocity %g/%g\n",
            vent.inflow_m3_s, vent.outflow_m3_s, vent.tracer_out,
            24 * (start.tracer_mean - vent.tracer_mean) - vent.tracer_out, vent.divergence,
            vent.grid[512 + (4*8)*8 + 4], slip.grid[512 + (4*8)*8 + 4]);
    flow_release(&start);
    flow_release(&vent);
    flow_release(&slip);

    put(args, "concentration", cJSON_CreateNumber(0));
    Json *inlet = at(at(args, "boundaries"), "x_min"), *exhaust = at(at(args, "boundaries"), "x_max");
    put(inlet, "tracer", cJSON_CreateNumber(1));
    put(inlet, "min_m", cJSON_Parse("[0.75,0.5]"));
    put(inlet, "max_m", cJSON_Parse("[2.25,1.5]"));
    put(exhaust, "min_m", cJSON_Parse("[0.75,0.5]"));
    put(exhaust, "max_m", cJSON_Parse("[2.25,1.5]"));
    ok &= flow_parse(args, length, &no_slip);
    vent = flow_solve(&no_slip);
    ok &= vent.grid && vent.inflow_m3_s == .75 && vent.outflow_m3_s == .75 &&
        fabs(vent.tracer_in - 1.5) < 1e-10 &&
        fabs(24 * vent.tracer_mean - (vent.tracer_in - vent.tracer_out)) < 1e-10;
    flow_release(&vent);
    const PhysicsBox partition[] = {{{1.5,0,0},{2,3,2}}};
    vent = flow_solve_domain(&no_slip, partition, 1);
    ok &= !vent.grid && vent.fallback && strstr(vent.fallback, "Disconnected CFD compartments");
    flow_release(&vent);
    put(exhaust, "speed_m_s", cJSON_CreateNumber(1));
    ok &= flow_parse(args, length, &no_slip);
    vent = flow_solve(&no_slip);
    ok &= !vent.grid && vent.fallback && strstr(vent.fallback, "cannot be balanced");
    flow_release(&vent);
    cJSON_DeleteItemFromObjectCaseSensitive(at(args, "boundaries"), "x_max");
    ok &= !flow_parse(args, length, &no_slip);
    cJSON_Delete(args);
    return ok;
}

static int ventilation_timing(void)
{
    const double length[] = {4,3,2};
    const int sizes[] = {8,12,24};
    puts("resolution,wall,repetition,batches,mean_ms,tracer_out,divergence,wall_adjacent_velocity_m_s");
    Json *args = cJSON_Parse(ventilated_room);
    integer(args, "seconds", 1);
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof sizes / sizeof *sizes; ++i) {
        integer(args, "resolution", sizes[i]);
        for (int slip = 0; ok && slip < 2; ++slip) {
            set(args, "boundary", slip ? "no_slip" : "closed");
            FlowConfig c;
            ok = flow_parse(args, length, &c);
            for (int rep = 0; ok && rep <= 5; ++rep) {
                const int batches = sizes[i] == 24 ? 4 : 20;
                double tracer = 0, divergence = 0, wall_velocity = 0;
                const clock_t start = clock();
                for (int j = 0; ok && j < batches; ++j) {
                    FlowField field = flow_solve(&c);
                    ok = field.grid && fabs(field.inflow_m3_s-3) < 1e-10 && fabs(field.outflow_m3_s-3) < 1e-10;
                    tracer = field.tracer_out;
                    divergence = field.divergence;
                    const int n = sizes[i];
                    if (ok) wall_velocity = field.grid[n*n*n + (n/2)*n*n + n/2];
                    flow_release(&field);
                }
                const clock_t end = clock();
                if (ok && rep) printf("%d,%s,%d,%d,%.6f,%.9g,%.9g,%.9g\n", sizes[i], slip ? "no-slip" : "free-slip",
                    rep,batches,1000.0*(end-start)/(CLOCKS_PER_SEC*(double)batches),tracer,divergence,wall_velocity);
            }
        }
    }
    cJSON_Delete(args);
    return !ok;
}

static bool pressure_cfd(void)
{
    const double length[] = {4,3,2}, near_probe[] = {3.75,1.6875,1.125}, far_probe[] = {.25,1.6875,1.125};
    Json *args = cJSON_Parse("{\"method\":\"cfd\",\"resolution\":8,\"seconds\":2,\"boundary\":\"closed\","
        "\"viscosity\":0,\"diffusivity\":0,\"concentration\":0,\"boundaries\":{"
        "\"x_min\":{\"type\":\"pressure\",\"pressure_pa\":0},"
        "\"x_max\":{\"type\":\"pressure\",\"pressure_pa\":0.12,\"backflow_tracer\":0.7}}}");
    FlowConfig c;
    if (!flow_parse(args, length, &c)) { cJSON_Delete(args); return false; }
    FlowField f = flow_solve(&c);
    bool ok = f.grid && f.inflow_m3_s > .27 && f.inflow_m3_s < .33 &&
        fabs(f.inflow_m3_s - f.outflow_m3_s) < .002 && f.inflow_m3_s == f.backflow_m3_s &&
        f.tracer_in > .18 && f.tracer_in == f.backflow_tracer_in &&
        fabs(f.tracer_in - f.tracer_out - 24*f.tracer_mean) < 1e-10 &&
        flow_sample(&f, near_probe) > .05 && flow_sample(&f, far_probe) < 1e-8;
    if (!ok) fprintf(stderr, "Pressure test: %s, in %.9g, out %.9g, backflow %.9g, tracer in %.9g, near %.9g\n",
        f.fallback ? f.fallback : "", f.inflow_m3_s, f.outflow_m3_s, f.backflow_m3_s, f.tracer_in, flow_sample(&f,near_probe));
    char *saved = flow_encode(&f);
    FlowConfig restored = c;
    restored.seconds = 0;
    FlowField resume = flow_resume_domain(&restored, nullptr, 0, saved ? saved : "");
    ok &= saved && strlen(saved) == flow_snapshot_bytes(&c) && resume.grid &&
        flow_sample(&resume, near_probe) == flow_sample(&f, near_probe) &&
        resume.backflow_m3_s == f.backflow_m3_s && resume.outflow_m3_s == f.outflow_m3_s;
    flow_release(&resume);
    restored.seconds = 1;
    resume = flow_resume_domain(&restored, nullptr, 0, saved ? saved : "");
    ok &= resume.grid && resume.backflow_m3_s > f.backflow_m3_s &&
        fabs(24*(resume.tracer_mean-f.tracer_mean) - (resume.tracer_in-resume.tracer_out)) < 1e-10;
    flow_release(&resume);
    free(saved);

    Json *boundaries = at(args, "boundaries"), *upper = at(boundaries,"x_max");
    put(at(boundaries,"x_min"), "pressure_pa", cJSON_CreateNumber(10));
    put(upper, "pressure_pa", cJSON_CreateNumber(10.12));
    ok &= flow_parse(args, length, &c);
    FlowField shifted = flow_solve(&c);
    ok &= shifted.grid && fabs(shifted.inflow_m3_s - f.inflow_m3_s) < 1e-12;
    flow_release(&shifted);
    flow_release(&f);

    set(upper, "type", "outflow");
    cJSON_DeleteItemFromObjectCaseSensitive(upper, "pressure_pa");
    cJSON_DeleteItemFromObjectCaseSensitive(upper, "backflow_tracer");
    ok &= !flow_parse(args, length, &c);
    cJSON_Delete(args);
    return ok;
}

static int pressure_timing(bool prescribed)
{
    const double length[] = {4,3,2};
    const int sizes[] = {8,12,24};
    puts("resolution,exhaust,repetition,batches,mean_ms,inlet_m3_s,outlet_m3_s,backflow_m3_s,tracer_out,divergence,workers");
    Json *args = cJSON_Parse(ventilated_room);
    integer(args, "seconds", 1);
    Json *exhaust = at(at(args, "boundaries"), "x_max");
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof sizes / sizeof *sizes; ++i) {
        if (prescribed && sizes[i] != 24) continue;
        integer(args, "resolution", sizes[i]);
        for (int pressure = 0; ok && pressure < (prescribed ? 1 : 2); ++pressure) {
            set(exhaust, "type", pressure ? "pressure" : "outflow");
            if (pressure) integer(exhaust, "pressure_pa", 0);
            else cJSON_DeleteItemFromObjectCaseSensitive(exhaust,"pressure_pa");
            FlowConfig c;
            ok = flow_parse(args, length, &c);
            for (int rep = 0; ok && rep <= 5; ++rep) {
                const int batches = sizes[i] == 24 ? 2 : 16;
                double tracer = 0, divergence = 0, in = 0, out = 0, backflow = 0;
                unsigned workers = 0;
                const double start = benchmark_seconds();
                for (int j = 0; ok && j < batches; ++j) {
                    FlowField f = flow_solve(&c);
                    ok = f.grid && fabs(f.inflow_m3_s - 3) < .1 && fabs(f.outflow_m3_s - 3) < .4;
                    if (!ok) fprintf(stderr, "Pressure benchmark n=%d: %s, in %.6g, out %.6g\n",
                                    sizes[i], f.fallback ? f.fallback : "", f.inflow_m3_s, f.outflow_m3_s);
                    tracer = f.tracer_out;
                    divergence = f.divergence;
                    in = f.inflow_m3_s; out = f.outflow_m3_s; backflow = f.backflow_m3_s;
                    workers = f.cpu_workers;
                    flow_release(&f);
                }
                const double elapsed = 1000.0*(benchmark_seconds()-start);
                if (ok && rep) printf("%d,%s,%d,%d,%.6f,%.9g,%.9g,%.9g,%.9g,%.9g,%u\n",
                    sizes[i], pressure ? "pressure" : "prescribed", rep,batches,
                    elapsed/batches,in,out,backflow,tracer,divergence,workers);
            }
        }
    }
    cJSON_Delete(args);
    return !ok;
}

static int scaling_proof(void)
{
    const double length[] = {4,3,2};
    Json *args = cJSON_Parse(ventilated_room);
    integer(args,"resolution",24);
    integer(args,"seconds",1);
    FlowConfig config;
    const bool valid = flow_parse(args,length,&config);
    cJSON_Delete(args);
    if (!valid) return 1;
    FlowField field = flow_solve(&config);
    char *state = flow_encode(&field);
    if (state) {
        printf("CFD %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n%s\n", field.inflow_m3_s,
               field.outflow_m3_s, field.tracer_in, field.tracer_out, field.tracer_mean,
               field.speed, field.divergence, state);
    }
    const bool saved = state != nullptr;
    free(state);
    flow_release(&field);
    return !saved || propagation_timing(true,true);
}

static void cell_scan_reference(const FlowConfig *c, const PhysicsBox *boxes, int count, unsigned char *solid)
{
    const int n = c->resolution;
    const double inv[] = {n/c->length[0], n/c->length[1], n/c->length[2]};
    for (int z = 0; z < n; ++z) for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const double point[] = {(x+.5)/inv[0], (y+.5)/inv[1], (z+.5)/inv[2]};
        bool occupied = false;
        for (int j = 0; j < count && !occupied; ++j) {
            occupied = true;
            for (int axis = 0; axis < 3; ++axis)
                occupied &= point[axis] >= boxes[j].lo[axis] && point[axis] < boxes[j].hi[axis];
        }
        solid[(z*n+y)*n+x] = occupied;
    }
}

static int raster_timing(void)
{
    PhysicsBox boxes[128];
    for (int i = 0; i < 128; ++i) {
        double x = 1 + (i % 8)*2.5, y = 1 + ((i/8) % 8)*2.5, z = 1 + (i/64)*5;
        boxes[i] = (PhysicsBox){{x,y,z},{x+1.1,y+1.1,z+1.1}};
    }
    FlowConfig c = {.length={24,24,24}, .resolution=24};
    unsigned char actual[FLOW_MAX_CELLS], expected[FLOW_MAX_CELLS];
    puts("boxes,method,repetition,batches,mean_us");
    for (int count = 8; count <= 128; count *= 4) {
        cell_scan_reference(&c, boxes, count, expected);
        flow_rasterize(&c, boxes, count, actual);
        if (memcmp(expected, actual, sizeof expected)) return 1;
        for (int raster = 0; raster < 2; ++raster) {
            const int batches = raster ? 10000 : 100;
            for (int rep = 0; rep <= 5; ++rep) {
                const clock_t start = clock();
                for (int j = 0; j < batches; ++j) {
                    if (raster) flow_rasterize(&c, boxes, count, actual);
                    else cell_scan_reference(&c, boxes, count, actual);
                    if (!actual[601]) return 1;
                }
                const clock_t end = clock();
                if (rep) printf("%d,%s,%d,%d,%.6f\n", count, raster ? "box-ranges" : "cell-scan",
                    rep, batches, 1e6 * (end - start) / (CLOCKS_PER_SEC * (double)batches));
            }
        }
    }
    return 0;
}

static int cfd_timing(void)
{
    const PhysicsBox walls[] = {{{2,0,0},{3,8,8}},{{6,0,0},{7,8,8}}};
    const int resolutions[] = {8, 16, 24};
    puts("resolution,obstacles,repetition,mean_ms,divergence_s_inv");
    for (size_t i = 0; i < sizeof resolutions / sizeof *resolutions; ++i) {
        FlowConfig config = {.length={8,8,8},.center={4.5,4.5,4.5},.sigma=.5,.density=1,
            .diffusivity=.02,.viscosity=.01,.seconds=2,.wind={.25,0,0},
            .vortex=.05,.resolution=resolutions[i],.cfd=true};
        for (int obstacles = 0; obstacles <= 2; obstacles += 2) {
            for (int sample = 0; sample <= 5; ++sample) {
                double divergence = 0;
                const int batches = 10;
                clock_t start = clock();
                for (int batch = 0; batch < batches; ++batch) {
                    FlowField field = flow_solve_domain(&config, walls, (size_t)obstacles);
                    bool valid = field.grid && (!obstacles || field.solid) && isfinite(field.divergence);
                    divergence = field.divergence;
                    flow_release(&field);
                    if (!valid) return 1;
                }
                clock_t end = clock();
                if (start == (clock_t)-1 || end == (clock_t)-1) return 1;
                if (sample) printf("%d,%d,%d,%.6f,%.9g\n", config.resolution, obstacles, sample,
                    1000.0 * (end - start) / (CLOCKS_PER_SEC * (double)batches), divergence);
            }
        }
    }
    return 0;
}

static bool scene_protocol(const char *backend)
{
    const char *room = "{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[8,5,4]}";
    const char *wall = "{\"neutrino_tool\":\"scene_place\",\"name\":\"Divider\",\"kind\":\"wall\","
                       "\"min\":[2.5,1,1],\"max\":[3.5,3,3]}";
    const char *prop = "{\"neutrino_tool\":\"scene_place\",\"name\":\"Shelf\",\"kind\":\"object\","
                       "\"min\":[0.5,1.5,3],\"max\":[1.5,2.5,3.5]}";
    const char *sound = "{\"neutrino_tool\":\"scene_acoustic\",\"source\":[1,2,2],\"listener\":[5,2,2],"
                        "\"backend\":\"cpu\"}";
    Preview p = {.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    Text plan = {0}, context = {0};
    format(&plan, "{\"actions\":[%s,%s,%s,{\"neutrino_tool\":\"scene_rays\",\"origin\":[1,2,2],"
           "\"directions\":[[1,0,0],[0,0,1],[0,0,-1]],\"backend\":\"%s\"}],\"continue\":true}",
           room, wall, prop, backend);
    bool ok = simulation_tool_reply(&p, str(&plan)) == SIM_REGENERATE &&
        !p.tool_prose_only && p.tool_rounds == 4 && !at(p.story, "simLog") &&
        strstr(str(&p.tool_results), "Divider 1.5 m; Shelf 1 m; room boundary 2 m;");
    simulation_prompt(&p, &context);
    ok = ok && strstr(str(&context), "8 tool operations left") &&
         strstr(str(&context), "3D room in metres") && strstr(str(&context), "Shelf");
    clear(&plan);
    format(&plan, "{\"actions\":[{\"neutrino_tool\":\"scene_remove\",\"name\":\"Divider\"},%s]}",
           "{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[1,1,1]}");
    ok = ok && simulation_tool_reply(&p, str(&plan)) == SIM_REGENERATE && !p.tool_prose_only &&
         strstr(str(&p.tool_results), "rejected at step 2");
    clear(&plan);
    format(&plan, "{\"actions\":[%s,{\"neutrino_tool\":\"scene_rays\","
           "\"origin\":[1,2,2],\"count\":4096,\"backend\":\"%s\"}]}", sound, backend);
    ok = ok && simulation_tool_reply(&p, str(&plan)) == SIM_REGENERATE &&
         p.tool_prose_only && p.reading_count == 1 &&
         strstr(str(&p.tool_results), "Direct blocked.") && strstr(str(&p.tool_results), "4096 rays") &&
         (strcmp(backend, "rt") || strstr(str(&p.tool_results), "Vulkan hardware RT")) &&
         cJSON_GetArraySize(p.tool_pending) == 3 &&
         simulation_tool_reply(&p, "Iona hears an echo around the divider.") == SIM_NARRATIVE;
    release(&context);
    release(&plan);
    free_protocol(&p);
    return ok;
}

#ifdef NEUTRINO_VULKAN_RT
static float reference_slab(const float *row)
{
    RAY_SLAB_BODY
}

static bool rt_parity(void)
{
    RtBox boxes[16] = {
        {{2, -1, -1}, {3, 1, 1}}, {{4, -1, -1}, {5, 2, 2}}, {{2, -1, -1}, {3, 1, 1}}
    };
    for (size_t i = 3; i < sizeof boxes / sizeof *boxes; ++i) {
        float x = (float)i + 4;
        boxes[i] = (RtBox){{x,-1,-1}, {x+0.5f,1,1}};
    }
    static RtRay rays[4096];
    static RtHit hits[4096];
    for (size_t i = 0; i < 4096; ++i) {
        rays[i] = (RtRay){{(i % 4 == 0) ? 2.5f : 0, (float)(i % 13) * .25f - 1.5f,
                           (float)(i % 17) * .125f - 1},
                          {(i % 6 == 0) ? -1 : 1, (i % 3 == 0) ? 0 : .125f, 0}};
    }
    rays[0] = (RtRay){{2.5f, 0, 0}, {0, 0, 0}};
    const size_t counts[] = {3,3,3,3,2,2,16,3};
    for (size_t pass = 0; pass < sizeof counts / sizeof *counts; ++pass) {
        if (pass == 2) boxes[0].lo[0] = 2.75f;
        if (pass == 3) boxes[1] = (RtBox){{1, -1.5f, -1}, {1.5f, 2, 2}};
        if (pass == 5) boxes[0].lo[0] = 2;
        size_t count = counts[pass];
        if (!ray_vulkan_rt(rays, 4096, boxes, count, hits)) return false;
        for (size_t i = 0; i < 4096; ++i) {
            int winner = -1;
            float expected = -1;
            for (size_t box = 0; box < count; ++box) {
                float row[RAY_VALUES];
                memcpy(row, &rays[i], sizeof rays[i]);
                memcpy(row + 6, &boxes[box], sizeof boxes[box]);
                float hit = reference_slab(row);
                if (hit >= 0 && (winner < 0 || hit < expected)) {
                    expected = hit;
                    winner = (int)box;
                }
            }
            if (hits[i].box != (float)winner || !(fabsf(hits[i].distance - expected) < 1e-5f)) {
                fprintf(stderr, "RT parity pass %zu ray %zu: expected box %d at %g, got box %g at %g\n",
                        pass, i, winner, expected, hits[i].box, hits[i].distance);
                return false;
            }
        }
    }
    return true;
}
#endif

static bool extended_models(void)
{
    const struct {const char *tool, *args, *expected;} cases[] = {
        {"elastic3d","{\"size\":[2,1,1],\"force\":[1000,0,0],\"young_pa\":1000000,\"poisson\":0.25}",
         "deformed size [2.002, 0.99975, 0.99975] m"},
        {"elastic3d","{\"size\":[1,1,1],\"force\":[1000,0,0],\"young_pa\":1}",nullptr},
        {"elastic3d","{\"model\":\"cantilever\",\"size\":[2,0.2,0.2],\"force\":[0,0,100],\"young_pa\":200000000000}",
         "tip displacement [0, 0, 1e-05] m"},
        {"softbody3d","{\"size\":[1,2,3],\"mass_kg\":1,\"stiffness\":100,\"seconds\":0}",
         "top displacement [0, 0, 0]"},
        {"softbody3d","{\"size\":[1,2,3],\"mass_kg\":1,\"stiffness\":10,\"resolution\":5,\"seconds\":0.001}",
         "125 mass nodes"},
        {"softbody3d","{\"size\":[1,2,3],\"mass_kg\":10,\"stiffness\":10,\"nodes\":100003,"
         "\"seconds\":0.001,\"sample_node\":100002}", "Sampled node 100002"},
        {"softbody3d","{\"size\":[1,2,3],\"mass_kg\":1,\"stiffness\":10,\"resolution\":[16,17,18],\"seconds\":0.001}",
         "4896 mass nodes"},
        {"softbody3d","{\"size\":[1,2,3],\"mass_kg\":1,\"stiffness\":10,\"nodes\":1000000000000,\"seconds\":0}",
         "1000000000000 mass nodes"},
        {"softbody3d","{\"size\":[1,2,3],\"mass_kg\":1,\"stiffness\":10,\"nodes\":125,\"resolution\":5,\"seconds\":0}",
         nullptr},
#ifdef _WIN32
        {"softbody3d","{\"size\":[1,2,3],\"mass_kg\":1,\"stiffness\":10,\"nodes\":4099,"
         "\"seconds\":0.001,\"memory\":\"mapped\"}", "4099 mass nodes, temporary file mapping"},
#endif
        {"trajectory3d","{\"origin\":[1,2,10],\"velocity\":[1,2,3],\"seconds\":1}",
         "position [2, 4, 8.096675] m"},
        {"thermal3d","{\"kelvin\":300,\"ambient_kelvin\":300,\"heat_capacity_j_k\":50,\"power_w\":250,\"seconds\":2}",
         "final 310 K, internal energy change 500 J"},
        {"thermal3d","{\"kelvin\":300,\"ambient_kelvin\":300,\"heat_capacity_j_k\":50,\"seconds\":1,"
         "\"size\":[1,1,1],\"probe\":[0,0,0],\"variation_k\":[5,10,15],\"thermal_diffusivity\":0}",
         "3D insulated conduction modes: probe 330 K"},
        {"gas","{\"size\":[6,6,6],\"method\":\"cfd\",\"seconds\":600,\"wind\":[25,25,25]}",
         "Fallback: CFD timestep budget exceeded."},
        {"fluid","{\"size\":[6,6,6],\"method\":\"navier_stokes\",\"seconds\":0}",
         "3D liquid flow: 3D incompressible Navier-Stokes, CPU"},
        {"flow3d","{\"size\":[6,6,6],\"seconds\":0}", "reduced Gaussian transport"},
        {"ignition3d","{\"source\":[1,2,3],\"site\":[1,2,3],\"pulse_ticks\":2,\"cool_ticks\":2}",
         "Ignition at 0.916291 ticks. Reset at"},
        {"ignition3d","{\"source\":[1,2,3],\"site\":[1,2,15],\"pulse_ticks\":10}",
         "Final state: inactive"}
    };
    Text result = {0};
    bool ok = true;
    for (size_t i=0; i<sizeof cases/sizeof *cases; ++i) {
        Json *args = cJSON_Parse(cases[i].args);
        bool ran = physics_run(cases[i].tool,args,nullptr,&result);
        if (ran != (cases[i].expected != nullptr) || (ran && !strstr(str(&result),cases[i].expected))) {
            fprintf(stderr,"%s extended case %zu: %s\n",cases[i].tool,i,str(&result));
            ok = false;
        }
        cJSON_Delete(args);
        clear(&result);
    }
    release(&result);
    return ok;
}

static bool narrator_world_control(void)
{
    App app = {0};
    Preview p = {.story=cJSON_Parse("{\"simEnabled\":true,\"head\":1,\"nodes\":["
        "{\"id\":1,\"parent\":0,\"role\":\"user\",\"content\":\"Look around.\"},"
        "{\"id\":2,\"parent\":0,\"role\":\"assistant\",\"content\":\"Another branch.\"},"
        "{\"id\":3,\"parent\":1,\"role\":\"assistant\",\"content\":\"A lantern gleams.\"}]}"),.parent=1};
    const char *plan = "{\"actions\":["
        "{\"neutrino_tool\":\"world\",\"action\":\"room\",\"name\":\"Cellar\",\"description\":\"Stone stairs.\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"room\",\"name\":\"Loft\",\"description\":\"Wood rafters.\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"connect\",\"name\":\"Cellar\",\"room\":\"Loft\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"move\",\"name\":\"Loft\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"npc\",\"name\":\"Mara\",\"description\":\"Keeper.\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"item\",\"name\":\"Scarf\",\"description\":\"Wool.\"},"
        "{\"neutrino_tool\":\"inventory\",\"action\":\"take\",\"name\":\"Scarf\"},"
        "{\"neutrino_tool\":\"inventory\",\"action\":\"wear\",\"name\":\"Scarf\"},"
        "{\"neutrino_tool\":\"set_time\",\"minutes\":60000000},"
        "{\"neutrino_tool\":\"auto_time\",\"minutes\":0},"
        "{\"neutrino_tool\":\"world_inspect\"}],\"continue\":true}";
    Text context = {0}, branch = {0};
    bool ok = simulation_tool_reply(&p,plan)==SIM_REGENERATE &&
        strstr(str(&p.tool_results),"Current room: Loft") && strstr(str(&p.tool_results),"Item: Scarf · Worn") &&
        !at(p.story,"world") && !at(p.story,"simLog");
    simulation_prompt(&p,&context);
    ok = ok && strstr(str(&context),"Staged world state") && strstr(str(&context),"Day 41667, 16:00") &&
         strstr(str(&context),"Present character: Mara");
    simulation_commit(&app,&p,3);
    world_context(p.story,3,&branch);
    ok = ok && strstr(str(&branch),"Room: Loft") && strstr(str(&branch),"Worn: Scarf");
    clear(&branch);
    world_context(p.story,2,&branch);
    ok = ok && !branch.len;
    release(&context);
    release(&branch);
    free_protocol(&p);
    return ok;
}

static bool party_control(void)
{
    Preview p = {.story=cJSON_Parse("{\"simEnabled\":true,\"nodes\":[{\"id\":1,\"parent\":0,"
        "\"role\":\"assistant\",\"content\":\"The party arrives.\"}]}")};
    App app = {0};
    const char *actions = "{\"actions\":["
        "{\"neutrino_tool\":\"world\",\"action\":\"room\",\"name\":\"Dock\",\"description\":\"Quay.\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"room\",\"name\":\"Loft\",\"description\":\"Rafters.\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"connect\",\"name\":\"Dock\",\"room\":\"Loft\"},"
        "{\"neutrino_tool\":\"character\",\"action\":\"create\",\"name\":\"Iona\",\"description\":\"Sailor.\"},"
        "{\"neutrino_tool\":\"character\",\"action\":\"create\",\"name\":\"Mara\",\"description\":\"Keeper.\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"item\",\"name\":\"Lantern\",\"description\":\"Brass.\"},"
        "{\"neutrino_tool\":\"inventory\",\"action\":\"take\",\"actor\":\"Iona\",\"name\":\"Lantern\"},"
        "{\"neutrino_tool\":\"inventory\",\"action\":\"give\",\"actor\":\"Iona\",\"name\":\"Lantern\",\"target\":\"Mara\"},"
        "{\"neutrino_tool\":\"world\",\"action\":\"move\",\"actor\":\"Iona\",\"name\":\"Loft\"},"
        "{\"neutrino_tool\":\"character\",\"action\":\"switch\",\"name\":\"Mara\"},"
        "{\"neutrino_tool\":\"world_inspect\"}]}";
    Text output = {0};
    bool ok = simulation_tool_reply(&p,actions)==SIM_REGENERATE &&
              strstr(str(&p.tool_results),"Active character: Mara") &&
              strstr(str(&p.tool_results),"Party member: Iona in Loft") &&
              strstr(str(&p.tool_results),"Item: Lantern · Carried by Mara");
    simulation_commit(&app,&p,1);
    integer(p.story,"head",1);
    Json *item = cJSON_Parse("{\"action\":\"take\",\"actor\":\"Iona\",\"name\":\"Lantern\"}");
    ok = ok && !world_inventory_tool(p.story,1,item);
    set(item,"action","give");
    set(item,"actor","Mara");
    set(item,"target","Iona");
    ok = ok && !world_inventory_tool(p.story,1,item);
    cJSON_Delete(item);
    world_context(p.story,1,&output);
    ok = ok && strstr(str(&output),"Active character: Mara") && strstr(str(&output),"Carried: Lantern");
    clear(&output);
    world_active_character(p.story,0,&output);
    ok = ok && !output.len;
    release(&output);
    free_protocol(&p);
    return ok;
}

static bool large_scene_state(const char *backend)
{
    App app = {0};
    Preview p = {.story=cJSON_Parse("{\"simEnabled\":true,\"nodes\":["
        "{\"id\":1,\"parent\":0,\"role\":\"assistant\",\"content\":\"The city opens its doors.\"}]}")};
    Json *settings = cJSON_CreateObject();
    Text plan = {0}, result = {0};
    append(&plan,"{\"actions\":[");
    char name[64];
    for (int i=0; i<145; ++i) {
        format(&plan,"%s{\"neutrino_tool\":\"world\",\"action\":\"room\",\"name\":\"Room %d\","
                     "\"description\":\"A room.\"}",i ? "," : "",i);
    }
    append(&plan,"]}");
    bool ok = simulation_tool_reply(&p,str(&plan))==SIM_REGENERATE && p.tool_prose_only &&
              cJSON_GetArraySize(p.tool_pending)==145;
    simulation_commit(&app,&p,1);
    Json *page = cJSON_Parse("{\"offset\":128}");
    ok = ok && world_inspect(p.story,1,page,&result) && strstr(str(&result),"Room 144");
    cJSON_Delete(page);
    free_protocol(&p);
    release(&plan);
    clear(&result);
    Json *room = cJSON_Parse("{\"neutrino_tool\":\"scene_room\",\"name\":\"Atrium\",\"min\":[0,0,0],\"max\":[100,10,10]}");
    ok = physics_scene_edit(settings,room) && ok;
    Json *edit = cJSON_Parse("{\"neutrino_tool\":\"scene_place\",\"kind\":\"object\",\"min\":[2,2,2],\"max\":[3,3,3]}");
    for (int i=0; i<40; ++i) {
        snprintf(name,sizeof name,"Prop %d",i);
        set(edit,"name",name);
        if (i==39) put(edit,"min",cJSON_Parse("[1.5,2,2]"));
        ok = physics_scene_edit(settings,edit) && ok;
    }
    set(room,"name","Balcony");
    ok = physics_scene_edit(settings,room) && ok;
    set(room,"neutrino_tool","scene_select");
    set(room,"name","Atrium");
    integer(room,"offset",32);
    ok = ok && physics_scene_edit(settings,room) && physics_run("scene_inspect",room,settings,&result) &&
         strstr(str(&result),"Prop 39") && strstr(str(&result),"\"object_count\":40");
    clear(&result);
    Json *ray = cJSON_Parse("{\"origin\":[1,2.5,2.5],\"directions\":[[1,0,0]],\"backend\":\"cpu\"}");
    set(ray,"backend",backend);
    ok = ok && physics_run("scene_rays",ray,settings,&result) && strstr(str(&result),"Prop 39 0.5 m");
    release(&result);
    cJSON_Delete(ray);
    cJSON_Delete(room);
    cJSON_Delete(edit);
    cJSON_Delete(settings);
    return ok;
}

static bool cfd_models(bool hardware)
{
    const double size[] = {6,6,6}, source[] = {2.75,2.75,2.75}, transported[] = {3.25,3.25,3.25};
    Json *args = cJSON_Parse("{\"method\":\"cfd\",\"resolution\":12,\"seconds\":0.5,\"wind\":[1,1,1],"
                            "\"center\":[2.75,2.75,2.75],\"diffusivity\":0}");
    FlowConfig config;
    if (!flow_parse(args,size,&config)) { cJSON_Delete(args); return false; }
    cJSON_Delete(args);
    FlowField field = flow_solve(&config);
    /* Six equal upwind interpolation steps per axis on this uniform-flow fixture. */
    const int binomial[] = {1,6,15,20,15,6,1};
    double one_axis = 0;
    for (int k=0; k<=6; ++k)
        one_axis += binomial[k]*pow(1.0/6,k)*pow(5.0/6,6-k)*exp(-.5*(k-1)*(k-1));
    bool ok = field.grid && field.divergence < 1e-8 && fabs(field.speed-sqrt(3)) < 1e-8 &&
              flow_sample(&field,transported) > flow_sample(&field,source) &&
              fabs(flow_sample(&field,transported)-one_axis*one_axis*one_axis) < 1e-8;
    if (!ok) fprintf(stderr,"Uniform CFD: divergence %g, speed %g, dye %g vs %g.\n",
                     field.divergence,field.speed,flow_sample(&field,transported),flow_sample(&field,source));
    flow_release(&field);
    config.vortex = .7;
    FlowField cpu = flow_solve(&config);
    ok = ok && cpu.grid && cpu.divergence < .01 && cpu.tracer_mean > 0;
    if (!ok) fprintf(stderr,"Vortex CFD: divergence %g, tracer mean %g.\n",cpu.divergence,cpu.tracer_mean);
    if (hardware) {
        config.gpu = true;
        FlowField gpu = flow_solve(&config);
        ok = ok && gpu.grid && !gpu.fallback && strstr(gpu.method,"OpenCL pressure") &&
             fabs(gpu.divergence-cpu.divergence) < 1e-5 &&
             fabs(flow_sample(&gpu,transported)-flow_sample(&cpu,transported)) < 1e-5;
        if (!ok) fprintf(stderr,"OpenCL CFD: %s, divergence %g.\n",gpu.method,gpu.divergence);
        flow_release(&gpu);
    }
#ifdef _WIN32
    config.gpu = false;
    config.mapped = true;
    FlowField mapped = flow_solve(&config);
    ok = ok && mapped.grid && mapped.mapped && flow_sample(&mapped,transported) == flow_sample(&cpu,transported);
    if (!ok) fprintf(stderr,"Mapped CFD: %s, tracer %g.\n",mapped.memory,flow_sample(&mapped,transported));
    flow_release(&mapped);
#endif
    flow_release(&cpu);
    return ok;
}

static bool optics_and_gas(void)
{
    Json *settings = cJSON_CreateObject();
    Json *room = cJSON_Parse("{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[6,5,4]}");
    Json *medium = cJSON_Parse("{\"neutrino_tool\":\"scene_medium\",\"method\":\"cfd\",\"seconds\":0,"
        "\"sigma_m\":1000,\"sound_loss\":2,\"light_loss\":2}");
    Json *query = cJSON_Parse("{\"source\":[1,2,2],\"listener\":[5,2,2],\"backend\":\"cpu\",\"wall_reflectivity\":0.4}");
    Json *glass = cJSON_Parse("{\"neutrino_tool\":\"scene_place\",\"name\":\"Glass\",\"kind\":\"wall\","
        "\"min\":[2.5,1,1],\"max\":[3.5,3,3],\"optical\":{\"transmission\":0.25,\"reflectivity\":0.5}}");
    Text result = {0};
    PhysicsReading reading;
    bool ok = physics_scene_edit(settings,room) && physics_scene_edit(settings,medium) &&
        physics_run_reading("scene_acoustic",query,settings,&result,&reading) &&
        fabs(reading.arrival[0].level_db - (-20*log10(4)-8)) < .001 &&
        strstr(str(&result),"Gas attenuation: 3D incompressible Navier-Stokes");
    clear(&result);
    ok = ok && physics_run("scene_light",query,settings,&result) &&
         strstr(str(&result),"Gas extinction: 3D incompressible Navier-Stokes") &&
         strstr(str(&result),"Y-min reflection");
    clear(&result);
    ok = ok && physics_scene_edit(settings,glass) && physics_run("scene_light",query,settings,&result);
    const char *direct = strstr(str(&result),"\nDirect ");
    double metres, nanos, db;
    ok = ok && direct && sscanf(direct,"\nDirect %lf m · %lf ns · %lf dB",&metres,&nanos,&db)==3 &&
         metres == 4 && fabs(nanos-4/.299792458) < .0001 &&
         fabs(db-(-20*log10(4)+10*log10(.25)-6)) < .001;
    release(&result);
    cJSON_Delete(glass);
    cJSON_Delete(query);
    cJSON_Delete(medium);
    cJSON_Delete(room);
    cJSON_Delete(settings);
    return ok;
}

static bool measurement_protocol(void)
{
    Preview p = {.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    bool ok = simulation_tool_reply(&p,
        "{\"neutrino_tool\":\"fluid\",\"height\":1,\"tank_area\":2,\"outlet_area\":0.003,\"seconds\":10}") == SIM_REGENERATE &&
        strstr(str(&p.tool_results), "Elapsed drainage interval: 10 seconds.") &&
        simulation_tool_reply(&p, "The simulation says it drained eight seconds.") == SIM_REGENERATE &&
        simulation_tool_reply(&p, "The tank drained for 8 seconds.") == SIM_NO_PROSE &&
        simulation_tool_reply(&p, "The tank drained 81.525 liters over ten seconds. The clock chimed eight seconds later.") == SIM_NARRATIVE &&
        !physics_prose_matches(p.readings, p.reading_count, "The tank drained 18 liters over ten seconds.") &&
        !physics_prose_matches(p.readings, p.reading_count, "The tank emptied in ten seconds.") &&
        !physics_prose_matches(p.readings, p.reading_count, "The simulation says the tank holds 0.95924 cubic meters.") &&
        !physics_prose_matches(p.readings, p.reading_count, "The water level is already 80% full.") &&
        !physics_prose_matches(p.readings, p.reading_count, "The water in the tank is thinning, the flow accelerating.") &&
        physics_prose_matches(p.readings, p.reading_count, "The tank holds about 1.91848 cubic meters.");
    Text prose = {0};
    append(&prose, "Iona listens to the water rushing away.");
    physics_prose_readouts(p.readings, p.reading_count, &prose);
    ok = ok && strstr(str(&prose), "81.525 liters over 10 seconds") &&
         physics_prose_matches(p.readings, p.reading_count, str(&prose));
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    ok = ok && simulation_tool_reply(&p,
         "{\"neutrino_tool\":\"acoustic\",\"min\":[0,0,0],\"max\":[6,5,4],\"source\":[1,2,2],\"listener\":[4,2,2]}") == SIM_REGENERATE &&
         !physics_prose_matches(p.readings, p.reading_count, "The direct sound was blocked.") &&
         !physics_prose_matches(p.readings, p.reading_count, "The first echo arrived at 20.39 ms.") &&
         !physics_prose_matches(p.readings, p.reading_count, "Iona saw the display flash 81.525 liters and ten seconds.") &&
         physics_prose_matches(p.readings, p.reading_count, "She remembered the earlier 81.525 liters.") &&
         !physics_prose_matches(p.readings, p.reading_count, "The direct sound arrives at 14.57 ms and the first echo at 8.74 ms.") &&
         physics_prose_matches(p.readings, p.reading_count, "The direct sound arrives at 8.74 ms and the first echo at 14.57 ms.");
    clear(&prose);
    append(&prose, "Iona hears the bell and a soft echo.");
    physics_prose_readouts(p.readings, p.reading_count, &prose);
    ok = ok && strstr(str(&prose), "first audible wall echo arrives at 14.57 milliseconds") &&
         physics_prose_matches(p.readings, p.reading_count, str(&prose));
    release(&prose);
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    ok = ok && simulation_tool_reply(&p,
        "{\"neutrino_tool\":\"fluid\",\"height\":1,\"tank_area\":2,\"outlet_area\":0.003,\"seconds\":10}") == SIM_REGENERATE &&
        simulation_tool_reply(&p,
        "{\"neutrino_tool\":\"acoustic\",\"min\":[0,0,0],\"max\":[10,8,4],\"source\":[2,2,2],\"listener\":[5,2,2],"
        "\"blockers\":[{\"min\":[3,1,1],\"max\":[4,3,3]}]}") == SIM_REGENERATE &&
        p.reading_count == 2 && strstr(str(&p.tool_results), "Direct blocked.") &&
        physics_prose_matches(p.readings, p.reading_count,
            "Over ten seconds the tank drained 81.525 liters, and the first echo arrived at 14.57 milliseconds.") &&
        physics_prose_matches(p.readings, p.reading_count,
            "The drain ran for ten seconds, and the first echo arrived after 0.01457 seconds.") &&
        !physics_prose_matches(p.readings, p.reading_count,
            "The tank drained 18 liters over ten seconds, and the first echo arrived at 14.57 milliseconds.") &&
        !physics_prose_matches(p.readings, p.reading_count,
            "The tank drained 81.525 liters over eight seconds, and the first echo arrived at 14.57 milliseconds.") &&
        !physics_prose_matches(p.readings, p.reading_count,
            "The drain ran for ten seconds, and the direct sound reached Iona.");
    free_protocol(&p);
    return ok;
}

static bool material_acoustics(void)
{
    const char *scene =
        "{\"backend\":\"cpu\",\"min\":[0,0,0],\"max\":[10,8,4],"
        "\"source\":[2,2,2],\"listener\":[8,2,2],"
        "\"medium\":{\"loss_db_per_m\":1,\"speed_m_s\":300},\"blockers\":["
        "{\"min\":[3,1,1],\"max\":[4,3,3],\"material\":{\"loss_db_per_m\":4,\"reflectivity\":0.6}},"
        "{\"min\":[5,1,1],\"max\":[5.5,3,3],\"material\":{\"loss_db_per_m\":8,\"reflectivity\":0}},"
        "{\"min\":[4,6,1],\"max\":[6,7,3],\"material\":{\"loss_db_per_m\":0,\"reflectivity\":0.5}}]}";
    Json *args = cJSON_Parse(scene);
    PhysicsReading reading;
    Text result = {0}, prose = {0};
    const double direct_level = -20 * log10(6) + 20 * log10(0.64) - 4.5 - 4 - 4;
    const double echo_length = 10;
    const double reflected_level = 20 * log10(0.5 / echo_length) - echo_length;
    bool ok = physics_run_reading("acoustic", args, nullptr, &result, &reading) &&
        reading.material_acoustics && reading.arrivals == ACOUSTIC_ROOM_PATHS + 1 &&
        !reading.arrival[0].blocked && fabs(reading.arrival[0].delay_ms - 20) < 1e-9 &&
        fabs(reading.arrival[0].level_db - direct_level) < 1e-9 &&
        fabs(reading.arrival[ACOUSTIC_ROOM_PATHS].level_db - reflected_level) < 1e-9 &&
        strstr(str(&result), "2 material traversals") && strstr(str(&result), "Object 3 Y-min echo");
    put(args, "min_level_db", cJSON_CreateNumber(-25));
    clear(&result);
    ok = ok && physics_run_reading("acoustic", args, nullptr, &result, &reading) &&
        reading.arrival[0].blocked && reading.arrival[0].faded &&
        strstr(str(&result), "Direct stopped (audibility 0).");
    physics_prose_readouts(&reading, 1, &prose);
    ok = ok && strstr(str(&prose), "below the modeled audibility cutoff");
    cJSON_Delete(args);
    args = cJSON_Parse("{\"min\":[0,0,0],\"max\":[5,5,5],\"source\":[1,2,2],\"listener\":[2,2,2],"
                       "\"medium\":{\"loss_db_per_m\":10},\"min_level_db\":-10}");
    clear(&result);
    ok = ok && physics_run_reading("acoustic", args, nullptr, &result, &reading) && reading.arrival[0].faded;
    put(at(args, "medium"), "loss_db_per_m", cJSON_CreateNumber(-1));
    ok = ok && !physics_run_reading("acoustic", args, nullptr, &result, &reading);
    cJSON_Delete(args);
    release(&result);
    release(&prose);
    return ok;
}

static bool supplied_acoustic_protocol(void)
{
    const char *acoustic = "{\"neutrino_tool\":\"acoustic\",\"min\":[0,0,0],\"max\":[10,8,4],"
                           "\"source\":[2,2,2],\"listener\":[5,2,2],\"medium\":{\"loss_db_per_m\":1},"
                           "\"blockers\":[{\"min\":[3,1,1],\"max\":[4,3,3],"
                           "\"material\":{\"loss_db_per_m\":6,\"reflectivity\":0.5}}]}";
    const char *selector = "{\"neutrino_tool\":\"acoustic\"}";
    Workspace ws = {0};
    Text cue = {0};
    Preview p = {.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    format(&cue, "Iona listens. Use [%s].", acoustic);
    p.parent = story_push(&ws, p.story, 0, "user", str(&cue));
    Text prompt = {0};
    bool ok = simulation_acoustic_selection_prompt(&p, &prompt, str(&cue)) &&
        strstr(str(&prompt), selector) && !strstr(str(&prompt), "loss_db_per_m") &&
        simulation_tool_reply(&p, selector) == SIM_REGENERATE && p.tool_rounds == 1 &&
        strstr(str(&p.tool_results), acoustic) && strstr(str(&p.tool_results), "Using the matching player-supplied") &&
        p.reading_count == 1 && strstr(str(&p.tool_results), "1 material traversals") &&
        simulation_tool_reply(&p, "The Narrative\n Flow feels fluid.") == SIM_REGENERATE &&
        simulation_tool_reply(&p, "Iona cups a hand to her ear.") == SIM_NARRATIVE;
    release(&prompt);
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    p.parent = story_push(&ws, p.story, 0, "user", str(&cue));
    ok = ok && simulation_tool_reply(&p,
        "[{\"neutrino_tool\":\"acoustic\",\"blockers\":[{\"min\":[3,1,1],\"max\":[4,3,3]}]}]") ==
        SIM_REGENERATE && p.reading_count == 1 && p.tool_rounds == 1 && p.tool_prose_only &&
        strstr(str(&p.tool_results), acoustic);
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    format(&cue, " Or use {\"neutrino_tool\":\"acoustic\",\"min\":[0,0,0],\"max\":[12,8,4],"
                 "\"source\":[2,2,2],\"listener\":[5,2,2]}.");
    p.parent = story_push(&ws, p.story, 0, "user", str(&cue));
    ok = ok && simulation_tool_reply(&p, selector) == SIM_REGENERATE && p.reading_count == 0 &&
        strstr(str(&p.tool_results), "Physics tool arguments rejected.") &&
        !strstr(str(&p.tool_results), "Using the matching player-supplied");
    free_protocol(&p);
    p = (Preview){.story = cJSON_Parse("{\"simEnabled\":true,\"nodes\":[]}")};
    format(&cue, " Acoustic setup: %s", acoustic);
    p.parent = story_push(&ws, p.story, 0, "user", str(&cue));
    ok = ok && simulation_attached_acoustic_turn(&p) &&
        simulation_tool_reply(&p, selector) == SIM_REGENERATE && p.tool_rounds == 1 &&
        p.reading_count == 1 && p.tool_prose_only && strstr(str(&p.tool_results), acoustic);
    free_protocol(&p);
    release(&cue);
    return ok;
}

static int parallel_fixture(void)
{
    const double length[] = {4,3,2};
    Json *gas = cJSON_Parse(ventilated_room);
    integer(gas, "resolution", 16);
    put(gas, "seconds", cJSON_CreateNumber(.25));
    Json *exhaust = at(at(gas, "boundaries"), "x_max");
    set(exhaust, "type", "pressure");
    integer(exhaust, "pressure_pa", 0);
    FlowConfig c;
    bool ok = flow_parse(gas, length, &c);
    cJSON_Delete(gas);
    if (!ok) return 1;
    FlowField f = flow_solve(&c);
    char *field = flow_encode(&f);
    ok = field && fabs(f.outflow_m3_s - 3) < .001;
    if (ok) {
        printf("CFD %.17g %.17g %s\n", f.divergence, f.tracer_out, field);
    }
    free(field);
    flow_release(&f);

    Json *settings = cJSON_CreateObject();
    ok &= rigid_actions(settings, "[{\"neutrino_tool\":\"scene_room\",\"min\":[0,0,0],\"max\":[12,8,4]},"
        "{\"neutrino_tool\":\"scene_runoff\",\"min_xy\":[0,0],\"max_xy\":[12,8],\"cells\":[64,48]},"
        "{\"neutrino_tool\":\"scene_material\",\"name\":\"ceramic\",\"mechanical\":{\"fracture_j\":5}},"
        "{\"neutrino_tool\":\"scene_material\",\"name\":\"reflector\","
        "\"acoustic\":{\"reflectivity\":0.9,\"loss_db_per_m\":0},\"optical\":{\"reflectivity\":0.9}},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"jug\",\"kind\":\"object\","
        "\"min\":[1,1,1],\"max\":[1.5,1.5,1.5],\"material_id\":\"ceramic\"},"
        "{\"neutrino_tool\":\"scene_body\",\"name\":\"jug\",\"mass_kg\":2,\"liquid_l\":1},"
        "{\"neutrino_tool\":\"scene_place\",\"name\":\"cup\",\"kind\":\"object\","
        "\"min\":[3,1,1],\"max\":[3.5,1.5,1.5],\"material_id\":\"ceramic\"},"
        "{\"neutrino_tool\":\"scene_body\",\"name\":\"cup\",\"mass_kg\":2,\"liquid_l\":1}]");
    Text edit = {0}, result = {0};
    for (int i = 0; ok && i < 50; ++i) {
        const int x = 2 + i%10, y = 2 + i/10;
        clear(&edit);
        format(&edit, "[{\"neutrino_tool\":\"scene_place\",\"name\":\"prop%d\",\"kind\":\"object\","
               "\"material_id\":\"reflector\",\"min\":[%d,%d,0],\"max\":[%.1f,%.1f,0.5]}]",i,x,y,x+.5,y+.5);
        ok = rigid_actions(settings, str(&edit));
    }
    ok &= rigid_actions(settings, "[{\"neutrino_tool\":\"scene_step\",\"seconds\":0.7}]");
    const Json *jug = physics_scene_body(settings,"jug");
    ok &= cJSON_IsTrue(at(jug,"fractured"));
    char *room = cJSON_PrintUnformatted(settings);
    if (ok && room) puts(room);
    free(room);

    Json *rays = cJSON_Parse("{\"backend\":\"cpu\",\"origin\":[1,1,3],\"count\":4096}");
    ok &= physics_run("scene_rays",rays,settings,&result) && strstr(str(&result), "4096 rays");
    puts(str(&result));
    clear(&result);
    cJSON_Delete(rays);
    Json *sound = cJSON_Parse("{\"backend\":\"cpu\",\"source\":[1,1,3],\"listener\":[4,3,2],"
                              "\"max_bounces\":4,\"include\":[\"prop0\",\"prop10\",\"prop12\",\"prop13\","
                              "\"prop14\",\"prop15\",\"prop20\",\"prop21\",\"prop22\",\"prop23\"]}");
    ok &= physics_run("scene_acoustic",sound,settings,&result) &&
          strstr(str(&result), "8192 candidates");
    puts(str(&result));
    clear(&result);
    ok &= physics_run("scene_light",sound,settings,&result) && strstr(str(&result), "8192 candidates");
    puts(str(&result));
    cJSON_Delete(sound);
    cJSON_Delete(settings);

    Json *soft = cJSON_Parse("{\"size\":[1,1,1],\"nodes\":10000,\"mass_kg\":2,"
                            "\"stiffness\":500,\"force\":[1,0,-2],\"seconds\":0.02}");
    ok &= physics_run("softbody3d",soft,nullptr,&result) && strstr(str(&result), "10000 mass nodes");
    puts(str(&result));
    cJSON_Delete(soft);
    release(&edit);
    release(&result);
    return !ok;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--rigid-timing")) return rigid_timing();
    if (argc > 1 && !strcmp(argv[1], "--checkpoint-timing")) return checkpoint_timing();
    if (argc > 1 && !strcmp(argv[1], "--cfd-reuse-timing")) return cfd_reuse_timing();
    if (argc > 1 && !strcmp(argv[1], "--persistent-cfd-timing")) return persistent_cfd_timing();
    if (argc > 1 && !strcmp(argv[1], "--raster-timing")) return raster_timing();
    if (argc > 1 && !strcmp(argv[1], "--ventilation-timing")) return ventilation_timing();
    if (argc > 1 && !strcmp(argv[1], "--pressure-timing"))
        return pressure_timing(argc > 2 && !strcmp(argv[2], "prescribed"));
    if (argc > 1 && !strcmp(argv[1], "--scaling-proof")) return scaling_proof();
    if (argc > 1 && !strcmp(argv[1], "--parallel-fixture")) return parallel_fixture();
    if (argc > 1 && !strcmp(argv[1], "--propagation-timing"))
        return propagation_timing(argc > 2 && !strcmp(argv[2], "dense"),false);
    if (argc > 1 && !strcmp(argv[1], "--cfd-timing")) return cfd_timing();
    const struct {
        const char *name, *json, *expected;
    } cases[] = {
        {"ray", "{\"origin\":[0,0,0],\"direction\":[3,0,0],\"min\":[2,-1,-1],\"max\":[3,1,1]}", "hits the box at 2 m"},
        {"ray", "{\"origin\":[0,2,0],\"direction\":[1,0,0],\"min\":[2,-1,-1],\"max\":[3,1,1]}", "misses the box"},
        {"ray", "{\"origin\":[0,0,0],\"direction\":[0,0,0],\"min\":[2,-1,-1],\"max\":[3,1,1]}", nullptr},
        {"rays", "{\"backend\":\"cpu\",\"min\":[2,-1,-1],\"max\":[3,1,1],\"rays\":["
            "{\"origin\":[0,0,0],\"direction\":[3,0,0]},{\"origin\":[0,2,0],\"direction\":[1,0,0]}]}",
            "CPU ray batch (2) hit distances in m: 2 miss"},
        {"acoustic", "{\"min\":[0,0,0],\"max\":[10,8,4],\"source\":[2,2,2],\"listener\":[5,2,2]}",
            "Direct 8.74"},
        {"acoustic", "{\"min\":[0,0,0],\"max\":[10,8,4],\"source\":[2,2,2],\"listener\":[5,2,2],"
            "\"blockers\":[{\"min\":[3,1,1],\"max\":[4,3,3]}],\"backend\":\"cpu\"}", "Direct blocked."},
        {"acoustic", "{\"min\":[0,0,0],\"max\":[10,8,4],\"source\":[0,2,2],\"listener\":[5,2,2]}", nullptr},
        {"fluid", "{\"height\":1,\"tank_area\":1,\"outlet_area\":0,\"seconds\":10}", "drained 0 L"},
        {"fluid", "{\"height\":1,\"tank_area\":1,\"outlet_area\":0.01,\"seconds\":1000}", "drained 1000 L"},
        {"gas", "{\"moles\":10,\"volume\":1,\"kelvin\":300,\"leak\":0.2,\"seconds\":10}", "8 mol remaining"},
        {"gas", "{\"moles\":10,\"volume\":0,\"kelvin\":300,\"leak\":0,\"seconds\":0}", nullptr},
        {"prop_load", "{\"strength\":\"reinforced\",\"load\":\"light\"}", "holds."},
        {"prop_load", "{\"strength\":\"ordinary\",\"load\":\"steady\"}", "gives slightly."},
        {"prop_load", "{\"strength\":\"fragile\",\"load\":\"heavy\"}", "breaks."},
        {"prop_load", "{\"strength\":\"ordinary\",\"load\":\"heavy\",\"speed\":20}", nullptr},
    };
    int failures = 0;
    if (!tool_protocol()) {
        fputs("The tool protocol must reject JSON prose, retain distinct results and skip reordered duplicate calls.\n", stderr);
        failures++;
    }
    if (!measurement_protocol()) {
        fputs("Saved narration must match typed fluid and acoustic readings and reject contradictory estimates.\n", stderr);
        failures++;
    }
    if (!material_acoustics()) {
        fputs("Material acoustics must split and attenuate paths and stop them at the modeled cutoff.\n", stderr);
        failures++;
    }
    if (!supplied_acoustic_protocol()) {
        fputs("Acoustic selectors must preserve supplied materials and reject ambiguous inputs or out-of-character prose.\n", stderr);
        failures++;
    }
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Json *args = cJSON_Parse(cases[i].json);
        Text result = {0};
        bool ok = args && physics_run(cases[i].name, args, nullptr, &result);
        if (ok != (cases[i].expected != nullptr) ||
            (ok && !strstr(str(&result), cases[i].expected))) {
            fprintf(stderr, "%s test %zu gave: %s\n", cases[i].name, i + 1, str(&result));
            failures++;
        }
        cJSON_Delete(args);
        release(&result);
    }
    Json *settings = cJSON_CreateObject();
    Json *tune = cJSON_Parse("{\"setting\":\"ambient_kelvin\",\"value\":200}");
    Json *invalid = cJSON_Parse("{\"setting\":\"ambient_kelvin\",\"value\":500}");
    Json *args = cJSON_Parse("{\"moles\":10,\"volume\":1,\"leak\":0,\"seconds\":0}");
    Text result = {0};
    if (!physics_tune(settings, tune) || physics_tune(settings, invalid) ||
        !physics_run("gas", args, settings, &result) || !strstr(str(&result), "16.629 kPa")) {
        fputs("The gas estimate must use scene ambient and reject invalid changes.\n", stderr);
        failures++;
    }
    clear(&result);
    Json *reflection = cJSON_Parse("{\"setting\":\"wall_reflectivity\",\"value\":0}");
    Json *acoustic_args = cJSON_Parse(cases[4].json);
    if (!physics_tune(settings, reflection) || !physics_run("acoustic", acoustic_args, settings, &result) ||
        !strstr(str(&result), "Direct ") || strstr(str(&result), "echo")) {
        fputs("A perfectly absorbing room must suppress the reflected paths.\n", stderr);
        failures++;
    }
    clear(&result);
    put(reflection, "value", cJSON_CreateNumber(DBL_TRUE_MIN));
    if (!physics_tune(settings, reflection) || !physics_run("acoustic", acoustic_args, settings, &result) ||
        !strstr(str(&result), "echo") || strstr(str(&result), "inf") || strstr(str(&result), "nan")) {
        fputs("Nonzero reflection levels must remain finite even when the linear amplitude underflows.\n", stderr);
        failures++;
    }
    cJSON_Delete(acoustic_args);
    cJSON_Delete(reflection);
    release(&result);
    cJSON_Delete(args);
    cJSON_Delete(invalid);
    cJSON_Delete(tune);
    cJSON_Delete(settings);
    if (!ordered_protocol()) { fputs("Ordered simulation protocol failed.\n", stderr); failures++; }
    if (!extended_models()) { fputs("Extended 3D model checks failed.\n", stderr); failures++; }
    if (!narrator_world_control()) { fputs("Narrator world control and branch isolation failed.\n",stderr); failures++; }
    if (!party_control()) { fputs("Party creation, acting, and inventory isolation failed.\n",stderr); failures++; }
    if (!large_scene_state(argc > 1 && !strcmp(argv[1],"--rt") ? "rt" : "cpu")) {
        fputs("Large scenario state checks failed.\n",stderr);
        failures++;
    }
    if (!cfd_models(argc > 1 && !strcmp(argv[1],"--cfd-gpu"))) {
        fputs("CFD transport/projection/memory checks failed.\n", stderr);
        failures++;
    } else if (argc > 1 && !strcmp(argv[1],"--cfd-gpu")) puts("OpenCL pressure projection matches CPU; Windows mapped memory matches CPU.");
    if (!optics_and_gas()) { fputs("3D optics and gas attenuation checks failed.\n", stderr); failures++; }
    if (!scenario_protocol()) { fputs("Typed scenario protocol failed.\n", stderr); failures++; }
    if (!rigid_dynamics()) { fputs("Shared rigid dynamics test failed.\n", stderr); failures++; }
    if (!rotation_and_spill()) { fputs("Rotation and spill physics test failed.\n", stderr); failures++; }
    if (!runoff_conservation()) { fputs("Runoff mass and depth test failed.\n", stderr); failures++; }
    if (!rigid_branch_replay()) { fputs("Rigid branch replay test failed.\n", stderr); failures++; }
    if (!checkpoint_order_and_siblings()) { fputs("Checkpoint order or sibling replay failed.\n", stderr); failures++; }
    if (!cumulative_plan_budget()) { fputs("Cumulative simulation plan budget failed.\n", stderr); failures++; }
    if (!character_knowledge_isolation()) { fputs("Character knowledge leaked across recipients or branches.\n", stderr); failures++; }
    if (!plan_cfd_reuse()) { fputs("Plan CFD field reuse or invalidation failed.\n", stderr); failures++; }
    if (!persisted_cfd()) { fputs("Saved CFD continuation or branch isolation failed.\n", stderr); failures++; }
    if (!recursive_relay(argc > 1 && !strcmp(argv[1], "--rt") ? "rt" : "cpu")) {
        fputs("Recursive relay propagation failed.\n", stderr);
        failures++;
    }
    if (!sensory_branch_state()) { fputs("Sensory observations escaped their transaction or branch.\n", stderr); failures++; }
    if (!attached_rigid_plan()) { fputs("Attached rigid plan test failed.\n", stderr); failures++; }
    if (!solid_cfd()) { fputs("Voxel CFD boundary test failed.\n", stderr); failures++; }
    if (!geometry_aware_flow_frames()) { fputs("CFD raster or geometry-aware frame retention failed.\n", stderr); failures++; }
    if (!closed_cfd()) { fputs("Closed CFD exterior failed.\n", stderr); failures++; }
    if (!ventilated_cfd()) { fputs("CFD ventilation or no-slip boundary failed.\n", stderr); failures++; }
    if (!pressure_cfd()) { fputs("CFD pressure openings or backflow failed.\n", stderr); failures++; }
    if (!material_transport()) { fputs("Material transport test failed.\n", stderr); failures++; }
    if (!scene_protocol("cpu")) { fputs("3D scene simulation protocol failed.\n", stderr); failures++; }
#ifndef NEUTRINO_VULKAN_RT
    Json *no_rt = cJSON_Parse(cases[3].json);
    set(no_rt, "backend", "rt");
    PhysicsReading unavailable;
    if (!physics_run_reading("rays", no_rt, nullptr, &result, &unavailable) || !unavailable.unavailable ||
        !strstr(str(&result), "hardware RT unavailable")) failures++;
    cJSON_Delete(no_rt);
    release(&result);
#endif
    if (argc > 1 && (!strcmp(argv[1], "--gpu") || !strcmp(argv[1], "--vulkan") ||
                     !strcmp(argv[1], "--rt"))) {
        bool vulkan = !strcmp(argv[1], "--vulkan");
        bool rt = !strcmp(argv[1], "--rt");
        if (rt && !scene_protocol("rt")) {
            fputs("Hardware RT 3D scene protocol failed.\n", stderr);
            failures++;
        }
#ifdef NEUTRINO_VULKAN_RT
        if (rt) {
            if (!rt_parity()) failures++;
            else puts("RT matches CPU distances and box IDs for 4096 rays, including cached and edited geometry.");
        }
#endif
        const char *backend = rt ? "rt" : vulkan ? "vulkan" : "gpu";
        const char *label = rt ? "Vulkan hardware RT" : vulkan ? "Vulkan " : "OpenCL GPU";
        Json *batch = cJSON_Parse(cases[3].json);
        set(batch, "backend", backend);
        if (!physics_run("rays", batch, nullptr, &result) ||
            !strstr(str(&result), label) ||
            !strstr(str(&result), "ray batch (2) hit distances in m: 2 miss")) failures++;
        puts(str(&result));
        clear(&result);
        Json *room = cJSON_Parse(cases[5].json);
        set(room, "backend", backend);
        if (!physics_run("acoustic", room, nullptr, &result) ||
            !strstr(str(&result), label) ||
            !strstr(str(&result), "Direct blocked.")) failures++;
        puts(str(&result));
        release(&result);
        cJSON_Delete(room);
        cJSON_Delete(batch);
    }
    return failures != 0;
}
