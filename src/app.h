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
typedef struct {
    Operation op;
    Json *scene, *story;
    Text source, messages_context, objective, lore_trace, memory_trace, recall_output, recall_error, recall_thinking;
    Text previous_reply, selection_trace;
    Text world_context, speaker, tool_results, tool_feedback;
    size_t start, end, insert_at;
    int version, parent, head;
    Candidate candidates[30];
    int count, current, next, active;
    bool open, reasoning, stopped, complete, recalling, repeat_retry;
    Json *memory_picks, *tool_pending, *tool_requests;
    int tool_rounds;
    bool tool_prose_only, tool_prose_retry;
    Inference *recall_job;
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

static inline bool preview_busy(const Preview *preview)
{
    return preview->recall_job != nullptr || preview->active > 0;
}
typedef struct {
    Workspace ws;
    Json *scene, *story;
    bool roleplay, demo, quit, compact, editing, slash, view_panel;
    bool panel_preview, panel_neofetch, selected, ssh_session;
    Focus focus;
    Text screen, prompt, edit, info, status, trace, server_address;
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
void world_context(const Json *story, int head, Text *out);
const char *world_room(const Json *story, int head, Text *out);
bool world_address(const Json *story, const char *line, Text *speaker, const char **message);
const char *world_character_room(const Json *story, int head, const char *name, Text *out);
bool world_inventory_tool(Json *story, int head, const Json *args);
bool physics_run(const char *name, const Json *args, const Json *settings, Text *out);
bool physics_tune(Json *settings, const Json *op);
void physics_describe(const Json *settings, Text *out);
bool simulation_enabled(const Json *story);
void simulation_command(App *a, const char *verb, const char *arg);
void simulation_prompt(const Preview *p, Text *system);
typedef enum { SIM_NARRATIVE, SIM_REGENERATE, SIM_NO_PROSE } SimulationReply;
SimulationReply simulation_tool_reply(Preview *p, const char *body);
void simulation_commit(App *a, Preview *p, int id);

#endif
