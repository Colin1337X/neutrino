#include "app.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const operation_names[OP_COUNT] = {
    "Continue", "Rewrite", "Expand", "Describe", "Draft", "Brainstorm",
    "Critique", "Continuity", "Extract", "Narrate", "Swipe"
};
static const char *const instructions[OP_COUNT] = {
    "Continue the scene in its established voice. Return only new prose.",
    "Rewrite the target passage to meet the objective. Return only the replacement prose.",
    "Expand the target with concrete detail and preserve its facts. Return only revised prose.",
    "Write one short, vivid sensory detail for the target. Preserve its plot, voice and viewpoint.",
    "Draft scene prose from the objective and supplied story context. Return only prose.",
    "Develop several distinct possibilities for the objective and explain their story implications.",
    "Give concrete editorial feedback on the target, citing passages and suggesting improvements.",
    "Identify possible contradictions. Quote evidence, indicate uncertainty and suggest resolutions.",
    "Extract suggested story facts as a JSON array. Each entry has string fields type, name, description. Types include character, location, object, event, lore, and world-rule.",
    "Narrate the next roleplay beat as the character and world. Do not decide the player's actions. Return only the next response.",
    "Write a fresh alternative roleplay beat as the character and world. Do not decide the player's actions. Return only the replacement response."
};
const Command commands[] = {
    {"help", "", "Show keys and commands"},
    {"scene", " /folder/book/chapter/scene", "Create or open a manuscript scene"},
    {"scratch", "", "Open the scratchpad"},
    {"edit", "", "Edit the current manuscript"},
    {"story", " title", "Start a roleplay story"},
    {"play", " [title]", "Start a roleplay linked to this scene"},
    {"branch", " [message ID]", "Inspect or switch the active path"},
    {"rewind", " [ancestor ID]", "Rewind one exchange or return to an earlier message"},
    {"search", " text", "Find literal text on the active roleplay path"},
    {"memory", " [mode|new|edit|status]", "Open or manage branch-aware story memory"},
    {"world", " [log]", "Inspect this branch's room, inventory, or activity"},
    {"map", " [destination]", "Show world links or the shortest route from this room"},
    {"inspect", " [name]", "Read details about the room, a nearby character, or an item"},
    {"room", " NAME | DESCRIPTION", "Define a room"},
    {"connect", " ROOM | ROOM", "Link two rooms"},
    {"go", " ROOM", "Move through an exit"},
    {"item", " NAME | DESCRIPTION", "Place an item in the current room"},
    {"take", " ITEM", "Pick up a nearby item"},
    {"drop", " ITEM", "Leave a carried item here"},
    {"npc", " NAME | DESCRIPTION", "Add a character in this room"},
    {"weather", " TEXT", "Set the weather on this branch"},
    {"say", " TEXT", "Speak in this room without generating a reply"},
    {"trace", " [file]", "Inspect or export the exact last prompt and lore decisions"},
    {"swipe", "", "Generate an alternate reply"},
    {"previous", "", "Select the previous reply variant"},
    {"next", "", "Select the next reply variant"},
    {"narrate", " [direction]", "Continue roleplay without a player message"},
    {"persona", " name | description", "Set the player's persona"},
    {"lore", " file", "Attach a keyed lorebook to the current story"},
    {"preset", " file", "Attach enabled prompt text from a chat preset"},
    {"link", " /folder/book/chapter/scene", "Share manuscript and canon with this story"},
    {"transcript", " /folder/book/chapter/scene", "Copy the active conversation to a new scene"},
    {"continue", " [objective]", "Generate the next passage for review"},
    {"rewrite", " [objective]", "Revise the scene or selection"},
    {"rewrite-preset", " [preset] [direction]", "Apply a focused revision to the scene or selection"},
    {"expand", " [objective]", "Develop the scene or selection"},
    {"describe", " [subject]", "Generate sensory details"},
    {"draft", " outline", "Draft prose from an outline"},
    {"brainstorm", " topic", "Explore possibilities"},
    {"critique", " [objective]", "Get editorial feedback"},
    {"continuity", " [objective]", "Review possible canon conflicts"},
    {"extract", "", "Propose Story Bible entries"},
    {"accept", "", "Apply the selected candidate"},
    {"insert", "", "Insert a Describe detail after the target"},
    {"reject", "", "Close the candidate review"},
    {"preview", " [number]", "Open or choose a candidate"},
    {"regenerate", "", "Run the last writing tool on the current scene"},
    {"preview-save", " file", "Export the selected candidate as text"},
    {"reasoning", "", "Toggle the provider's reasoning view"},
    {"count", " 1–5", "Set candidates per operation or sense"},
    {"senses", " sight,sound,smell,touch,taste,metaphor", "Set Describe senses or use all"},
    {"select", " all | clear | START END", "Select a UTF-8 byte range for writing tools"},
    {"find", " text", "Find literal text across the manuscript"},
    {"replace", " OLD | NEW", "Replace literal text in the current scene and save a revision"},
    {"revisions", "", "Inspect saved drafts"},
    {"restore", " number", "Restore a prior draft and keep the current version"},
    {"bible", "", "Inspect canon and draft entries"},
    {"canon", " type | name | description", "Add a book-scoped canon entry"},
    {"promote", " entry ID", "Promote a Bible draft to canon"},
    {"context", "", "Inspect the assembled story sources"},
    {"budget", " 128–32768", "Set estimated source-token budget"},
    {"pin", " source ID", "Require a source in context"},
    {"exclude", " source ID", "Omit a source from context"},
    {"unpin", " source ID", "Restore default source inclusion"},
    {"premise", " text", "Set the current scene's premise"},
    {"control", " name | value", "Set a creative control on the current scene"},
    {"preferences", " text", "Set workspace-wide writing preferences"},
    {"import", " file", "Import a workspace, chat journal, or character card"},
    {"export", " file", "Write a new workspace backup"},
    {"save", "", "Save the workspace"},
    {"model", " [MODEL|recall CONN:MODEL]", "Select a server-configured narrator or recall model"},
    {"server", " [URL]", "Set the Neutrino inference server"},
    {"neofetch", "", "Show server status and generation performance"},
    {"focus", "", "Toggle the side panes"},
    {"quit", "", "Save and exit"}
};
const size_t command_count = sizeof commands / sizeof *commands;
static const char *const sense_names[] = {"sight", "sound", "smell", "touch", "taste", "metaphor"};
static const char *const bible_types[] = {"character", "location", "organization", "world-rule",
    "relationship", "timeline", "plot-thread", "object", "event", "lore"};
static volatile sig_atomic_t interrupted;

static void on_signal(int sig) { (void)sig; interrupted = 1; }

const char *ui_model(const App *a)
{
    const char *selected = js(a->ws.root, "serverNarrator");
    return *selected ? selected : "Server default";
}
const char *ui_endpoint(const App *a)
{
    if (a->server_override) return a->server_override;
    return env("NEUTRINO_SERVER", a->server_address.len ? str(&a->server_address) : "http://127.0.0.1:4783/v1");
}
Inference *app_inference(const App *a, const char *role, const Json *messages)
{
    Text route = {0};
    append(&route, role);
    const char *model = js(a->ws.root, "serverNarrator");
    if (!strcmp(role, "narrator") && *model) format(&route, "/%s", model);
    Inference *job = inference_start(ui_endpoint(a), env("NEUTRINO_SERVER_TOKEN", ""), str(&route), messages);
    release(&route);
    return job;
}
const char *ui_scene_name(const App *a) { return a->scene ? js(a->scene, "name") : "Scratchpad"; }
const char *ui_story_name(const App *a) { return a->story ? js(a->story, "title") : "New roleplay"; }
static const char *trim_left(const char *s)
{
    while (isspace((unsigned char)*s)) s++;
    return s;
}
static void status(App *a, const char *s) { clear(&a->status); append(&a->status, s); }
void app_status(App *a, const char *s) { status(a, s); }
static void save(App *a)
{
    if (!workspace_save(&a->ws)) status(a, "Save failed. Your changes are in memory; /export can write a backup.");
}
static void selection_save(App *a)
{
    Json *selection = cJSON_CreateObject();
    set(selection, "scene", js(a->scene, "path"));
    set(selection, "story", js(a->story, "id"));
    put(selection, "roleplay", cJSON_CreateBool(a->roleplay));
    put(a->ws.root, "neutrinoSelection", selection);
    a->ws.dirty = true;
}
static void show_scene(App *a, Json *scene)
{
    a->scene = scene;
    a->roleplay = false;
    a->scroll = 0;
    a->selected = false;
    a->editing = false;
    a->focus = COMPOSER;
    a->view_panel = false;
    selection_save(a);
    ui_clamp_rail(a);
}
static void show_story(App *a, Json *story)
{
    a->story = story;
    a->roleplay = true;
    a->scroll = INT_MAX;
    a->editing = false;
    a->focus = COMPOSER;
    a->view_panel = false;
    selection_save(a);
    ui_clamp_rail(a);
}
static void show_info(App *a, const char *title)
{
    snprintf(a->info_title, sizeof a->info_title, "%s", title);
    clear(&a->info);
    a->view_panel = true;
    a->panel_preview = false;
    a->panel_neofetch = false;
    a->panel_scroll = 0;
    a->focus = PANEL;
}
void app_info(App *a, const char *title) { show_info(a, title); }
static void show_preview(App *a)
{
    a->view_panel = true;
    a->panel_preview = true;
    a->panel_neofetch = false;
    a->panel_scroll = 0;
    a->focus = PANEL;
}
static void close_panel(App *a) { a->view_panel = false; a->focus = COMPOSER; }

static void show_neofetch(App *a)
{
    show_info(a, "Neofetch");
    a->panel_neofetch = true;
    a->neo.refresh_at = 0;
}

static void record_generation(App *a, const Candidate *candidate)
{
    if (!candidate->body.len || candidate->error.len) return;
    Neofetch *neo = &a->neo;
    neo->samples[neo->next] = candidate->metrics;
    neo->next = (neo->next + 1) % NEOFETCH_SAMPLES;
    if (neo->count < NEOFETCH_SAMPLES) neo->count++;
}

static void reset_neofetch_server(Neofetch *neo)
{
    if (neo->probe) {
        inference_cancel(neo->probe);
        neo->stale_probe = true;
    }
    cJSON_Delete(neo->server);
    neo->server = nullptr;
    clear(&neo->error);
    neo->refresh_at = 0;
}

static bool tick_neofetch(App *a)
{
    Neofetch *neo = &a->neo;
    const uint64_t now = monotonic_ms();
    const bool shown = a->view_panel && a->panel_neofetch;
    if (neo->probe) {
        Text body = {0}, reasoning = {0}, error = {0};
        bool done = inference_read(neo->probe, &body, &reasoning, &error);
        if (done) {
            inference_free(neo->probe);
            neo->probe = nullptr;
            if (!neo->stale_probe) {
                cJSON_Delete(neo->server);
                neo->server = cJSON_ParseWithOpts(str(&body), nullptr, true);
                clear(&neo->error);
                if (strcmp(js(neo->server, "service"), "neutrino")) {
                    cJSON_Delete(neo->server);
                    neo->server = nullptr;
                    append(&neo->error, error.len ? str(&error) : "Unrecognized status response.");
                }
                neo->refresh_at = now + 3000;
            }
            neo->stale_probe = false;
        }
        release(&body);
        release(&reasoning);
        release(&error);
        return done && shown;
    }
    if (shown && now >= neo->refresh_at) {
        neo->probe = inference_probe(ui_endpoint(a), env("NEUTRINO_SERVER_TOKEN", ""));
        neo->refresh_at = now + 3000;
        if (!neo->probe && !neo->error.len) {
            append(&neo->error, "Could not start the status check.");
            return true;
        }
    }
    return false;
}
static void close_preview(App *a)
{
    if (preview_busy(&a->preview)) { status(a, "Esc cancels generation."); return; }
    a->preview.open = false;
    close_panel(a);
}

static void show_help(App *a)
{
    show_info(a, "Keyboard & commands");
    append(&a->info,
        "F1  Help                     F2  Manuscript / roleplay\n"
        "F3  Library                  F4  Edit manuscript\n"
        "F5  Continue / narrate       F6  Context sources\n"
        "F7  Candidate review         F8  Accept candidate\n"
        "F9  Reject review            F10 Story Bible\n"
        "Ctrl+P  Command palette      Ctrl+Q  Save and quit\n"
        "Page Up / Down  Scroll       Esc  Close or cancel\n\n"
        "In the editor, Ctrl+S saves, Esc discards the editing buffer,\n"
        "and Ctrl+Space sets the selection anchor. Move the caret to\n"
        "extend the range, then save to use it with a writing tool.\n\n");
    for (size_t i = 0; i < command_count; i++)
        format(&a->info, "/%s%s\n  %s\n", commands[i].name, commands[i].args, commands[i].summary);
}

static void edit_start(App *a)
{
    if (a->roleplay) { status(a, "Switch to Manuscript with F2 to edit."); return; }
    const char *current = scene_text(&a->ws, a->scene);
    if (strlen(current) > EDIT_LIMIT) { status(a, "This imported scene exceeds the edit limit."); return; }
    close_panel(a);
    clear(&a->edit);
    for (const char *s = current; *s; s++)
        if (*s != '\r' || s[1] != '\n') append_n(&a->edit, s, 1);
    a->focus = EDITOR;
    a->editing = true;
    a->caret = 0;
    a->mark = SIZE_MAX;
    a->scroll = 0;
    status(a, "Editing manuscript");
}
static void edit_save(App *a)
{
    if (!scene_edit(&a->ws, a->scene, str(&a->edit), "edit")) {
        status(a, "The manuscript exceeds the editing limit.");
        return;
    }
    a->selected = a->mark != SIZE_MAX && a->mark != a->caret;
    if (a->selected) {
        a->selection_a = a->mark < a->caret ? a->mark : a->caret;
        a->selection_b = a->mark > a->caret ? a->mark : a->caret;
    }
    a->editing = false;
    a->focus = COMPOSER;
    status(a, a->selected ? "Saved · selection ready for a writing tool" : "Manuscript saved");
    save(a);
}

static void insert_bytes(Text *t, size_t *pos, const char *bytes, size_t n)
{
    if (t->len + n > EDIT_LIMIT) return;
    size_t old = t->len;
    append_n(t, bytes, n);
    memmove(t->data + *pos + n, t->data + *pos, old - *pos);
    memcpy(t->data + *pos, bytes, n);
    *pos += n;
}
static void remove_bytes(Text *t, size_t start, size_t end)
{
    if (end <= start) return;
    memmove(t->data + start, t->data + end, t->len - end + 1);
    t->len -= end - start;
}
static size_t line_start(const Text *t, size_t pos)
{
    while (pos && t->data[pos - 1] != '\n') pos--;
    return pos;
}
static size_t line_end(const Text *t, size_t pos)
{
    while (pos < t->len && t->data[pos] != '\n') pos++;
    return pos;
}
static void edit_key(App *a, int key)
{
    Text *t = &a->edit;
    size_t old_len = t->len, old_pos = a->caret, *pos = &a->caret;
    switch (key) {
        case 19: edit_save(a); return;
        case 27:
            a->editing = false;
            a->focus = COMPOSER;
            a->scroll = 0;
            status(a, "Editing buffer closed");
            return;
        case KEY_MARK: a->mark = a->mark == SIZE_MAX ? *pos : SIZE_MAX; return;
        case KEY_LEFT: *pos = utf_prev(str(t), *pos); return;
        case KEY_RIGHT: *pos = utf_next(str(t), t->len, *pos); return;
        case KEY_HOME: *pos = line_start(t, *pos); return;
        case KEY_END: *pos = line_end(t, *pos); return;
        case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN:
            *pos = ui_move_editor(a, key == KEY_UP ? -1 : key == KEY_DOWN ? 1 :
               key == KEY_PGUP ? -12 : 12);
            return;
        case 8: case 127:
            *pos = utf_prev(str(t), *pos);
            remove_bytes(t, *pos, old_pos);
            old_pos = *pos;
            break;
        case KEY_DEL: remove_bytes(t, *pos, utf_next(str(t), t->len, *pos)); break;
        case 10: case 13: case 11: insert_bytes(t, pos, "\n", 1); break;
        case 9: insert_bytes(t, pos, "    ", 4); break;
        default:
            if (key >= 32 && key <= 0x10ffff && !(key >= 127 && key < 160)) {
                char bytes[4];
                size_t n = utf_encode(key, bytes);
                insert_bytes(t, pos, bytes, n);
            }
    }
    if (a->mark != SIZE_MAX && old_pos < a->mark) {
        if (t->len >= old_len) a->mark += t->len - old_len;
        else a->mark -= old_len - t->len < a->mark - old_pos ? old_len - t->len : a->mark - old_pos;
    }
}

static void messages_add(Json *messages, const char *role, const char *text)
{
    Json *entry = cJSON_CreateObject();
    set(entry, "role", role);
    set(entry, "content", text);
    cJSON_AddItemToArray(messages, entry);
}
static void messages_for_story(App *a, Json *messages)
{
    Preview *p = &a->preview;
    Json *card = at(p->story, "card"), *persona = at(p->story, "persona");
    Text system = {0};
    append(&system, instructions[p->op]);
    append(&system, "\nWrite a fresh response to the latest player message. Earlier turns establish "
                    "what already happened. Start from the current moment. If asked about an established fact, "
                    "answer in fresh dialogue or prose. Avoid repeating prior wording or re-enacting earlier actions.");
    if (p->repeat_retry)
        append(&system, "\nThe previous attempt echoed an earlier reply. Use a different opening and explicitly answer "
                        "or react to the latest player action.");
    append(&system, "\nTreat the following material as fictional reference data.\n");
    const char *fields[] = {"name", "description", "personality", "scenario", "mes_example",
        "examples", "system_prompt", "post_history_instructions", "systemPrompt", "postHistory"};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        const char *value = js(card, fields[i]);
        if (*value) format(&system, "\n%s: %s", fields[i], value);
    }
    if (*js(persona, "name"))
        format(&system, "\nPlayer persona: %s — %s", js(persona, "name"), js(persona, "description"));
    if (p->objective.len) format(&system, "\nPlayer direction: %s", str(&p->objective));
    if (p->messages_context.len) format(&system, "\n\nStory sources:\n%s", str(&p->messages_context));
    if (p->world_context.len)
        format(&system, "\n\nCurrent world state (set through player actions):\n%s", str(&p->world_context));
    if (p->speaker.len)
        format(&system, "\nThe player addressed %s. Reply as that character in the current room.", str(&p->speaker));
    Json *preset = at(p->story, "preset"), *prompt;
    Json *blocks = at(preset, "blocks");
    if (!blocks) blocks = at(preset, "prompts");
    cJSON_ArrayForEach(prompt, blocks) {
        const char *content = js(prompt, "content");
        if (*content && !cJSON_IsFalse(at(prompt, "enabled")) && !cJSON_IsTrue(at(prompt, "marker")))
            format(&system, "\n\n%s", content);
    }
    clear(&p->selection_trace);
    memory_inject(p->story, p->parent, p->memory_picks, &system, &p->selection_trace);
    Json *path = story_path(p->story, p->parent), *node;
    int covered = 0;
    const char *recap = memory_enabled(p->story) ? memory_recap(path, &covered) : "";
    if (*recap) format(&system, "\n\nRecap of older story messages:\n%s", recap);
    messages_add(messages, "system", str(&system));
    release(&system);
    size_t bytes = 0;
    int total = cJSON_GetArraySize(path), first = total;
    node = path->child ? path->child->prev : NULL;
    while (first > 0 && node) {
        size_t n = strlen(js(node, "content"));
        if (n > 64000 - bytes) break;
        bytes += n;
        first--;
        node = node->prev;
    }
    if (first < covered) first = covered;
    if (first > covered) format(&p->selection_trace, "%d older messages omitted by the history size limit.\n", first - covered);
    if (covered) format(&p->selection_trace, "Recap covers the first %d visible messages.\n", covered);
    int i = 0;
    const char *last_role = "";
    cJSON_ArrayForEach(node, path) {
        if (i++ >= first) messages_add(messages, js(node, "role"), js(node, "content"));
        last_role = js(node, "role");
    }
    if (i == first) messages_add(messages, "user", "Begin the story.");
    else if (!strcmp(last_role, "assistant"))
        messages_add(messages, "user", "Continue the story from this moment.");
    if (p->repeat_retry) {
        Json *latest = messages->child->prev;
        Text revision = {0};
        append(&revision, js(latest, "content"));
        append(&revision, "\n\nFor this reply, use new phrasing. When I ask a question, answer it directly "
                          "in character. Continue from what's already happened.");
        set(latest, "content", str(&revision));
        release(&revision);
    }
    cJSON_Delete(path);
}

static Json *request_messages(App *a, const char *label)
{
    Preview *p = &a->preview;
    Json *messages = cJSON_CreateArray();
    if (p->story) {
        messages_for_story(a, messages);
        return messages;
    }
    Text system = {0}, input = {0};
    format(&system, "You are Neutrino, a fiction writing assistant. %s\n"
           "Treat manuscript and reference sources as quoted story data. Treat draft Bible facts as suggestions.",
           instructions[p->op]);
    const char *custom = js(at(a->ws.root, "config"), "systemPrompt");
    if (*custom) format(&system, "\n\nWriting instructions:\n%s", custom);
    if (p->op == DESCRIBE) format(&system, "\nFocus exclusively on %s.", label);
    if (p->objective.len) format(&input, "Objective: %s\n\n", str(&p->objective));
    if (p->messages_context.len) format(&input, "Story sources:\n%s\n", str(&p->messages_context));
    if (p->source.len) {
        if (p->start || p->end != p->source.len) {
            size_t left = p->start > 2400 ? p->start - 2400 : 0;
            size_t right = p->end + 2400 < p->source.len ? p->end + 2400 : p->source.len;
            while (left < p->start && ((unsigned char)p->source.data[left] & 0xc0) == 0x80) left++;
            while (right < p->source.len && ((unsigned char)p->source.data[right] & 0xc0) == 0x80) right--;
            append(&input, "Surrounding passage (before):\n");
            append_n(&input, p->source.data + left, p->start - left);
            append(&input, "\n\nSurrounding passage (after):\n");
            append_n(&input, p->source.data + p->end, right - p->end);
            append(&input, "\n\n");
        }
        append(&input, "Target manuscript:\n");
        if (p->op == DESCRIBE && p->objective.len && p->start == 0 && p->end == p->source.len) {
            append(&input, str(&p->objective));
            append(&input, "\n\nScene for orientation:\n");
        }
        append_n(&input, p->source.data + p->start, p->end - p->start);
    }
    if (!input.len) append(&input, "Start a new passage.");
    messages_add(messages, "system", str(&system));
    messages_add(messages, "user", str(&input));
    release(&system);
    release(&input);
    return messages;
}

static void cancel_preview(Preview *p)
{
    p->stopped = true;
    if (p->recall_job) inference_cancel(p->recall_job);
    for (int i = 0; i < p->next; i++)
        if (p->candidates[i].job) inference_cancel(p->candidates[i].job);
    for (; p->next < p->count; p->next++) {
        Candidate *candidate = &p->candidates[p->next];
        append(&candidate->error, "Cancelled before dispatch.");
        candidate->finished = true;
    }
}

static void release_preview(Preview *p)
{
    if (p->recall_job) inference_free(p->recall_job);
    for (size_t i = 0; i < sizeof p->candidates / sizeof *p->candidates; i++) {
        if (p->candidates[i].job) inference_free(p->candidates[i].job);
        release(&p->candidates[i].body);
        release(&p->candidates[i].thinking);
        release(&p->candidates[i].error);
    }
    release(&p->source);
    release(&p->messages_context);
    release(&p->objective);
    release(&p->lore_trace);
    release(&p->memory_trace);
    release(&p->recall_output);
    release(&p->recall_thinking);
    release(&p->recall_error);
    release(&p->previous_reply);
    release(&p->selection_trace);
    release(&p->world_context);
    release(&p->speaker);
    cJSON_Delete(p->memory_picks);
    *p = (Preview){0};
}
static void start_candidate(App *a, int index)
{
    Preview *p = &a->preview;
    Candidate *c = &p->candidates[index];
    Json *messages = request_messages(a, c->label);
    clear(&a->trace);
    format(&a->trace, "%s · %s · %s\n", operation_names[p->op], c->label, ui_model(a));
    if (!p->story && p->count > 1) format(&a->trace, "Last dispatched candidate %d/%d\n", index + 1, p->count);
    if (p->story) {
        format(&a->trace, "Story: %s · parent #%d\n", js(p->story, "title"), p->parent);
        if (p->repeat_retry) append(&a->trace, "Second attempt · discarded a repeat of the previous reply.\n");
        format(&a->trace, "\nLore decisions\n%s\nCompendium\n%s%s",
            p->lore_trace.len ? str(&p->lore_trace) : "No lore entries attached.\n",
            str(&p->memory_trace), str(&p->selection_trace));
        if (p->world_context.len) format(&a->trace, "\nWorld state\n%s", str(&p->world_context));
        if (p->speaker.len) format(&a->trace, "Addressed character: %s\n", str(&p->speaker));
    }
    append(&a->trace, "\nMessages sent to the server (JSON, in order)\n\n");
    char *wire = cJSON_Print(messages);
    if (!wire) abort();
    append(&a->trace, wire);
    cJSON_free(wire);
    c->finished = false;
    c->job = app_inference(a, "narrator", messages);
    cJSON_Delete(messages);
    if (c->job) p->active++;
    else {
        append(&c->error, "Could not start inference. Check /server and the server process.");
        c->finished = true;
        if (p->story) {
            status(a, str(&c->error));
            p->complete = true;
        }
    }
}

static void dispatch_writing(App *a)
{
    Preview *p = &a->preview;
    enum { CANDIDATE_WORKERS = 3 };
    while (!p->stopped && p->active < CANDIDATE_WORKERS && p->next < p->count) {
        const int index = p->next++;
        start_candidate(a, index);
    }
    if (p->active || p->next != p->count) return;
    p->complete = true;
    bool ready = false, has_text = false;
    for (int i = 0; i < p->count; i++) {
        const Candidate *candidate = &p->candidates[i];
        if (!candidate->body.len) continue;
        has_text = true;
        if (!candidate->error.len) ready = true;
    }
    if (p->stopped) status(a, ready ? "Cancelled · completed candidates are available" :
        has_text ? "Generation cancelled · partial text is available for review" : "Generation cancelled");
    else status(a, ready ? "Review ready · ←/→ candidates · /accept or /reject" :
        p->candidates[0].error.len ? str(&p->candidates[0].error) : "The model returned no candidate text");
}

static bool read_candidate(Preview *p, Candidate *c)
{
    size_t before = c->body.len + c->thinking.len + c->error.len;
    if (!inference_read(c->job, &c->body, &c->thinking, &c->error))
        return before != c->body.len + c->thinking.len + c->error.len;
    c->metrics = inference_metrics(c->job);
    inference_free(c->job);
    c->job = nullptr;
    c->finished = true;
    p->active--;
    return true;
}
static bool begin_recall(App *a)
{
    Preview *p = &a->preview;
    if (!memory_enabled(p->story)) return false;
    const char *mode = memory_mode(p->story);
    const Json *head = story_pick(p->story, p->head);
    if (p->op == SWIPE && !p->objective.len && strcmp(mode, "lexical") && strcmp(mode, "off") &&
        cJSON_IsArray(at(head, "memoryRecallPicks"))) {
        p->memory_picks = cJSON_Duplicate(at(head, "memoryRecallPicks"), true);
        append(&p->memory_trace, "Recall reused from the previous swipe.\n");
        return false;
    }
    if (!strcmp(mode, "prefetch") || !strcmp(mode, "auto")) {
        MemoryLane *lane = &a->memory;
        int previous = !strcmp(js(story_pick(p->story, p->parent), "role"), "user") ?
            number(story_pick(p->story, p->parent), "parent") : p->parent;
        if (lane->cached_story == p->story &&
            (lane->cached_head == p->parent || lane->cached_head == previous) &&
            lane->cached_picks && !p->objective.len) {
            p->memory_picks = cJSON_Duplicate(lane->cached_picks, true);
            append(&p->memory_trace, "Used prefetched model recall and current lexical matches.\n");
        } else append(&p->memory_trace, "No matching prefetch. Used lexical recall.\n");
        return false;
    }
    if (!strcmp(mode, "sync")) {
        Json *messages = memory_recall_prompt(p->story, p->parent, str(&p->objective));
        if (messages) {
            p->recall_job = memory_start_recall(a, p->story, messages);
            cJSON_Delete(messages);
        }
        if (p->recall_job) {
            p->recalling = true;
            status(a, "Recalling story memory · Esc cancels");
            return true;
        }
        append(&p->memory_trace, "No model recall available. Used lexical recall.\n");
    } else format(&p->memory_trace, "Recall mode: %s (no model call).\n", mode);
    return false;
}
static void remember_previous_reply(Preview *p)
{
    Json *head = story_pick(p->story, p->head);
    if (p->op == SWIPE && !strcmp(js(head, "role"), "assistant")) {
        normalize_words(&p->previous_reply, js(head, "content"));
        return;
    }
    Json *path = story_path(p->story, p->parent), *node;
    for (node = path->child ? path->child->prev : NULL; node;) {
        if (!strcmp(js(node, "role"), "assistant")) {
            normalize_words(&p->previous_reply, js(node, "content"));
            break;
        }
        node = node == path->child ? NULL : node->prev;
    }
    cJSON_Delete(path);
}

static bool repeats_reply(const Preview *p, const Text *body)
{
    size_t old = p->previous_reply.len;
    if (!old) return false;
    Text draft = {0};
    normalize_words(&draft, str(body));
    bool exact = old == draft.len && !strcmp(str(&p->previous_reply), str(&draft));
    bool replay = old >= 80 && old < draft.len && draft.len - old <= old / 5 &&
        draft.data[old] == ' ' && !memcmp(p->previous_reply.data, draft.data, old);
    release(&draft);
    return exact || replay;
}

static void begin(App *a, Operation op, const char *objective, int parent, const char *speaker)
{
    Preview *p = &a->preview;
    if (preview_busy(p)) { status(a, "A generation is already running. Esc cancels."); return; }
    if (op < NARRATE && a->roleplay) { status(a, "F2 switches to Manuscript for writing tools."); return; }
    if (op >= NARRATE && (!a->roleplay || !a->story)) { status(a, "Open a roleplay story first."); return; }
    release_preview(p);
    p->op = op;
    p->scene = a->scene;
    p->version = number(a->scene, "version");
    append(&p->objective, objective);
    if (op >= NARRATE) {
        p->story = a->story;
        p->parent = parent;
        p->head = number(a->story, "head");
        remember_previous_reply(p);
        if (!speaker && op == SWIPE) speaker = js(story_pick(p->story, p->head), "speaker");
        if (!speaker && op == NARRATE) speaker = js(story_pick(p->story, parent), "addressed");
        if (speaker) append(&p->speaker, speaker);
        world_context(p->story, parent, &p->world_context);
        if (p->world_context.len > 32000) {
            status(a, "World context exceeds the size limit for this request.");
            return;
        }
    } else {
        const char *source = scene_text(&a->ws, a->scene);
        size_t source_size = strlen(source);
        if (source_size > EDIT_LIMIT) { status(a, "This scene exceeds the generation input limit."); return; }
        append(&p->source, source);
        p->end = p->source.len;
        if (a->selected && (op == REWRITE || op == EXPAND || op == DESCRIBE || op == CRITIQUE || op == DRAFT)) {
            p->start = a->selection_a;
            p->end = a->selection_b;
        }
        p->insert_at = p->end;
        if (op == DESCRIBE && !p->source.len && !p->objective.len) {
            status(a, "Select a passage or provide a subject to describe.");
            return;
        }
    }
    if (!collect_context(&a->ws, p->story ? NULL : p->scene, p->story, p->parent,
                         &p->messages_context, &p->lore_trace)) {
        status(a, "Required context exceeds the budget. Raise /budget or unpin a source.");
        return;
    }
    int count = number(a->ws.root, "neutrinoCandidates");
    if (count < 1 || count > 5 || op >= NARRATE) count = 1;
    if (op == DESCRIBE) {
        Json *senses = at(a->ws.root, "describeSenses");
        for (size_t i = 0; i < sizeof sense_names / sizeof *sense_names; i++) {
            bool enabled = !cJSON_IsArray(senses) || !senses->child;
            Json *sense;
            cJSON_ArrayForEach(sense, senses)
                if (cJSON_IsString(sense) && !strcmp(sense->valuestring, sense_names[i])) enabled = true;
            if (enabled) for (int j = 0; j < count; j++)
                snprintf(p->candidates[p->count++].label, sizeof p->candidates[0].label,
                         "%s", sense_names[i]);
        }
    }
    if (!p->count) {
        p->count = count;
        for (int i = 0; i < count; i++)
            snprintf(p->candidates[i].label, sizeof p->candidates[i].label, "%s", operation_names[op]);
    }
    p->open = true;
    if (!p->story) show_preview(a);
    memory_pause(a);
    if (p->story && begin_recall(a)) return;
    status(a, "Generating · Esc cancels");
    if (p->story) {
        p->next = 1;
        start_candidate(a, 0);
    } else dispatch_writing(a);
}

static bool update_generation(App *a)
{
    Preview *p = &a->preview;
    if (!preview_busy(p)) return false;
    if (p->recalling) {
        if (!inference_read(p->recall_job, &p->recall_output, &p->recall_thinking, &p->recall_error)) return false;
        inference_free(p->recall_job);
        p->recall_job = nullptr;
        p->recalling = false;
        if (p->stopped || p->head != number(p->story, "head")) {
            p->open = false;
            p->complete = true;
            status(a, p->stopped ? "Generation cancelled" : "The active branch moved during recall");
            return true;
        }
        if (!p->recall_error.len) p->memory_picks = memory_parse_picks(&p->recall_output);
        append(&p->memory_trace, p->memory_picks ? "Used synchronous model recall and lexical matches.\n" :
            "Model recall failed or returned invalid IDs. Used lexical recall.\n");
        status(a, "Generating · Esc cancels");
        p->next = 1;
        start_candidate(a, 0);
        return true;
    }
    if (!p->story) {
        bool changed = false;
        int active = p->active;
        for (int i = 0; i < p->next; i++) {
            Candidate *candidate = &p->candidates[i];
            if (candidate->job && read_candidate(p, candidate)) {
                if (candidate->finished) record_generation(a, candidate);
                changed = true;
            }
        }
        if (p->active != active) dispatch_writing(a);
        return changed;
    }
    Candidate *c = &p->candidates[0];
    bool changed = read_candidate(p, c);
    if (!c->finished) return changed;
    if (c->error.len || p->stopped) status(a, c->error.len ? str(&c->error) : "Generation cancelled");
    else {
        if (p->head == number(p->story, "head")) {
            if (repeats_reply(p, &c->body)) {
                if (!p->repeat_retry) {
                    p->repeat_retry = true;
                    clear(&c->body);
                    clear(&c->thinking);
                    status(a, "Repeated reply discarded · retrying");
                    start_candidate(a, 0);
                } else {
                    status(a, "The model repeated itself twice. /narrate retries the uncompleted turn.");
                    p->complete = true;
                }
                return true;
            }
            int id = story_push(&a->ws, p->story, p->parent, "assistant", str(&c->body));
            record_generation(a, c);
            Json *reply = story_pick(p->story, id);
            if (c->thinking.len) set(reply, "reasoning", str(&c->thinking));
            if (p->speaker.len) set(reply, "speaker", str(&p->speaker));
            if (p->memory_picks) put(reply, "memoryRecallPicks", cJSON_Duplicate(p->memory_picks, true));
            memory_schedule(a, p->story, id);
            p->open = false;
            if (a->roleplay && a->story == p->story) a->scroll = INT_MAX;
            status(a, p->op == SWIPE ? "Alternate reply saved · /previous or /branch" : "Reply saved");
            save(a);
        } else status(a, "The active branch moved. The reply is available in F7.");
    }
    p->complete = true;
    return true;
}

static const char *book_scope(const App *a, char path[1025])
{
    const char *source = a->roleplay ? js(a->story, "scene") : js(a->scene, "path");
    if (*source != '/') return "/";
    const char *folder = strchr(source + 1, '/');
    const char *book = folder ? strchr(folder + 1, '/') : NULL;
    if (!book || (size_t)(book - source) > 1024) return "/";
    memcpy(path, source, (size_t)(book - source));
    path[book - source] = 0;
    return path;
}
static bool bible_type(const char *s)
{
    for (size_t i = 0; i < sizeof bible_types / sizeof *bible_types; i++)
        if (!strcmp(s, bible_types[i])) return true;
    return false;
}
static void bible_add(App *a, const char *type, const char *name, const char *desc, const char *state)
{
    Json *entry = cJSON_CreateObject(), *bible = at(a->ws.root, "bible");
    char scope[1025], id[64];
    int serial = cJSON_GetArraySize(bible);
    bool unique;
    do {
        snprintf(id, sizeof id, "note-%d", ++serial);
        unique = true;
        Json *other;
        cJSON_ArrayForEach(other, bible) if (!strcmp(js(other, "id"), id)) unique = false;
    } while (!unique);
    set(entry, "id", id);
    set(entry, "book", book_scope(a, scope));
    set(entry, "type", type);
    set(entry, "name", name);
    set(entry, "description", desc);
    set(entry, "status", state);
    cJSON_AddItemToArray(bible, entry);
    a->ws.dirty = true;
}
static void view_bible(App *a)
{
    show_info(a, "Story Bible");
    Json *entry;
    cJSON_ArrayForEach(entry, at(a->ws.root, "bible")) {
        format(&a->info, "%s  ·  %s  ·  %s\n%s  %s\n%s\n\n", js(entry, "name"), js(entry, "type"),
               js(entry, "status"), js(entry, "id"), js(entry, "book"), js(entry, "description"));
    }
    if (!a->info.len) append(&a->info, "No entries yet. Add one with /canon or propose drafts with /extract.");
}
static void view_context(App *a)
{
    show_info(a, "Context inspector");
    Json *story = a->roleplay ? a->story : NULL;
    bool fits = collect_context(&a->ws, a->roleplay ? NULL : a->scene, story,
                                number(story, "head"), &a->info, NULL);
    if (!fits) append(&a->info, "Required or pinned sources exceed the budget.\n\n");
    if (!a->info.len) append(&a->info, "No supplemental sources. Add a premise, creative controls or Story Bible entry.\n\n");
    else format(&a->info, "Source estimate: %zu tokens\n\n", (a->info.len + 3) / 4);
    append(&a->info, "Source IDs for /pin and /exclude\n");
    Json *entry;
    cJSON_ArrayForEach(entry, at(a->ws.root, "bible"))
        format(&a->info, "%s  %s\n", js(entry, "id"), js(entry, "name"));
    cJSON_ArrayForEach(entry, at(a->ws.root, "records"))
        if (!strcmp(js(entry, "type"), "scene")) format(&a->info, "%s  %s\n", js(entry, "id"), js(entry, "path"));
}

static void accept(App *a, bool insert)
{
    Preview *p = &a->preview;
    if (preview_busy(p) || !p->open || !p->complete) { status(a, "Wait for a completed candidate."); return; }
    Candidate *c = &p->candidates[p->current];
    if (!c->finished || !c->body.len || c->error.len) {
        status(a, "This candidate did not complete. Use /preview-save to keep partial text.");
        return;
    }
    if (p->story) { status(a, "Use /preview-save to keep this off-branch reply."); return; }
    if (a->scene != p->scene || a->roleplay || a->editing ||
        p->version != number(a->scene, "version") || strcmp(scene_text(&a->ws, a->scene), str(&p->source))) {
        status(a, "The source scene changed. Generate a new preview or export this candidate.");
        return;
    }
    if (p->op == EXTRACT) {
        const char *start = trim_left(str(&c->body));
        Text payload = {0};
        if (!strncmp(start, "\x60\x60\x60", 3)) {
            const char *open = strchr(start, '\n');
            const char *close = open ? strstr(open + 1, "\x60\x60\x60") : NULL;
            if (close) append_n(&payload, open + 1, (size_t)(close - open - 1));
            start = str(&payload);
        }
        Json *items = cJSON_ParseWithOpts(start, NULL, 1), *item;
        bool valid = cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0 && cJSON_GetArraySize(items) <= 200;
        cJSON_ArrayForEach(item, items)
            if (!*js(item, "name") || !*js(item, "description") || !bible_type(js(item, "type"))) valid = false;
        if (valid) cJSON_ArrayForEach(item, items)
            bible_add(a, js(item, "type"), js(item, "name"), js(item, "description"), "draft");
        cJSON_Delete(items);
        release(&payload);
        if (!valid) { status(a, "This candidate is not a valid array of Bible drafts."); return; }
        close_preview(a);
        view_bible(a);
        status(a, "Drafts added · /promote marks a reviewed entry as canon");
        return;
    }
    if (p->op == BRAINSTORM || p->op == CRITIQUE || p->op == CONTINUITY) {
        status(a, "Analysis is read-only. /preview-save writes it to a text file.");
        return;
    }
    if (insert && p->op != DESCRIBE) { status(a, "/insert applies to Describe candidates."); return; }
    size_t start = p->start, end = p->end;
    Text replacement = {0};
    if (p->op == CONTINUE) start = end = p->source.len;
    else if (insert) start = end = p->insert_at;
    append_n(&replacement, str(&p->source), start);
    if (p->op == CONTINUE && start) append(&replacement, "\n\n");
    else if (insert && start && !isspace((unsigned char)p->source.data[start - 1])) append(&replacement, " ");
    append(&replacement, str(&c->body));
    append(&replacement, str(&p->source) + end);
    bool ok = scene_edit(&a->ws, a->scene, str(&replacement), operation_names[p->op]);
    release(&replacement);
    if (!ok) { status(a, "The result exceeds the manuscript limit."); return; }
    if (insert) {
        size_t change = strlen(scene_text(&a->ws, a->scene)) - p->source.len;
        p->insert_at += change;
        p->version = number(a->scene, "version");
        clear(&p->source);
        append(&p->source, scene_text(&a->ws, a->scene));
        a->selected = false;
        status(a, "Detail inserted · choose another variation or Esc");
    } else {
        a->selected = false;
        close_preview(a);
        status(a, "Candidate applied and saved");
    }
}

static int parse_number(const char *s, int min, int max)
{
    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);
    return end != s && !*trim_left(end) && !errno && value >= min && value <= max ? (int)value : -1;
}
static char *trim_piece(char *s)
{
    s = (char *)trim_left(s);
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}
static char *split_piece(char **rest, char sep)
{
    char *piece = *rest;
    if (!piece) return NULL;
    char *next = strchr(piece, sep);
    if (next) *next++ = 0;
    *rest = next;
    return trim_piece(piece);
}
static void show_branches(App *a)
{
    if (!a->story) { status(a, "Open a roleplay story first."); return; }
    show_info(a, "Roleplay branches");
    format(&a->info, "%s  ·  /branch <ID> to switch\n\n", ui_story_name(a));
    Json *node;
    int head = number(a->story, "head");
    cJSON_ArrayForEach(node, at(a->story, "nodes")) {
        int id = number(node, "id");
        const char *content = js(node, "content");
        size_t n = strlen(content), cap = 160;
        if (n > cap) {
            while (cap && ((unsigned char)content[cap] & 0xc0) == 0x80) cap--;
            n = cap;
        }
        format(&a->info, "%s #%d  ← #%d  %s%s\n", id == head ? "●" : "○",
               id, number(node, "parent"), js(node, "role"),
               cJSON_IsTrue(at(node, "hidden")) ? " (hidden)" : "");
        append_n(&a->info, content, n);
        append(&a->info, "\n\n");
    }
}
static void change_branch(App *a, int id)
{
    if (!a->story || (id != 0 && !story_pick(a->story, id))) { status(a, "Unknown message ID."); return; }
    integer(a->story, "head", id);
    a->ws.dirty = true;
    a->scroll = INT_MAX;
    close_panel(a);
    status(a, id ? "Active branch changed" : "Rewound to the start of the story");
}
static void rewind_story(App *a, const char *arg)
{
    if (!a->roleplay || !a->story) { status(a, "Open a roleplay story first."); return; }
    if (preview_busy(&a->preview)) { status(a, "Finish or cancel generation before rewinding."); return; }
    int head = number(a->story, "head");
    if (!head) { status(a, "Already at the start of the story."); return; }
    int target = *arg ? parse_number(arg, 0, INT_MAX) : 0;
    if (target < 0) { status(a, "Use /rewind [ancestor ID]."); return; }
    for (int id = number(story_pick(a->story, head), "parent"); id > 0 && id < head;) {
        Json *node = story_pick(a->story, id);
        if (!node) break;
        if ((*arg && id == target) ||
            (!*arg && !strcmp(js(node, "role"), "assistant") && !cJSON_IsTrue(at(node, "hidden")))) {
            change_branch(a, id);
            return;
        }
        int parent = number(node, "parent");
        if (parent >= id) break;
        id = parent;
    }
    if (!target) change_branch(a, 0);
    else status(a, "Choose an earlier message on this branch.");
}
static void search_story(App *a, const char *query)
{
    if (!a->roleplay || !a->story) { status(a, "Open a roleplay story first."); return; }
    if (!*query) { status(a, "Use /search TEXT."); return; }
    show_info(a, "Search this branch");
    Json *path = story_path(a->story, number(a->story, "head"));
    int matches = 0;
    for (Json *node = path->child ? path->child->prev : nullptr; node;
         node = node == path->child ? nullptr : node->prev) {
        const char *content = js(node, "content");
        const char *hit = strstr(content, query);
        if (!hit) continue;
        if (++matches > 40) {
            append(&a->info, "More matches omitted. Narrow your search.\n");
            break;
        }
        const size_t len = strlen(content), offset = (size_t)(hit - content);
        size_t start = offset > 55 ? offset - 55 : 0;
        while (start < offset && ((unsigned char)content[start] & 0xc0) == 0x80) start++;
        size_t end = len > start + 220 ? start + 220 : len;
        while (end > start && ((unsigned char)content[end] & 0xc0) == 0x80) end--;
        format(&a->info, "#%d  %s\n%s", number(node, "id"), js(node, "role"), start ? "…" : "");
        append_n(&a->info, content + start, end - start);
        append(&a->info, end < len ? "…\n\n" : "\n\n");
    }
    if (!matches) append(&a->info, "No matching messages on the active path.");
    else append(&a->info, "/branch ID opens a matching message.");
    cJSON_Delete(path);
}
static void sibling(App *a, bool next)
{
    int head = number(a->story, "head");
    Json *current = story_pick(a->story, head), *node;
    if (!current) { status(a, "No reply variants yet."); return; }
    int target = 0;
    cJSON_ArrayForEach(node, at(a->story, "nodes")) {
        int id = number(node, "id");
        if (number(node, "parent") != number(current, "parent") ||
            strcmp(js(node, "role"), js(current, "role")) || cJSON_IsTrue(at(node, "hidden"))) continue;
        if (!next && id < head) target = id;
        if (next && id > head) { target = id; break; }
    }
    if (target) change_branch(a, target);
    else status(a, next ? "At the newest variant" : "At the oldest variant");
}

static void view_revisions(App *a)
{
    if (!a->scene || a->roleplay) { status(a, "Open a manuscript scene first."); return; }
    show_info(a, "Saved drafts");
    Json *item;
    int i = 0;
    cJSON_ArrayForEach(item, at(a->scene, "revisions")) {
        format(&a->info, "%d  ·  %s  ·  %s\n", ++i, js(item, "at"), js(item, "reason"));
        format(&a->info, "%.160s\n\n", js(item, "content"));
    }
    if (!i) append(&a->info, "No earlier versions. Saved edits and accepted candidates create revisions.");
    else append(&a->info, "/restore <number> restores a prior draft.");
}
static void replace_scene_text(App *a, char *arg)
{
    if (!a->scene || a->roleplay) { status(a, "Open a manuscript scene first."); return; }
    char *replacement = arg, *needle = split_piece(&replacement, '|');
    if (!replacement || !*needle) { status(a, "Use /replace OLD | NEW. Leave NEW empty to remove OLD."); return; }
    replacement = trim_piece(replacement);
    if (!strcmp(needle, replacement)) { status(a, "The replacement is identical."); return; }
    const size_t old_len = strlen(needle), new_len = strlen(replacement);
    const char *start = scene_text(&a->ws, a->scene);
    Text edited = {0};
    size_t count = 0;
    const char *match;
    while ((match = strstr(start, needle))) {
        const size_t span = (size_t)(match - start);
        if (span + new_len > EDIT_LIMIT - edited.len) break;
        append_n(&edited, start, span);
        append_n(&edited, replacement, new_len);
        start = match + old_len;
        count++;
    }
    if (match || strlen(start) > EDIT_LIMIT - edited.len) status(a, "The replacement exceeds the manuscript limit.");
    else if (!count) status(a, "No matching text in this scene.");
    else {
        append(&edited, start);
        if (scene_edit(&a->ws, a->scene, str(&edited), "replace")) {
            a->selected = false;
            a->scroll = 0;
            close_panel(a);
            char message[80];
            snprintf(message, sizeof message, "Replaced %zu %s · /revisions to review", count,
                     count == 1 ? "match" : "matches");
            status(a, message);
        }
    }
    release(&edited);
}

static void configure_senses(App *a, const char *arg)
{
    Json *senses = cJSON_CreateArray();
    char *buf = copy(arg), *rest = buf, *piece;
    bool all = !strcmp(arg, "all"), valid = true;
    while (!all && (piece = split_piece(&rest, ',')) != NULL) {
        bool known = false, repeated = false;
        for (size_t i = 0; i < sizeof sense_names / sizeof *sense_names; i++)
            if (!strcmp(piece, sense_names[i])) known = true;
        Json *sense;
        cJSON_ArrayForEach(sense, senses) if (!strcmp(piece, sense->valuestring)) repeated = true;
        if (!known) valid = false;
        else if (!repeated) cJSON_AddItemToArray(senses, cJSON_CreateString(piece));
    }
    if (all) for (size_t i = 0; i < sizeof sense_names / sizeof *sense_names; i++)
        cJSON_AddItemToArray(senses, cJSON_CreateString(sense_names[i]));
    if (valid && senses->child) {
        put(a->ws.root, "describeSenses", senses);
        a->ws.dirty = true;
        status(a, "Describe senses saved");
    } else {
        cJSON_Delete(senses);
        status(a, "Choose all or a comma-separated list of sight,sound,smell,touch,taste,metaphor.");
    }
    free(buf);
}

static void set_selection(App *a, const char *arg)
{
    if (a->roleplay) { status(a, "Switch to Manuscript with F2 to select prose."); return; }
    const char *source = scene_text(&a->ws, a->scene);
    size_t len = strlen(source);
    if (!strcmp(arg, "clear")) { a->selected = false; status(a, "Selection cleared"); return; }
    if (!strcmp(arg, "all")) {
        a->selection_a = 0;
        a->selection_b = len;
    } else {
        char *middle, *end;
        long left = strtol(arg, &middle, 10);
        long right = strtol(middle, &end, 10);
        if (middle == arg || end == middle || *trim_left(end) || left < 0 || right <= left ||
            (size_t)right > len || ((unsigned char)source[left] & 0xc0) == 0x80 ||
            ((unsigned char)source[right] & 0xc0) == 0x80) {
            status(a, "/select expects two ordered UTF-8 byte boundaries or all.");
            return;
        }
        a->selection_a = (size_t)left;
        a->selection_b = (size_t)right;
    }
    a->selected = a->selection_a != a->selection_b;
    status(a, a->selected ? "Manuscript selection set" : "Manuscript is empty");
}

static void attach_story_file(App *a, const char *field, const char *path)
{
    if (!a->roleplay || !a->story) { status(a, "Open a roleplay story first."); return; }
    if (preview_busy(&a->preview)) { status(a, "Wait for generation to finish."); return; }
    char *data = read_file(path);
    Json *value = data ? cJSON_ParseWithOpts(data, NULL, 1) : NULL;
    free(data);
    bool lore = !strcmp(field, "lore");
    bool valid = lore ? cJSON_IsArray(at(value, "entries")) || cJSON_IsObject(at(value, "entries")) :
        cJSON_IsArray(at(value, "blocks")) || cJSON_IsArray(at(value, "prompts"));
    if (!valid) {
        cJSON_Delete(value);
        status(a, lore ? "Expected a JSON lorebook with entries." : "Expected a JSON preset with blocks or prompts.");
        return;
    }
    put(a->story, field, value);
    a->ws.dirty = true;
    status(a, lore ? "Lorebook attached" : "Prompt preset attached");
}

static void import_file(App *a, const char *arg)
{
    if (a->preview.open || preview_busy(&a->preview)) { status(a, "Close the candidate review before importing."); return; }
    Text error = {0};
    Json *result = workspace_import(&a->ws, arg, &error);
    if (!result) status(a, str(&error));
    else if (result != a->ws.root) {
        show_story(a, result);
        status(a, "Roleplay imported");
    } else {
        Json *scene = ui_library_at(a, 1);
        a->story = cJSON_GetArrayItem(at(a->ws.root, "stories"), 0);
        show_scene(a, scene);
        status(a, "Workspace imported");
    }
    release(&error);
}

static void quit(App *a)
{
    if (preview_busy(&a->preview)) {
        cancel_preview(&a->preview);
        status(a, "Cancelling generation. Quit after the connection closes.");
    } else if (workspace_save(&a->ws)) a->quit = true;
    else status(a, "Save failed. /export can write a backup before closing.");
}

static void rewrite_preset(App *a, const char *arg)
{
    static const struct { const char *name, *objective; } presets[] = {
        {"clarity", "Clarify the action, references, and sequence while preserving the meaning."},
        {"concise", "Cut repetition and wordiness while preserving the essential facts and voice."},
        {"description", "Sharpen concrete sensory details without adding story facts."},
        {"dialogue", "Give each speaker a distinct voice and make the exchange sound natural."},
        {"emotion", "Convey the emotion through actions, sensations, and subtext."},
        {"pacing", "Improve the rhythm and transitions while preserving the order of events."},
        {"tone", "Make the tone consistent with the direction and the surrounding manuscript."}
    };
    if (!*arg) {
        show_info(a, "Rewrite presets");
        for (size_t i = 0; i < sizeof presets / sizeof *presets; i++)
            format(&a->info, "%s\n  %s\n", presets[i].name, presets[i].objective);
        append(&a->info, "\n/rewrite-preset NAME [additional direction]");
        return;
    }
    if (!a->roleplay && !*scene_text(&a->ws, a->scene)) {
        status(a, "Write or open a passage before applying a rewrite preset.");
        return;
    }
    size_t len = strcspn(arg, " \t");
    for (size_t i = 0; i < sizeof presets / sizeof *presets; i++) {
        if (strlen(presets[i].name) != len || strncmp(arg, presets[i].name, len)) continue;
        Text objective = {0};
        append(&objective, presets[i].objective);
        const char *direction = trim_left(arg + len);
        if (*direction) format(&objective, "\nAdditional direction: %s", direction);
        begin(a, REWRITE, str(&objective), 0, nullptr);
        release(&objective);
        return;
    }
    status(a, "Unknown preset. Use /rewrite-preset to list them.");
}

static void slash_command(App *a, char *line)
{
    char *arg = line;
    while (*arg && !isspace((unsigned char)*arg)) arg++;
    if (*arg) *arg++ = 0;
    arg = trim_piece(arg);
    if (a->ssh_session && (!strcmp(line, "import") || !strcmp(line, "export") ||
        !strcmp(line, "lore") || !strcmp(line, "preset") || !strcmp(line, "preview-save") ||
        !strcmp(line, "server") || (!strcmp(line, "trace") && *arg))) {
        status(a, "That command requires a local workspace session.");
        return;
    }
    const char *const world_commands[] = {"world", "room", "connect", "go", "item",
        "take", "drop", "npc", "weather", "say", "map", "inspect"};
    for (size_t i = 0; i < sizeof world_commands / sizeof *world_commands; i++) {
        if (!strcmp(line, world_commands[i])) { world_command(a, line, arg); return; }
    }
    for (int i = 0; i < NARRATE; i++) {
        char name[32];
        snprintf(name, sizeof name, "%s", operation_names[i]);
        name[0] = (char)tolower((unsigned char)name[0]);
        if (!strcmp(line, name)) { begin(a, (Operation)i, arg, 0, NULL); return; }
    }
    if (!strcmp(line, "help")) show_help(a);
    else if (!strcmp(line, "edit")) edit_start(a);
    else if (!strcmp(line, "rewrite-preset")) rewrite_preset(a, arg);
    else if (!strcmp(line, "rewind")) rewind_story(a, arg);
    else if (!strcmp(line, "search")) search_story(a, arg);
    else if (!strcmp(line, "replace")) replace_scene_text(a, arg);
    else if (!strcmp(line, "quit")) quit(a);
    else if (!strcmp(line, "save")) { status(a, "Workspace saved"); save(a); }
    else if (!strcmp(line, "focus")) a->compact = !a->compact;
    else if (!strcmp(line, "neofetch")) {
        if (*arg) status(a, "Use /neofetch without arguments.");
        else show_neofetch(a);
    }
    else if (!strcmp(line, "memory")) memory_command(a, arg);
    else if (!strcmp(line, "trace")) {
        if (!a->trace.len) status(a, "No model prompt has been prepared in this session.");
        else if (*arg) status(a, write_file(arg, str(&a->trace), false) ? "Prompt trace exported" :
                           "Trace export failed. Supply a new filename.");
        else {
            show_info(a, "Prompt trace");
            append(&a->info, str(&a->trace));
        }
    }
    else if (!strcmp(line, "scene")) {
        Json *scene = scene_add(&a->ws, arg);
        if (!scene) status(a, "Use /scene /folder/book/chapter/scene.");
        else { show_scene(a, scene); status(a, "Scene open · F4 to edit"); }
    } else if (!strcmp(line, "scratch")) show_scene(a, NULL);
    else if (!strcmp(line, "story") || !strcmp(line, "play")) {
        bool linked = !strcmp(line, "play") && a->scene;
        if (!*arg && !linked) { status(a, "Use /story <title> or open a scene for /play."); return; }
        Json *story = story_add(&a->ws, *arg ? arg : ui_scene_name(a), NULL);
        if (linked) set(story, "scene", js(a->scene, "path"));
        show_story(a, story);
        status(a, "Story created · write an action or use /narrate");
    } else if (!strcmp(line, "link")) {
        Json *scene = scene_find(&a->ws, arg);
        if (!a->story || !scene || strcmp(js(scene, "type"), "scene")) status(a, "Open a story and give an existing scene path.");
        else {
            set(a->story, "scene", arg);
            a->ws.dirty = true;
            status(a, "Scene linked");
        }
    } else if (!strcmp(line, "persona")) {
        char *rest = arg, *name = split_piece(&rest, '|');
        if (!a->story || !*name || !rest || !*trim_piece(rest)) status(a, "Use /persona name | description in a story.");
        else {
            Json *persona = cJSON_CreateObject();
            set(persona, "name", name);
            set(persona, "description", rest);
            put(a->story, "persona", persona);
            a->ws.dirty = true;
            status(a, "Persona saved");
        }
    } else if (!strcmp(line, "lore") || !strcmp(line, "preset")) attach_story_file(a, line, arg);
    else if (!strcmp(line, "branch")) {
        if (*arg) change_branch(a, parse_number(arg, 1, INT_MAX));
        else show_branches(a);
    } else if (!strcmp(line, "previous") || !strcmp(line, "next")) sibling(a, !strcmp(line, "next"));
    else if (!strcmp(line, "narrate")) begin(a, NARRATE, arg, number(a->story, "head"), NULL);
    else if (!strcmp(line, "swipe")) {
        Json *node = story_pick(a->story, number(a->story, "head"));
        if (!node || strcmp(js(node, "role"), "assistant")) status(a, "Select a narrator reply to swipe.");
        else begin(a, SWIPE, arg, number(node, "parent"), NULL);
    } else if (!strcmp(line, "transcript")) {
        if (!a->story || scene_find(&a->ws, arg) || !number(a->story, "head")) {
            status(a, "Open a story with messages and use a new scene path.");
            return;
        }
        Text transcript = {0};
        Json *path = story_path(a->story, number(a->story, "head")), *node;
        cJSON_ArrayForEach(node, path)
            format(&transcript, "%s\n%s\n\n", !strcmp(js(node, "role"), "user") ? "Player" :
                   *js(node, "speaker") ? js(node, "speaker") : "Narrator",
                   js(node, "content"));
        Json *scene = transcript.len <= EDIT_LIMIT ? scene_add(&a->ws, arg) : NULL;
        if (scene && scene_edit(&a->ws, scene, str(&transcript), "roleplay transcript")) {
            show_scene(a, scene);
            status(a, "Transcript saved as a new scene");
        } else status(a, "Use a valid scene path and a transcript under 2 MiB.");
        cJSON_Delete(path);
        release(&transcript);
    } else if (!strcmp(line, "preview") || !strcmp(line, "reasoning")) {
        Preview *p = &a->preview;
        int n = *arg ? parse_number(arg, 1, p->count) : p->current + 1;
        if (!p->open && a->roleplay && !strcmp(line, "reasoning")) {
            int head = number(a->story, "head");
            if (*arg) head = parse_number(arg, 1, INT_MAX);
            const char *reasoning = js(story_pick(a->story, head), "reasoning");
            show_info(a, "Reply reasoning");
            append(&a->info, *reasoning ? reasoning : "This reply has no saved provider reasoning.");
        } else if (!p->open || n < 1) status(a, "No matching candidate.");
        else {
            p->current = n - 1;
            if (!strcmp(line, "reasoning")) p->reasoning = !p->reasoning;
            show_preview(a);
        }
    } else if (!strcmp(line, "regenerate")) {
        Preview *p = &a->preview;
        if (!p->open || p->story || preview_busy(p)) status(a, "Open a completed writing preview first.");
        else {
            char *objective = copy(str(&p->objective));
            begin(a, p->op, objective, 0, NULL);
            free(objective);
        }
    } else if (!strcmp(line, "preview-save")) {
        Preview *p = &a->preview;
        const Text *body = p->open ? &p->candidates[p->current].body : NULL;
        status(a, body && body->len && *arg && write_file(arg, str(body), false) ? "Candidate exported" :
               "Export failed. Supply a new filename and choose a candidate with text.");
    } else if (!strcmp(line, "accept") || !strcmp(line, "insert")) accept(a, !strcmp(line, "insert"));
    else if (!strcmp(line, "reject")) close_preview(a);
    else if (!strcmp(line, "select")) set_selection(a, arg);
    else if (!strcmp(line, "senses")) configure_senses(a, arg);
    else if (!strcmp(line, "count") || !strcmp(line, "budget")) {
        bool budget = !strcmp(line, "budget");
        int n = parse_number(arg, budget ? 128 : 1, budget ? 32768 : 5);
        if (n < 0) status(a, budget ? "Enter a token estimate between 128 and 32768." : "Enter a candidate count between 1 and 5.");
        else {
            if (budget) cJSON_DeleteItemFromObjectCaseSensitive(a->ws.root, "neutrinoBudget");
            integer(a->ws.root, budget ? "contextBudget" : "neutrinoCandidates", n);
            a->ws.dirty = true;
            status(a, budget ? "Context budget saved" : "Candidate count saved");
        }
    } else if (!strcmp(line, "revisions")) view_revisions(a);
    else if (!strcmp(line, "restore")) {
        int n = parse_number(arg, 1, INT_MAX);
        Json *rev = cJSON_GetArrayItem(at(a->scene, "revisions"), n - 1);
        if (n < 1 || a->roleplay || !rev) status(a, "Use /revisions to find the draft number.");
        else if (scene_edit(&a->ws, a->scene, js(rev, "content"), "restore")) {
            a->selected = false;
            close_panel(a);
            status(a, "Draft restored");
        } else status(a, "This revision exceeds the manuscript editing limit.");
    } else if (!strcmp(line, "context")) view_context(a);
    else if (!strcmp(line, "bible")) view_bible(a);
    else if (!strcmp(line, "canon")) {
        char *rest = arg, *type = split_piece(&rest, '|'), *name = split_piece(&rest, '|');
        if (!name || !*name || !rest || !*trim_piece(rest) || !bible_type(type))
            status(a, "Use /canon type | name | description (e.g. location | Night Relay | Island station).");
        else {
            bible_add(a, type, name, rest, "canon");
            view_bible(a);
            status(a, "Canon entry saved");
        }
    } else if (!strcmp(line, "promote")) {
        Json *entry, *found = NULL;
        cJSON_ArrayForEach(entry, at(a->ws.root, "bible"))
            if (!strcmp(js(entry, "id"), arg)) found = entry;
        if (!found) status(a, "Use /bible to find the entry ID.");
        else {
            set(found, "status", "canon");
            a->ws.dirty = true;
            view_bible(a);
            status(a, "Entry promoted to canon");
        }
    } else if (!strcmp(line, "pin") || !strcmp(line, "unpin") || !strcmp(line, "exclude")) {
        if (!*arg) { status(a, "Use /context to find a source ID."); return; }
        Json *policy = at(a->ws.root, "contextPolicy");
        if (!cJSON_IsObject(policy)) put(a->ws.root, "contextPolicy", policy = cJSON_CreateObject());
        if (!strcmp(line, "unpin")) cJSON_DeleteItemFromObjectCaseSensitive(policy, arg);
        else set(policy, arg, line);
        a->ws.dirty = true;
        view_context(a);
    } else if (!strcmp(line, "premise") || !strcmp(line, "control")) {
        if (!a->scene || a->roleplay) { status(a, "Open a manuscript scene first."); return; }
        if (!strcmp(line, "premise")) set(a->scene, "premise", arg);
        else {
            char *rest = arg, *name = split_piece(&rest, '|');
            if (!*name || !rest) { status(a, "Use /control name | value."); return; }
            set(at(a->scene, "creativeControls"), name, trim_piece(rest));
        }
        a->ws.dirty = true;
        status(a, "Creative settings saved");
    } else if (!strcmp(line, "preferences")) {
        set(a->ws.root, "preferences", arg);
        a->ws.dirty = true;
        status(a, "Writing preferences saved");
    } else if (!strcmp(line, "find")) {
        if (!*arg) { status(a, "Use /find <text>."); return; }
        show_info(a, "Manuscript search");
        Json *r;
        cJSON_ArrayForEach(r, at(a->ws.root, "records"))
            if (!strcmp(js(r, "type"), "scene") && strstr(js(r, "content"), arg))
                format(&a->info, "%s\n", js(r, "path"));
        if (!a->info.len) append(&a->info, "No matching scenes.");
    } else if (!strcmp(line, "import")) import_file(a, arg);
    else if (!strcmp(line, "export")) {
        status(a, *arg && workspace_export(&a->ws, arg) ? "Workspace backup exported" :
            "Export failed. Use the path to a new file.");
    } else if (!strcmp(line, "model")) {
        if (!strncmp(arg, "recall", 6) && (!arg[6] || isspace((unsigned char)arg[6])))
            memory_binding(a, trim_left(arg + 6));
        else if (!*arg) status(a, ui_model(a));
        else if (preview_busy(&a->preview)) status(a, "Finish or cancel generation before changing models.");
        else if (strlen(arg) > 160 || strpbrk(arg, " \t\r\n")) status(a, "Enter one server-configured model ID or default.");
        else {
            const char *model = !strcmp(arg, "default") ? "" : arg;
            if (strcmp(js(a->ws.root, "serverNarrator"), model)) a->neo.count = a->neo.next = 0;
            set(a->ws.root, "serverNarrator", model);
            a->ws.dirty = true;
            status(a, "Narrator model selection saved");
        }
    } else if (!strcmp(line, "server")) {
        if (!*arg) status(a, ui_endpoint(a));
        else if (preview_busy(&a->preview) || a->memory.job) status(a, "Wait for active requests before changing servers.");
        else if (a->server_override || *env("NEUTRINO_SERVER", ""))
            status(a, "Restart without --server or NEUTRINO_SERVER to change the address here.");
        else if ((strncmp(arg, "http://", 7) && strncmp(arg, "https://", 8)) || strpbrk(arg, " \t\r\n?#@"))
            status(a, "Use the Neutrino server URL, such as http://127.0.0.1:4783/v1.");
        else {
            if (strcmp(ui_endpoint(a), arg)) {
                a->neo.count = a->neo.next = 0;
                reset_neofetch_server(&a->neo);
            }
            clear(&a->server_address);
            append(&a->server_address, arg);
            status(a, "Inference server set for this session");
        }
    } else if (!strcmp(line, "endpoint")) {
        status(a, "Set the provider endpoint in the server process. /server sets this client's address.");
    } else status(a, "Unknown command · Ctrl+P opens the palette");
}

static void remember(App *a, const char *line)
{
    if (a->history_count && !strcmp(a->history[a->history_count - 1], line)) return;
    if (a->history_count == (int)(sizeof a->history / sizeof *a->history)) {
        free(a->history[0]);
        memmove(a->history, a->history + 1, sizeof a->history - sizeof a->history[0]);
        a->history_count--;
    }
    a->history[a->history_count++] = copy(line);
}
static void submit(App *a)
{
    if (!*trim_left(str(&a->prompt))) return;
    char *data = copy(trim_left(str(&a->prompt)));
    if (*data != '/' && a->roleplay && preview_busy(&a->preview)) {
        status(a, "A generation is already running. Esc cancels.");
        free(data);
        return;
    }
    Text speaker = {0};
    const char *message = data;
    if (*data != '/' && a->roleplay && !world_address(a->story, data, &speaker, &message)) {
        status(a, "Address a character shown by /world, followed by a message.");
        release(&speaker);
        free(data);
        return;
    }
    remember(a, data);
    clear(&a->prompt);
    a->cursor = 0;
    a->slash = false;
    a->prompt_history = a->history_count;
    if (*data == '/') slash_command(a, data + 1);
    else if (a->roleplay) {
        if (!a->story) show_story(a, story_add(&a->ws, "Untitled story", NULL));
        int id = story_push(&a->ws, a->story, number(a->story, "head"), "user", message);
        if (speaker.len) set(story_pick(a->story, id), "addressed", str(&speaker));
        close_panel(a);
        a->scroll = INT_MAX;
        save(a);
        begin(a, NARRATE, "", id, speaker.len ? str(&speaker) : NULL);
    } else begin(a, CONTINUE, data, 0, NULL);
    release(&speaker);
    free(data);
}

static void prompt_set(App *a, const char *value)
{
    clear(&a->prompt);
    append(&a->prompt, value);
    a->cursor = a->prompt.len;
    a->slash = a->prompt.len && a->prompt.data[0] == '/' && !strpbrk(str(&a->prompt), " \n");
    a->command_cursor = 0;
}
static void choose_command(App *a, bool invoke)
{
    int matches[COMMAND_MATCH_LIMIT];
    int count = ui_command_matches(a, matches, COMMAND_MATCH_LIMIT);
    if (!count) { if (invoke) submit(a); return; }
    if (a->command_cursor >= count) a->command_cursor = count - 1;
    const Command *cmd = &commands[matches[a->command_cursor]];
    if (invoke && *cmd->args && cmd->args[1] == '[' && !strcmp(str(&a->prompt) + 1, cmd->name)) {
        submit(a);
        return;
    }
    clear(&a->prompt);
    format(&a->prompt, "/%s%s", cmd->name, *cmd->args ? " " : "");
    a->cursor = a->prompt.len;
    a->slash = false;
    if (invoke && !*cmd->args) submit(a);
}
static void compose_key(App *a, int key)
{
    Text *t = &a->prompt;
    size_t *pos = &a->cursor;
    if (key == 9 && a->slash) { choose_command(a, false); return; }
    if (key == 13 || key == 10) {
        if (a->slash) {
            int matches[COMMAND_MATCH_LIMIT];
            int total = ui_command_matches(a, matches, COMMAND_MATCH_LIMIT);
            if (a->command_cursor == 0 && total && !strcmp(str(t) + 1, commands[matches[0]].name)) submit(a);
            else choose_command(a, true);
        } else submit(a);
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        int delta = key == KEY_DOWN ? 1 : -1;
        if (a->slash) {
            int matches[COMMAND_MATCH_LIMIT];
            int total = ui_command_matches(a, matches, COMMAND_MATCH_LIMIT), next = a->command_cursor + delta;
            if (next >= 0 && next < total) a->command_cursor = next;
        } else {
            int next = a->prompt_history + delta;
            if (next >= 0 && next <= a->history_count) {
                a->prompt_history = next;
                prompt_set(a, next == a->history_count ? "" : a->history[next]);
            }
        }
        return;
    }
    switch (key) {
        case KEY_LEFT: *pos = utf_prev(str(t), *pos); return;
        case KEY_RIGHT: *pos = utf_next(str(t), t->len, *pos); return;
        case KEY_HOME: *pos = 0; return;
        case KEY_END: *pos = t->len; return;
        case 21: clear(t); *pos = 0; break;
        case 8: case 127: {
            size_t old = *pos;
            *pos = utf_prev(str(t), *pos);
            remove_bytes(t, *pos, old);
            break;
        }
        case KEY_DEL: remove_bytes(t, *pos, utf_next(str(t), t->len, *pos)); break;
        case 11: insert_bytes(t, pos, "\n", 1); break;
        default:
            if (key >= 32 && key <= 0x10ffff && !(key >= 127 && key < 160)) {
                char bytes[4];
                size_t n = utf_encode(key, bytes);
                insert_bytes(t, pos, bytes, n);
            } else return;
    }
    a->slash = t->len && t->data[0] == '/' && !strpbrk(str(t), " \n");
    a->command_cursor = 0;
}

static void library_key(App *a, int key)
{
    int max = ui_library_size(a) - 1, n = a->rail_cursor;
    if (key == KEY_UP) n--;
    else if (key == KEY_DOWN) n++;
    else if (key == KEY_HOME) n = 0;
    else if (key == KEY_END) n = max;
    else if (key == 13 || key == 10) {
        Json *item = ui_library_at(a, n);
        if (a->roleplay) show_story(a, item);
        else show_scene(a, item);
        return;
    } else if (key == KEY_LEFT) {
        a->focus = COMPOSER;
        return;
    }
    if (max >= 0) a->rail_cursor = n < 0 ? 0 : n > max ? max : n;
}

static void key_event(App *a, int key)
{
    if (key == KEY_CLOSED) { a->quit = true; return; }
    if (a->editing && key != 17) { edit_key(a, key); return; }
    switch (key) {
        case 17: if (a->editing) edit_save(a); if (!a->editing) quit(a); return;
        case 3: case 27:
            if (key == 27 && a->view_panel && a->panel_neofetch && !a->slash) close_panel(a);
            else if (preview_busy(&a->preview)) {
                cancel_preview(&a->preview);
                status(a, "Cancelling generation…");
            } else if (a->slash) a->slash = false;
            else if (a->view_panel) close_panel(a);
            else a->focus = COMPOSER;
            return;
        case KEY_F1: show_help(a); return;
        case KEY_F2:
            if (a->roleplay) show_scene(a, a->scene);
            else show_story(a, a->story);
            return;
        case KEY_F3:
            if (a->focus == LIBRARY) a->focus = COMPOSER;
            else { close_panel(a); a->focus = LIBRARY; }
            return;
        case KEY_F4: edit_start(a); return;
        case KEY_F5:
            begin(a, a->roleplay ? NARRATE : CONTINUE, "", number(a->story, "head"), NULL);
            return;
        case KEY_F6: view_context(a); return;
        case KEY_F7:
            if (a->preview.open) show_preview(a);
            else status(a, "No candidate review is open.");
            return;
        case KEY_F8: accept(a, false); return;
        case KEY_F9: close_preview(a); return;
        case KEY_F10: view_bible(a); return;
        case KEY_FOCUS: a->focus = COMPOSER; prompt_set(a, "/"); return;
        case 19: status(a, "Workspace saved"); save(a); return;
        case KEY_PGUP: case KEY_PGDN: {
            int *scroll = a->view_panel ? &a->panel_scroll : &a->scroll;
            if (*scroll == INT_MAX) *scroll = a->scroll_max;
            *scroll += key == KEY_PGUP ? -8 : 8;
            if (*scroll < 0) *scroll = 0;
            return;
        }
    }
    if (a->focus == PANEL && a->view_panel) {
        Preview *p = &a->preview;
        if (key == KEY_UP && a->panel_scroll > 0) a->panel_scroll--;
        else if (key == KEY_DOWN) a->panel_scroll++;
        else if ((key == KEY_LEFT || key == KEY_RIGHT) && a->panel_preview) {
            int n = p->current + (key == KEY_LEFT ? -1 : 1);
            if (n >= 0 && n < p->count) { p->current = n; a->panel_scroll = 0; }
        } else if (key == 9 || key == KEY_BACKTAB || key == '/') {
            a->focus = COMPOSER;
            if (key == '/') prompt_set(a, "/");
        }
        return;
    }
    if ((key == 9 || key == KEY_BACKTAB) && !a->slash) {
        a->focus = a->focus == LIBRARY ? COMPOSER : LIBRARY;
        return;
    }
    if (a->focus == LIBRARY) {
        if (key == '/') { a->focus = COMPOSER; prompt_set(a, "/"); }
        else library_key(a, key);
        return;
    }
    compose_key(a, key);
}

static void initial_selection(App *a)
{
    Json *selection = at(a->ws.root, "neutrinoSelection");
    if (selection) {
        a->scene = scene_find(&a->ws, js(selection, "scene"));
        Json *story;
        cJSON_ArrayForEach(story, at(a->ws.root, "stories"))
            if (!strcmp(js(story, "id"), js(selection, "story"))) a->story = story;
        a->roleplay = cJSON_IsTrue(at(selection, "roleplay"));
    } else {
        a->scene = ui_library_at(a, 1);
        a->story = cJSON_GetArrayItem(at(a->ws.root, "stories"), 0);
    }
    if (a->roleplay) a->scroll = INT_MAX;
    ui_clamp_rail(a);
}
static void restore_terminal(void)
{
    const char end[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
    terminal_write(end, sizeof end - 1);
    terminal_close();
}

static int run(int argc, char **argv)
{
    App a = {0};
    const char *path = env("NEUTRINO_WORKSPACE", ".neutrino.json"), *import = NULL;
    bool dump = false, serve = false;
    int port = 4783, bench = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--demo")) a.demo = true;
        else if (!strcmp(argv[i], "--snapshot")) dump = true;
        else if (!strcmp(argv[i], "--ssh-session")) a.ssh_session = true;
        else if (!strcmp(argv[i], "--serve")) serve = true;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = parse_number(argv[++i], 1, 65535);
            if (port < 0) { fputs("Invalid port.\n", stderr); return 2; }
        }
        else if (!strcmp(argv[i], "--server") && i + 1 < argc) a.server_override = argv[++i];
        else if (!strcmp(argv[i], "--bench-ttft") || (!strcmp(argv[i], "bench") &&
                 i + 1 < argc && !strcmp(argv[i + 1], "ttft"))) {
            if (!strcmp(argv[i], "bench")) i++;
            bench = 3;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                bench = parse_number(argv[++i], 1, 20);
                if (bench < 0) { fputs("Enter a benchmark round count from 1 to 20.\n", stderr); return 2; }
            }
        }
        else if (!strcmp(argv[i], "--workspace") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--import") && i + 1 < argc) import = argv[++i];
        else if (!strcmp(argv[i], "--version")) { puts("Neutrino " NEUTRINO_VERSION); return 0; }
        else {
            bool help = !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h");
            FILE *out = help ? stdout : stderr;
            if (!help) fprintf(out, "Unexpected argument: %s\n", argv[i]);
            fputs("Usage: neutrino [--workspace FILE] [--import FILE] [--demo] [--snapshot] [--server URL]\n"
                  "       neutrino --ssh-session --workspace FILE [--server URL]\n"
                  "       neutrino --serve [--port PORT]\n"
                  "       neutrino bench ttft [ROUNDS] [--server URL]\n", out);
            return help ? 0 : 2;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (a.ssh_session && (serve || bench || dump || import || a.demo ||
        !*env("SSH_TTY", "") || *env("SSH_ORIGINAL_COMMAND", ""))) {
        fputs("Open an interactive SSH connection with a PTY and no remote command.\n", stderr);
        return 2;
    }
    if (serve && bench) { fputs("Start the server and benchmark in separate processes.\n", stderr); return 2; }
    if (serve) return inference_server(port, &interrupted);
    if (bench) return bench_ttft(ui_endpoint(&a), env("NEUTRINO_SERVER_TOKEN", ""), bench, &interrupted);
    if (!workspace_open(&a.ws, a.demo ? NULL : path, !dump || import != NULL)) {
        fprintf(stderr, "Could not open workspace: %s (locked, inaccessible, or invalid JSON)\n", path);
        return 1;
    }
    if (a.demo) workspace_demo(&a.ws);
    if (import) {
        Text error = {0};
        if (!workspace_import(&a.ws, import, &error) || !workspace_save(&a.ws)) {
            fprintf(stderr, "Import failed: %s\n", error.len ? str(&error) : "workspace could not be saved");
            release(&error);
            workspace_close(&a.ws);
            return 1;
        }
        release(&error);
    }
    initial_selection(&a);
    if (dump) {
        ui_dump(&a, 112, 30);
    } else {
        if (!terminal_open()) {
            fprintf(stderr, "Neutrino needs an interactive VT-capable terminal. Use --snapshot for a static preview.\n");
            workspace_close(&a.ws);
            return 1;
        }
        atexit(restore_terminal);
        const char start[] = "\x1b[?1049h\x1b[?25l";
        terminal_write(start, sizeof start - 1);
        bool redraw = true;
        while (!a.quit && !interrupted) {
            int cols, rows;
            terminal_size(&cols, &rows);
            if (cols != a.cols || rows != a.rows) {
                a.cols = cols;
                a.rows = rows;
                redraw = true;
            }
            if (update_generation(&a)) redraw = true;
            if (tick_neofetch(&a)) redraw = true;
            if (memory_tick(&a)) {
                save(&a);
                redraw = true;
            }
            if (redraw) ui_draw(&a);
            int key = terminal_key(50);
            redraw = key >= 0;
            if (redraw) {
                for (int n = 0; key >= 0 && n < 96; n++) {
                    key_event(&a, key);
                    if (a.quit || n == 95) break;
                    key = terminal_key(0);
                }
                save(&a);
            }
        }
        restore_terminal();
    }
    if (preview_busy(&a.preview)) cancel_preview(&a.preview);
    memory_close(&a);
    release_preview(&a.preview);
    reset_neofetch_server(&a.neo);
    if (a.neo.probe) inference_free(a.neo.probe);
    release(&a.neo.error);
    for (int i = 0; i < a.history_count; i++) free(a.history[i]);
    release(&a.screen);
    release(&a.prompt);
    release(&a.edit);
    release(&a.status);
    release(&a.info);
    release(&a.trace);
    release(&a.server_address);
    workspace_close(&a.ws);
    return 0;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int wmain(int argc, wchar_t **argv)
{
    char **args = calloc((size_t)argc + 1, sizeof(*args));
    if (!args) abort();
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0) abort();
        args[i] = malloc((size_t)n);
        if (!args[i]) abort();
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, args[i], n, NULL, NULL);
    }
    int code = run(argc, args);
    for (int i = 0; i < argc; i++) free(args[i]);
    free(args);
    return code;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
