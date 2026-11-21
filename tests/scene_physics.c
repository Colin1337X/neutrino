#include "app.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

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
         strstr(str(&p.tool_results), "Discarded prose that mentioned the tool protocol.") &&
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

int main(int argc, char **argv)
{
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
    if (argc > 1 && !strcmp(argv[1], "--gpu")) {
        Json *batch = cJSON_Parse(cases[3].json);
        set(batch, "backend", "gpu");
        if (!physics_run("rays", batch, nullptr, &result) ||
            !strstr(str(&result), "OpenCL GPU ray batch (2) hit distances in m: 2 miss")) failures++;
        puts(str(&result));
        clear(&result);
        Json *room = cJSON_Parse(cases[5].json);
        set(room, "backend", "gpu");
        if (!physics_run("acoustic", room, nullptr, &result) ||
            !strstr(str(&result), "OpenCL GPU visibility") || !strstr(str(&result), "Direct blocked.")) failures++;
        puts(str(&result));
        release(&result);
        cJSON_Delete(room);
        cJSON_Delete(batch);
    }
    return failures != 0;
}
