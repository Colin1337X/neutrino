#ifndef NEUTRINO_APP_H
#define NEUTRINO_APP_H
#include "neutrino.h"

typedef enum { CONTINUE, REWRITE, EXPAND, DESCRIBE, DRAFT, BRAINSTORM,
               CRITIQUE, CONTINUITY, EXTRACT, NARRATE, SWIPE, OP_COUNT } Operation;
typedef enum { COMPOSER, LIBRARY, EDITOR, PANEL } Focus;
typedef enum { MEMORY_IDLE, MEMORY_FILE, MEMORY_HISTORY, MEMORY_PREFETCH } MemoryTask;
typedef struct {
    Inference *job;
    MemoryTask task;
    Json *story, *cached_story;
    int anchor, cached_head;
    uint64_t after;
    bool preempted, filing_retry;
    Text output, reasoning, error;
    Json *cached_picks;
} MemoryLane;
typedef struct {
    Text body, thinking, error;
    Inference *job;
    InferenceMetrics metrics;
    char label[64];
    bool finished;
} Candidate;
enum { SIM_TOOL_LIMIT = 12, ACOUSTIC_BLOCKERS = 16, ACOUSTIC_ROOM_PATHS = 7,
       PHYSICS_ACOUSTIC_PATHS = ACOUSTIC_ROOM_PATHS + 6 * ACOUSTIC_BLOCKERS,
       PHYSICS_RECURSIVE_ARRIVALS = 24 };
typedef enum { READING_NONE, READING_FLUID, READING_ACOUSTIC, READING_OPTICAL, READING_FLOW,
               READING_MASS_TRANSFER } ReadingKind;
typedef struct {
    ReadingKind kind;
    double seconds, level_m, drained_l, remaining_l, flow_lps, initial_flow_lps;
    struct { bool blocked, faded; int bounces; double delay_ms, level_db; }
        arrival[PHYSICS_ACOUSTIC_PATHS + PHYSICS_RECURSIVE_ARRIVALS];
    int arrivals, recursive_arrivals;
    unsigned recursive_workers;
    bool material_acoustics, unavailable;
    double optical_power, optical_first_ns, optical_first_power;
    int optical_first_bounces;
    bool recursive_limited, optical_direct_blocked;
    double tracer_concentration;
    double flow_time_s;
    double porous_speed, pore_volume;
    bool voxel_flow, porous_flow, stored_flow, closed_flow, ventilated_flow, pressure_flow;
    int unresolved_obstacles;
    double inflow_m3_s, outflow_m3_s, backflow_m3_s;
    const char *transfer_method;
    double transferred_kg, final_kg, equilibrium_kg, gas_kg;
    bool closed_transfer;
} PhysicsReading;
enum { SCENARIO_ACTION_LIMIT = 32 };
typedef struct {
    const Json *queries;
    const char *name;
    bool hypothetical;
} SimulationPlan;
bool simulation_plan_parse(const Json *plan, SimulationPlan *parsed);
int simulation_plan_condition(const Json *query, const Json *settings,
                              const PhysicsReading readings[], int preceding_steps);
void simulation_plan_report(const SimulationPlan *plan, const Json *settings,
                            const PhysicsReading readings[], int steps, bool complete,
                            bool pending_commit, Text *out);
int physics_scene_relation(const Json *settings, const char *entity, const char *target);
const Json *physics_scene_body(const Json *settings, const char *entity);
bool physics_scene_spill(const Json *settings, const char *entity, const char *surface,
                         Json *evidence, double *litres);
bool physics_scene_puddle(const Json *settings, const char *entity, const char *surface,
                          Json *evidence, double *litres, double *depth);
typedef struct {
    Operation op;
    Json *scene, *story;
    Text source, messages_context, objective, lore_trace, memory_trace, recall_output, recall_error, recall_thinking;
    Text previous_reply, selection_trace;
    Text complete_draft;
    InferenceMetrics complete_draft_metrics;
    Text world_context, speaker, tool_results, tool_feedback, research_data;
    Text auto_research_trace, auto_research_output, auto_research_error, auto_research_thinking;
    size_t start, end, insert_at;
    int version, parent, head;
    Candidate candidates[30];
    int count, current, next, active;
    bool open, reasoning, stopped, complete, recalling, repeat_retry, completion_retry, trimmed_reply;
    Json *memory_picks, *tool_pending, *tool_requests;
    PhysicsReading readings[SIM_TOOL_LIMIT];
    int tool_rounds, reading_count;
    int narrator_requests;
    uint64_t narrator_prompt_tokens;
    bool tool_prose_only, tool_prose_retry, tool_measurement_retry;
    Inference *recall_job;
    Inference *auto_research_job;
    bool planning_research, inline_research, searched_web, definition_research;
} Preview;

enum { NEOFETCH_SAMPLES = 32 };
typedef struct {
    Inference *probe;
    Json *server;
    Text error;
    uint64_t refresh_at;
    bool stale_probe;
    InferenceMetrics samples[NEOFETCH_SAMPLES];
    int count, next;
} Neofetch;

typedef struct {
    Text endpoint, model, key, error;
    int provider, field;
    size_t caret;
    bool open, admin, authenticated, usage;
} ModelMenu;

static inline bool preview_busy(const Preview *preview)
{
    return preview->recall_job != nullptr || preview->auto_research_job != nullptr || preview->active > 0;
}
typedef struct {
    Workspace ws;
    Json *scene, *story;
    bool roleplay, demo, quit, compact, editing, slash, view_panel;
    bool panel_preview, panel_neofetch, selected, ssh_session, browser;
    Focus focus;
    Text screen, prompt, edit, info, status, trace, simulation_trace, server_address;
    Text research_scope, research_pending_scope, research_data, research_reply;
    Inference *research_job;
    Inference *provider_job;
    bool provider_switching, provider_menu_request;
    ModelMenu model_menu;
    char info_title[96];
    size_t cursor, caret, mark, selection_a, selection_b;
    int cols, rows, scroll, scroll_max, panel_scroll, rail_cursor, rail_offset, command_cursor;
    int prompt_history;
    char *history[40];
    int history_count;
    Preview preview;
    MemoryLane memory;
    Neofetch neo;
    const char *server_override;
} App;

enum { MODEL_CURRENT, MODEL_OLLAMA, MODEL_LLAMACPP, MODEL_OPENAI, MODEL_CUSTOM, MODEL_PROVIDER_COUNT };
enum { MODEL_PROVIDER_FIELD, MODEL_ENDPOINT_FIELD, MODEL_ID_FIELD, MODEL_KEY_FIELD, MODEL_USAGE_FIELD,
       MODEL_APPLY_FIELD, MODEL_FIELD_COUNT };
extern const char *const model_provider_labels[MODEL_PROVIDER_COUNT];
bool model_field_enabled(const ModelMenu *menu, int field);

typedef struct Browser Browser;
typedef enum { BROWSER_STATE, BROWSER_HISTORY, BROWSER_ACTION, BROWSER_IMPORT,
               BROWSER_LORE, BROWSER_PRESET, BROWSER_EXPORT, BROWSER_TRACE, BROWSER_CANDIDATE } BrowserRoute;
Browser *browser_open(int port);
void browser_poll(Browser *web, App *app);
void browser_close(Browser *web);
int app_browser_response(App *app, BrowserRoute route, const Text *input, Text *output);

typedef struct { const char *name, *args, *summary; } Command;
enum { COMMAND_MATCH_LIMIT = 96 };
extern const Command commands[];
extern const size_t command_count;
extern const char *const operation_names[OP_COUNT];

void ui_draw(App *app);
void ui_dump(App *app, int cols, int rows);
size_t ui_move_editor(const App *app, int delta);
int ui_command_matches(const App *app, int *results, int cap);
int ui_library_size(const App *app);
Json *ui_library_at(const App *app, int index);
void ui_clamp_rail(App *app);
const char *ui_scene_name(const App *app);
const char *ui_story_name(const App *app);
const char *ui_model(const App *app);
const char *ui_endpoint(const App *app);
Inference *app_inference(const App *app, const char *role, const Json *messages);
void app_info(App *app, const char *title);
void app_status(App *app, const char *message);

bool memory_enabled(const Json *story);
const char *memory_mode(const Json *story);
void memory_command(App *app, char *args);
void memory_inject(const Json *story, int head, const Json *picks, Text *system, Text *trace);
int memory_history_target(const Json *path);
const char *memory_recap(const Json *path, int *covered);
Json *memory_recall_prompt(const Json *story, int head, const char *objective);
Json *memory_parse_picks(const Text *output);
void memory_schedule(App *app, Json *story, int reply);
bool memory_tick(App *app);
void memory_pause(App *app);
void memory_close(App *app);
void memory_binding(App *app, const char *binding);
Inference *memory_start_recall(const App *app, const Json *story, const Json *messages);
void world_command(App *app, const char *verb, char *argument);
void world_character_messages(const Preview *p, Json *messages);
Json *world_local_cast(const Json *story, int head);
void world_context(const Json *story, int head, Text *out);
const char *world_room(const Json *story, int head, Text *out);
bool world_address(const Json *story, const char *line, Text *speaker, const char **message);
const char *world_character_room(const Json *story, int head, const char *name, Text *out);
bool world_inventory_tool(Json *story, int head, const Json *args);
bool world_character_tool(Json *story, int head, const Json *args);
void world_active_character(const Json *story, int head, Text *out);
bool world_item_available(const Json *story, const char *name);
bool world_edit(Json *story, int head, const Json *args);
bool world_inspect(const Json *story, int head, const Json *args, Text *out);
bool physics_run(const char *name, const Json *args, const Json *settings, Text *out);
bool physics_run_reading(const char *name, const Json *args, const Json *settings, Text *out, PhysicsReading *reading);
struct PhysicsCache;
bool physics_run_cached(const char *name, const Json *args, const Json *settings, Text *out,
                        PhysicsReading *reading, struct PhysicsCache *cache);
bool physics_scene_edit(Json *settings, const Json *op);
bool physics_prose_matches(const PhysicsReading *readings, int count, const char *body);
void physics_prose_readouts(const PhysicsReading *readings, int count, Text *body);
bool physics_tune(Json *settings, const Json *op);
void physics_describe(const Json *settings, Text *out);
bool simulation_enabled(const Json *story);
Json *simulation_state(const Json *story, int head);
void simulation_observations(const Json *story, int head, const char *character, Text *out, bool details);
void simulation_command(App *a, const char *verb, const char *arg);
void simulation_prompt(const Preview *p, Text *system);
void simulation_player_reference(Text *out, const char *action);
bool simulation_acoustic_selection_prompt(const Preview *p, Text *out, const char *action);
bool simulation_plan_selector(const Preview *p);
bool simulation_attached_acoustic_turn(const Preview *p);
typedef enum { SIM_NARRATIVE, SIM_REGENERATE, SIM_NO_PROSE } SimulationReply;
SimulationReply simulation_tool_reply(Preview *p, const char *body);
void simulation_commit(App *a, Preview *p, int id);

#endif
