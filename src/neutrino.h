#ifndef NEUTRINO_H
#define NEUTRINO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include "cJSON.h"

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#error "Neutrino requires a C23 compiler."
#endif

typedef cJSON Json;
typedef struct { char *data; size_t len, cap; } Text;
#define NEUTRINO_VERSION "0.20.3"
#define FILE_LIMIT (32u * 1024u * 1024u)
#define EDIT_LIMIT (2u * 1024u * 1024u)

void append_n(Text *text, const char *value, size_t length);
void append(Text *text, const char *value);
void format(Text *text, const char *pattern, ...);
void clear(Text *text);
void release(Text *text);
const char *str(const Text *text);
void normalize_words(Text *text, const char *value);
bool compendium_novel_claims(const Json *known, const Json *batch, const char *type,
                             const char *name, const char *description, Text *out);
bool compendium_placement_evidence(const Json *known, const Json *history, const char *reply,
                                   Text *name, Text *evidence);
Json *compendium_placement_entries(const Json *known, const Json *history, const char *reply);
Json *compendium_opening_evidence(const char *reply);
Json *compendium_signal_evidence(const char *reply);
Json *compendium_scene_evidence(const char *reply);
Json *compendium_actor_evidence(const char *reply);
bool compendium_filing_index(const char *reply, const char *tag, Text *out);
bool compendium_filing_evidence(const char *reply, const char *quote, Text *evidence);
bool compendium_filing_label(const char *name, const char *evidence);
bool compendium_character_alias(const char *left, const char *right);
bool compendium_titled_character(const char *name);
const char *compendium_filing_identity(const char *type, const char *name, const char *description,
                                       const char *reply, Text *corrected_name);
char *copy(const char *value);
const char *js(const Json *obj, const char *key);
Json *at(const Json *obj, const char *key);
int number(const Json *obj, const char *key);
void put(Json *obj, const char *key, Json *value);
void set(Json *obj, const char *key, const char *value);
void integer(Json *obj, const char *key, int value);
const char *env(const char *name, const char *fallback);

char *read_file(const char *path);
bool write_file(const char *path, const char *data, bool replace);

typedef struct {
    Json *root;
    const char *path;
    struct WorkspaceLock *lock;
    bool dirty;
} Workspace;

bool workspace_open(Workspace *ws, const char *path, bool writable);
bool workspace_lock(Workspace *ws);
void workspace_close(Workspace *ws);
bool workspace_save(Workspace *ws);
bool workspace_export(const Workspace *ws, const char *path);
Json *workspace_snapshot(const Workspace *ws);
bool workspace_replace(Workspace *ws, const Json *root);
void workspace_sync_mark(Json *root);
Json *workspace_import(Workspace *ws, const char *path, Text *error);
Json *workspace_import_data(Workspace *ws, const char *name, const char *data, size_t size, Text *error);
Json *scene_find(const Workspace *ws, const char *path);
Json *scene_add(Workspace *ws, const char *path);
bool scene_edit(Workspace *ws, Json *scene, const char *value, const char *reason);
Json *story_add(Workspace *ws, const char *title, Json *card);
Json *story_pick(const Json *story, int id);
Json *story_find(const Workspace *ws, const char *id);
int story_push(Workspace *ws, Json *story, int parent, const char *role, const char *content);
bool story_lorebook(const Json *input);
void story_expand_names(const Json *story, const char *source, Text *out);
Json *story_path(const Json *story, int head);
int workspace_context_budget(const Workspace *ws);
void narrator_add_history(Json *messages, const Json *path, int covered, int insertion,
                          int budget, Text *trace);
void workspace_demo(Workspace *ws);
bool collect_context(const Workspace *ws, const Json *scene, const Json *story, int head, Text *out, Text *trace);
bool collect_context_with_cue(const Workspace *ws, const Json *scene, const Json *story, int head,
                              const char *cue, Text *out, Text *trace);
const char *scene_text(const Workspace *ws, const Json *scene);

enum {
    KEY_RESIZE = 0x110000, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME,
    KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_F1, KEY_F2, KEY_F3, KEY_F4,
    KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_BACKTAB, KEY_FOCUS, KEY_MARK, KEY_CLOSED
};
bool terminal_open(void);
void terminal_close(void);
void terminal_size(int *cols, int *rows);
static inline void bound_terminal_viewport(int *cols, int *rows)
{
    enum { MAX_VIEWPORT_COLUMNS = 240, MAX_VIEWPORT_ROWS = 120 };
    if (*cols > MAX_VIEWPORT_COLUMNS) *cols = MAX_VIEWPORT_COLUMNS;
    if (*rows > MAX_VIEWPORT_ROWS) *rows = MAX_VIEWPORT_ROWS;
}
size_t narrator_complete_prefix(const char *text);
bool narrator_short_answer(const char *reply, size_t length, const char *cue);
int terminal_key(int timeout);
void terminal_write(const char *data, size_t length);
size_t utf_next(const char *s, size_t length, size_t pos);
size_t utf_prev(const char *s, size_t pos);
size_t utf_encode(int cp, char bytes[4]);
int utf_width(const char *s, size_t n);

typedef struct Inference Inference;
int session_http(const char *url, const char *path, const char *password, const char *admin,
                 const Text *input, Text *output);
Inference *session_request(const char *url, const char *path, const char *password, const char *admin,
                           const Json *settings);
enum { NEUTRINO_GATEWAY_MESSAGE_LIMIT = 512 };
typedef struct {
    int slot, max_tokens, timeout_ms;
    const char *json_schema;
    bool include_usage;
} RequestOptions;
typedef struct {
    double tokens_per_second;
    int completion_tokens, prompt_tokens;
    uint64_t ttft_ms;
    bool has_ttft, clean_stop;
} InferenceMetrics;
Inference *inference_start(const char *endpoint, const char *key, const char *model,
                           const Json *messages);
Inference *inference_request(const char *endpoint, const char *key, const char *model,
                             const Json *messages, RequestOptions options);
Inference *inference_web(const char *mode, const Json *messages);
bool inference_read(Inference *job, Text *out, Text *reasoning, Text *error);
Inference *inference_probe(const char *endpoint, const char *key);
Inference *inference_provider(const char *endpoint, const char *key, const Json *settings);
Inference *inference_models(const char *endpoint, const char *key);
InferenceMetrics inference_metrics(Inference *job);
void inference_cancel(Inference *job);
void inference_free(Inference *job);
uint64_t monotonic_ms(void);
void pause_ms(unsigned milliseconds);
typedef struct { const char *endpoint, *model, *key_env; } ProviderSetup;
typedef struct Gateway Gateway;
Gateway *local_gateway_open(const char *password, const char *admin, const char *profiles, int *port);
void gateway_poll(Gateway *gateway);
void gateway_close(Gateway *gateway);
int inference_server(int port, const ProviderSetup *setup, const volatile sig_atomic_t *stop);
int bench_ttft(const char *server, const char *token, int trials, const volatile sig_atomic_t *stop);

#endif
