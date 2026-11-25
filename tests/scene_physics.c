#include "app.h"
#include "ray.h"
#include "physics_models.h"

#include <float.h>
#include <math.h>
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
    RtBox boxes[] = {
        {{2, -1, -1}, {3, 1, 1}}, {{4, -1, -1}, {5, 2, 2}}, {{2, -1, -1}, {3, 1, 1}}
    };
    static RtRay rays[4096];
    static RtHit hits[4096];
    for (size_t i = 0; i < 4096; ++i) {
        rays[i] = (RtRay){{(i % 4 == 0) ? 2.5f : 0, (float)(i % 13) * .25f - 1.5f,
                           (float)(i % 17) * .125f - 1},
                          {(i % 6 == 0) ? -1 : 1, (i % 3 == 0) ? 0 : .125f, 0}};
    }
    rays[0] = (RtRay){{2.5f, 0, 0}, {0, 0, 0}};
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 2) boxes[0].lo[0] = 2.75f;
        if (!ray_vulkan_rt(rays, 4096, boxes, sizeof boxes / sizeof *boxes, hits)) return false;
        for (size_t i = 0; i < 4096; ++i) {
            int winner = -1;
            float expected = -1;
            for (size_t box = 0; box < sizeof boxes / sizeof *boxes; ++box) {
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
                fprintf(stderr, "RT parity pass %d ray %zu: expected box %d at %g, got box %g at %g\n",
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
    if (!large_scene_state(argc > 1 && !strcmp(argv[1],"--rt") ? "rt" : "cpu")) {
        fputs("Large scenario state checks failed.\n",stderr);
        failures++;
    }
    if (!cfd_models(argc > 1 && !strcmp(argv[1],"--cfd-gpu"))) {
        fputs("CFD transport/projection/memory checks failed.\n", stderr);
        failures++;
    } else if (argc > 1 && !strcmp(argv[1],"--cfd-gpu")) puts("OpenCL pressure projection matches CPU; Windows mapped memory matches CPU.");
    if (!optics_and_gas()) { fputs("3D optics and gas attenuation checks failed.\n", stderr); failures++; }
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
