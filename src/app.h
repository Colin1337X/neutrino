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
    bool preempted;
    Text output, reasoning, error;
    Json *cached_picks;
} MemoryLane;
typedef struct { Text body, thinking, error; char label[64]; } Candidate;
typedef struct {
    Operation op;
    Json *scene, *story;
    Text source, messages_context, objective, lore_trace, memory_trace, recall_output, recall_error, recall_thinking;
    size_t start, end, insert_at;
    int version, parent, head;
    Candidate candidates[30];
    int count, current, running;
    bool open, reasoning, stopped, complete, recalling;
    Json *memory_picks;
    Inference *job;
} Preview;
typedef struct {
    Workspace ws;
    Json *scene, *story;
    bool roleplay, demo, quit, compact, editing, slash, view_panel;
    bool panel_preview, selected;
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
    const char *server_override;
} App;

typedef struct { const char *name, *args, *summary; } Command;
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

#endif
