#ifndef NEUTRINO_APP_H
#define NEUTRINO_APP_H
#include "neutrino.h"

typedef enum { CONTINUE, REWRITE, EXPAND, DESCRIBE, DRAFT, BRAINSTORM,
               CRITIQUE, CONTINUITY, EXTRACT, NARRATE, SWIPE, OP_COUNT } Operation;
typedef enum { COMPOSER, LIBRARY, EDITOR, PANEL } Focus;
typedef struct { Text body, thinking, error; char label[64]; } Candidate;
typedef struct {
    Operation op;
    Json *scene, *story;
    Text source, messages_context, objective;
    size_t start, end, insert_at;
    int version, parent, head;
    Candidate candidates[30];
    int count, current, running;
    bool open, reasoning, stopped, complete;
    Inference *job;
} Preview;
typedef struct {
    Workspace ws;
    Json *scene, *story;
    bool roleplay, demo, quit, compact, editing, slash, view_panel;
    bool panel_preview, selected;
    Focus focus;
    Text screen, prompt, edit, info, status;
    char info_title[96];
    size_t cursor, caret, mark, selection_a, selection_b;
    int cols, rows, scroll, scroll_max, panel_scroll, rail_cursor, rail_offset, command_cursor;
    int prompt_history;
    char *history[40];
    int history_count;
    Preview preview;
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

#endif
