#include "app.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kinds[] = {
    "character", "place", "item", "event", "note", "faction", "rule",
    "relationship", "plot", "quest", "creature"
};

static bool valid_kind(const char *kind)
{
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (!strcmp(kind, kinds[i])) return true;
    return false;
}
bool memory_enabled(const Json *story)
{
    return story && !cJSON_IsFalse(at(story, "memoryEnabled"));
}
const char *memory_mode(const Json *story)
{
    const char *mode = js(story, "memoryMode");
    return *mode ? mode : "auto";
}

static char *piece(char **remaining, char delimiter)
{
    if (!*remaining) return "";
    char *s = *remaining;
    char *end = delimiter ? strchr(s, delimiter) : NULL;
    if (end) *end++ = 0;
    *remaining = end;
    while (isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}
static Json *message(const char *role, const char *text)
{
    Json *m = cJSON_CreateObject();
    set(m, "role", role);
    set(m, "content", text);
    return m;
}
static Json *request(const char *instructions, const Text *input)
{
    Json *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, message("system", instructions));
    cJSON_AddItemToArray(messages, message("user", str(input)));
    return messages;
}
static Json *entry(const char *id, const char *type, const char *name, const char *description)
{
    Json *out = cJSON_CreateObject();
    set(out, "id", id);
    set(out, "type", type);
    set(out, "name", name);
    set(out, "description", description);
    return out;
}
static bool same_words(const char *a, const char *b)
{
    if (!strcmp(a, b)) return true;
    Text left = {0}, right = {0};
    normalize_words(&left, a);
    normalize_words(&right, b);
    bool equal = left.len && !strcmp(str(&left), str(&right));
    release(&left);
    release(&right);
    return equal;
}
static bool contains_word(const char *value, const char *pattern, size_t len)
{
    if (!len) return false;
    for (; *value; value++) {
        size_t i = 0;
        while (i < len && value[i] &&
            tolower((unsigned char)value[i]) == tolower((unsigned char)pattern[i])) i++;
        if (i == len) return true;
    }
    return false;
}
static Json *find_entry(const Json *entries, const char *id)
{
    Json *item;
    cJSON_ArrayForEach(item, entries)
        if (!strcmp(js(item, "id"), id)) return item;
    return NULL;
}
static Json *find_entity(const Json *entries, const char *type, const char *name)
{
    Json *item;
    cJSON_ArrayForEach(item, entries)
        if (!strcmp(js(item, "type"), type) && same_words(js(item, "name"), name)) return item;
    return NULL;
}

/* Short labels can legitimately be shared by unrelated people or objects. */
static int signal_kind(const char *type, const char *name)
{
    if (strcmp(type, "event")) return 0;
    if (same_words(name, "bell") || same_words(name, "brass bell")) return 1;
    return same_words(name, "flare") ? 2 : 0;
}
static bool duplicates_fact(const Json *entries, const char *type, const char *name, const char *fact)
{
    const int signal = signal_kind(type, name);
    Json *item;
    cJSON_ArrayForEach(item, entries) {
        bool same_entity = !strcmp(js(item, "type"), type) && same_words(js(item, "name"), name);
        /* Distinct witnessed signals may cite the very same sentence. */
        if (signal && !same_entity && signal != signal_kind(js(item, "type"), js(item, "name"))) continue;
        if (!same_entity && strlen(fact) < 32) continue;
        const char *description = js(item, "description");
        if (same_words(description, fact)) return true;
        if (same_entity && strchr(description, '\n')) {
            for (const char *line = description; *line;) {
                const char *next = strchr(line, '\n');
                if (!next) next = line + strlen(line);
                Text part = {0};
                append_n(&part, line, (size_t)(next - line));
                bool equal = same_words(str(&part), fact);
                release(&part);
                if (equal) return true;
                line = *next ? next + 1 : next;
            }
        }
    }
    return false;
}
static void merge_entry(Json *entries, const Json *source, const char *id)
{
    const char *type = js(source, "type"), *name = js(source, "name");
    const char *description = js(source, "description");
    Json *matched = id ? find_entry(entries, id) : find_entity(entries, type, name);
    if (cJSON_IsTrue(at(source, "deleted"))) {
        if (matched) cJSON_Delete(cJSON_DetachItemViaPointer(entries, matched));
        return;
    }
    Text novel = {0}, combined = {0};
    if (!id) {
        bool ambiguous = compendium_novel_claims(entries, NULL, type, name, description, &novel);
        if (!novel.len || (!ambiguous && duplicates_fact(entries, type, name, str(&novel)))) {
            release(&novel);
            return;
        }
        bool partial = strcmp(description, str(&novel)) || cJSON_IsTrue(at(source, "memoryAppend"));
        description = str(&novel);
        if (matched && partial) {
            format(&combined, "%s\n%s", description, js(matched, "description"));
            description = str(&combined);
        }
    }
    Json *copy = entry(matched ? js(matched, "id") : id ? id : js(source, "id"),
                       type, name, description);
    if (matched) cJSON_ReplaceItemViaPointer(entries, matched, copy);
    else cJSON_AddItemToArray(entries, copy);
    release(&novel);
    release(&combined);
}

static Json *collect_entries(const Json *story, int head)
{
    Json *entries = cJSON_CreateArray(), *path = story_path(story, head), *node, *item;
    int count = cJSON_GetArraySize(at(story, "nodes"));
    bool *visible = calloc((size_t)count + 1, sizeof(*visible));
    if (!visible) abort();
    visible[0] = true;
    cJSON_ArrayForEach(node, path) {
        int id = number(node, "id");
        if (id >= 1 && id <= count) visible[id] = true;
        cJSON_ArrayForEach(item, at(node, "memoryFile"))
            if (valid_kind(js(item, "type")) && *js(item, "name"))
                merge_entry(entries, item, NULL);
    }
    cJSON_ArrayForEach(item, at(story, "memoryManual")) {
        int anchor = number(item, "anchor");
        const char *id = js(item, "id");
        if (*id && anchor >= 0 && anchor <= count && visible[anchor]) merge_entry(entries, item, id);
    }
    free(visible);
    cJSON_Delete(path);
    return entries;
}

static void excerpt(Text *out, const char *source, size_t cap)
{
    size_t n = strlen(source);
    if (n > cap) {
        n = cap;
        while (n && ((unsigned char)source[n] & 0xc0) == 0x80) n--;
    }
    append_n(out, source, n);
}
static void recent_turns(Text *out, const Json *path, int count)
{
    int skip = cJSON_GetArraySize(path) - count;
    Json *node;
    cJSON_ArrayForEach(node, path) {
        if (skip-- > 0) continue;
        format(out, "\n%s: ", js(node, "role"));
        excerpt(out, js(node, "content"), 2400);
    }
}

static int relevance(const Json *item, const char *query)
{
    const char *name = js(item, "name"), *details = js(item, "description");
    int score = *name && contains_word(query, name, strlen(name)) ? 40 : 0;
    for (const char *start = query; *start;) {
        while (*start && !isalnum((unsigned char)*start) && (unsigned char)*start < 128) start++;
        const char *end = start;
        while (isalnum((unsigned char)*end) || (unsigned char)*end >= 128) end++;
        size_t n = (size_t)(end - start);
        if (n >= 4 && n < 40) {
            if (contains_word(name, start, n)) score += 3;
            else if (contains_word(details, start, n)) score++;
        }
        start = end;
    }
    return score;
}
static bool picked(const Json *ids, const char *id)
{
    Json *item;
    cJSON_ArrayForEach(item, ids)
        if (cJSON_IsString(item) && !strcmp(item->valuestring, id)) return true;
    return false;
}

void memory_inject(const Json *story, int head, const Json *picks, Text *system, Text *trace)
{
    if (!memory_enabled(story)) { append(trace, "Compendium disabled for this story.\n"); return; }
    Json *entries = collect_entries(story, head), *path = story_path(story, head), *item;
    Text query = {0}, selected = {0};
    recent_turns(&query, path, 4);
    cJSON_Delete(path);
    int visible = cJSON_GetArraySize(entries), used = 0;
    for (int pass = 0; pass < 2 && used < 12; pass++) {
        for (item = entries->child ? entries->child->prev : NULL; item && used < 12;) {
            const char *id = js(item, "id"), *name = js(item, "name");
            bool model = picked(picks, id);
            if ((pass == 0 && model) || (pass == 1 && !model && relevance(item, str(&query)) > 0)) {
                if (selected.len < 6500) {
                    format(&selected, "\n- %s (%s): ", name, js(item, "type"));
                    excerpt(&selected, js(item, "description"), 650);
                    format(trace, "  %s %s — %s\n", id, name, model ? "model recall" : "lexical match");
                    used++;
                }
            }
            item = item == entries->child ? NULL : item->prev;
        }
    }
    if (used) format(system, "\n\nCompendium (established story facts):%s", str(&selected));
    format(trace, "%d of %d active entries included.\n", used, visible);
    release(&query);
    release(&selected);
    cJSON_Delete(entries);
}

int memory_history_target(const Json *path)
{
    int total = cJSON_GetArraySize(path), desired = total > 20 ? ((total - 14) / 6) * 6 : 0;
    size_t bytes = 0;
    int budget_start = total;
    Json *node = path->child ? path->child->prev : NULL;
    while (budget_start > 0 && node) {
        size_t n = strlen(js(node, "content"));
        if (n > 64000 - bytes) break;
        bytes += n;
        budget_start--;
        node = node->prev;
    }
    /* Historian covers complete older turns, leaving the newest messages verbatim. */
    if (budget_start > desired) desired = budget_start;
    return desired < total ? desired : total > 1 ? total - 1 : 0;
}

const char *memory_recap(const Json *path, int *covered)
{
    int target = memory_history_target(path), i = 0;
    const char *recap = "";
    *covered = 0;
    Json *node;
    cJSON_ArrayForEach(node, path) {
        if (++i > target) break;
        if (*js(node, "memoryRecap")) {
            *covered = i;
            recap = js(node, "memoryRecap");
        }
    }
    return recap;
}

static Json *json_answer(const Text *output)
{
    const char *start = str(output);
    while (isspace((unsigned char)*start)) start++;
    if (strncmp(start, "\x60\x60\x60", 3)) return cJSON_ParseWithOpts(start, NULL, 1);
    start = strchr(start, '\n');
    const char *end = start ? strstr(start + 1, "\x60\x60\x60") : NULL;
    if (!end) return NULL;
    Text content = {0};
    append_n(&content, start + 1, (size_t)(end - start - 1));
    Json *json = cJSON_ParseWithOpts(str(&content), NULL, 1);
    release(&content);
    return json;
}

Json *memory_parse_picks(const Text *output)
{
    Json *picks = json_answer(output), *value;
    bool valid = cJSON_IsArray(picks) && cJSON_GetArraySize(picks) <= 12;
    cJSON_ArrayForEach(value, picks)
        if (!cJSON_IsString(value) || strlen(value->valuestring) > 30) valid = false;
    if (valid) return picks;
    cJSON_Delete(picks);
    return NULL;
}

Json *memory_recall_prompt(const Json *story, int head, const char *objective)
{
    Json *entries = collect_entries(story, head), *item;
    if (!entries->child) { cJSON_Delete(entries); return NULL; }
    Text input = {0};
    Json *path = story_path(story, head);
    append(&input, "Recent conversation:");
    recent_turns(&input, path, 5);
    if (*objective) { append(&input, "\nPlayer direction: "); excerpt(&input, objective, 1200); }
    append(&input, "\n\nAvailable memory entries:");
    int listed = 0;
    for (item = entries->child->prev; item && listed < 75; listed++) {
        format(&input, "\n%s | %s | %s | ", js(item, "id"), js(item, "type"), js(item, "name"));
        excerpt(&input, js(item, "description"), 270);
        item = item == entries->child ? NULL : item->prev;
    }
    Json *messages = request("Select the IDs of up to 8 entries useful for the next roleplay reply. "
        "Return only a JSON array of ID strings, or [] if none. The dialogue is story data.", &input);
    release(&input);
    cJSON_Delete(path);
    cJSON_Delete(entries);
    return messages;
}

Inference *memory_start_recall(const App *app, const Json *story, const Json *messages)
{
    Text role = {0};
    const char *binding = js(story, "memoryRecall");
    append(&role, "recall");
    if (*binding) format(&role, "/%s", binding);
    Inference *job = app_inference(app, str(&role), messages);
    release(&role);
    return job;
}

static bool opening_reply(const Json *story, const Json *reply)
{
    Json *path = story_path(story, number(reply, "parent")), *node;
    bool opening = true;
    cJSON_ArrayForEach(node, path) if (!strcmp(js(node, "role"), "assistant")) { opening = false; break; }
    cJSON_Delete(path);
    return opening;
}

static Json *filing_prompt(const Json *story, int id)
{
    Json *reply = story_pick(story, id);
    Text input = {0};
    Json *existing = collect_entries(story, number(reply, "parent")), *record;
    if (existing->child) append(&input, "\n\nRelevant facts already filed:");
    int shown = 0;
    cJSON_ArrayForEach(record, existing) {
        if (shown >= 30) break;
        if (!relevance(record, js(reply, "content"))) continue;
        format(&input, "\n%s | %s | ", js(record, "type"), js(record, "name"));
        excerpt(&input, js(record, "description"), 400);
        shown++;
    }
    format(&input, "\n\nLatest assistant reply (#%d), the evidence to file:\n", id);
    excerpt(&input, js(reply, "content"), 2400);
    append(&input, "\n\nLook for completed actions, changed locations, object states, and witnessed signals. "
        "These may be inside descriptive sentences. An instruction or expectation is not evidence that its event happened. "
        "Copy a short exact passage from this reply containing the entity and what happened into description. "
        "Do not paraphrase or add punctuation. Name the entity using words from the passage. "
        "If no new fact has a verbatim passage here, output [].");
    Json *messages = request("File durable facts newly established by the final assistant message. "
        "Ignore speculation. Preserve uncertainty when the story only suggests a fact. "
        "Return only a JSON array of up to six objects with type, name and description, with description "
        "copied verbatim from the latest reply. Previously filed entries are only for deduplication. "
        "Use one object per entity and choose the most specific type. Allowed types: "
        "character, place, item, event, note, faction, rule, relationship, plot, quest, creature. "
        "Use character for people and item for physical objects, even when described figuratively. "
        "Keep the entity named in the description aligned with the entry name. "
        "An object being hidden and the container or place hiding it are separate entities. "
        "Reuse existing names for updates. Skip facts already in the supplied ledger, including under another type. "
        "If nothing changed, return [].", &input);
    cJSON_Delete(existing);
    release(&input);
    return messages;
}
static bool file_candidates(Json *result, const Json *known, const Json *answer, const Json *reply,
                            bool fallback, int *index)
{
    const int anchor = number(reply, "id");
    const char *prose = js(reply, "content");
    Json *item;
    cJSON_ArrayForEach(item, answer) {
        if (cJSON_GetArraySize(result) >= 6) break;
        const char *type = js(item, "type"), *name = js(item, "name"), *description = js(item, "description");
        if (!valid_kind(type) || !*name || strlen(name) > 160 || !*description || strlen(description) > 2400)
            return false;
        (*index)++;
        Text corrected = {0}, evidence = {0}, novel = {0};
        if (!compendium_filing_evidence(prose, description, &evidence)) continue;
        type = compendium_filing_identity(type, name,
            !strcmp(type, "character") ? str(&evidence) : description, prose, &corrected);
        if (corrected.len) name = str(&corrected);
        if (compendium_filing_label(name, str(&evidence))) {
            bool ambiguous = compendium_novel_claims(known, result, type, name, str(&evidence), &novel);
            bool partial = strcmp(str(&evidence), str(&novel)) != 0;
            description = str(&novel);
            Text exact = {0};
            if (novel.len && partial && !compendium_filing_evidence(prose, description, &exact))
                description = str(&evidence);
            if (novel.len &&
                (ambiguous || !duplicates_fact(known, type, name, description)) &&
                !duplicates_fact(result, type, name, description)) {
                Json *entity = find_entity(result, type, name);
                if (entity) {
                    const char *previous = js(entity, "description");
                    if (strlen(previous) + strlen(description) + 1 <= 2400) {
                        Text combined = {0};
                        format(&combined, "%s\n%s", previous, description);
                        set(entity, "description", str(&combined));
                        release(&combined);
                    }
                } else {
                    char id[40];
                    snprintf(id, sizeof id, "A%d.%d", anchor, *index);
                    entity = entry(id, type, name, description);
                    if (fallback) set(entity, "memorySource", "reply-evidence");
                    cJSON_AddItemToArray(result, entity);
                }
                if (partial) put(entity, "memoryAppend", cJSON_CreateTrue());
            }
            release(&exact);
        }
        release(&novel);
        release(&evidence);
        release(&corrected);
    }
    return true;
}
static Json *parse_filing(const Text *output, const Json *story, const Json *reply)
{
    Json *answer = json_answer(output);
    if (!cJSON_IsArray(answer) || cJSON_GetArraySize(answer) > 6) { cJSON_Delete(answer); return NULL; }
    Json *result = cJSON_CreateArray();
    Json *known = collect_entries(story, number(reply, "parent"));
    int index = 0;
    bool valid = file_candidates(result, known, answer, reply, false, &index);
    cJSON_Delete(answer);
    if (valid && !result->child && opening_reply(story, reply)) {
        Json *fallback = compendium_opening_evidence(js(reply, "content"));
        valid = file_candidates(result, known, fallback, reply, true, &index);
        cJSON_Delete(fallback);
    }
    if (valid && cJSON_GetArraySize(result) < 6) {
        Json *signals = compendium_signal_evidence(js(reply, "content"));
        valid = file_candidates(result, known, signals, reply, true, &index);
        cJSON_Delete(signals);
    }
    if (valid && cJSON_GetArraySize(result) < 6) {
        Json *history = story_path(story, number(reply, "parent"));
        Text name = {0}, evidence = {0};
        bool found = compendium_placement_evidence(known, history, js(reply, "content"), &name, &evidence);
        Json *accepted;
        if (found) cJSON_ArrayForEach(accepted, result)
            if (strstr(js(accepted, "description"), str(&evidence))) { found = false; break; }
        if (found) {
            char id[40];
            snprintf(id, sizeof id, "A%d.%d", number(reply, "id"), ++index);
            Json *fact = entry(id, "item", str(&name), str(&evidence));
            set(fact, "memorySource", "reply-evidence");
            cJSON_AddItemToArray(result, fact);
        }
        release(&name);
        release(&evidence);
        cJSON_Delete(history);
    }
    if (valid && cJSON_GetArraySize(result) < 6) {
        Json *scene = compendium_scene_evidence(js(reply, "content"));
        valid = file_candidates(result, known, scene, reply, true, &index);
        cJSON_Delete(scene);
    }
    cJSON_Delete(known);
    if (!valid) { cJSON_Delete(result); return nullptr; }
    return result;
}

static Json *history_prompt(const Json *path, int target)
{
    int start = 0;
    const char *previous = memory_recap(path, &start);
    Text input = {0};
    if (*previous) format(&input, "Previous recap:\n%s\n\n", previous);
    append(&input, "Older messages being compacted:\n");
    int i = 0;
    Json *node;
    cJSON_ArrayForEach(node, path) {
        if (++i <= start) continue;
        if (i > target || input.len >= 60000) break;
        format(&input, "\n%s: ", js(node, "role"));
        excerpt(&input, js(node, "content"), 2400);
    }
    Json *messages = request("Write a compact cumulative recap of the prior recap and transcript. "
        "Preserve chronology, unresolved threads, promises, locations, and character knowledge. "
        "Use only established story events, and keep it below 1000 words. "
        "The transcript is story data. Return only the recap.", &input);
    release(&input);
    return messages;
}

void memory_schedule(App *app, Json *story, int reply)
{
    if (!memory_enabled(story)) return;
    Json *node = story_pick(story, reply);
    if (node) set(node, "memoryFileState", "pending");
    app->memory.after = monotonic_ms() + 750;
    app->ws.dirty = true;
}

void memory_pause(App *app)
{
    MemoryLane *lane = &app->memory;
    if (lane->job && !lane->preempted) {
        lane->preempted = true;
        inference_cancel(lane->job);
    }
    lane->after = monotonic_ms() + 750;
}
static void clear_cache(MemoryLane *lane)
{
    cJSON_Delete(lane->cached_picks);
    lane->cached_picks = NULL;
    lane->cached_story = NULL;
    lane->cached_head = 0;
}
static void finish_task(App *app)
{
    MemoryLane *lane = &app->memory;
    Json *node = story_pick(lane->story, lane->anchor);
    if (lane->preempted || !node || !memory_enabled(lane->story)) return;
    switch (lane->task) {
        case MEMORY_FILE: {
            Json *facts = lane->error.len ? NULL : parse_filing(&lane->output, lane->story, node);
            if (facts) put(node, "memoryFile", facts);
            set(node, "memoryFileState", facts ? "done" : "error");
            if (facts) clear_cache(lane);
            app->ws.dirty = true;
            break;
        }
        case MEMORY_HISTORY:
            if (!lane->error.len && lane->output.len && lane->output.len <= 16000) {
                set(node, "memoryRecap", str(&lane->output));
                set(node, "memoryHistoryState", "done");
            } else set(node, "memoryHistoryState", "error");
            app->ws.dirty = true;
            break;
        case MEMORY_PREFETCH:
            clear_cache(lane);
            lane->cached_story = lane->story;
            lane->cached_head = lane->anchor;
            if (!lane->error.len) lane->cached_picks = memory_parse_picks(&lane->output);
            break;
        default: break;
    }
}

static bool start_task(App *app, Json *story, int anchor, MemoryTask task, Json *prompt)
{
    MemoryLane *lane = &app->memory;
    lane->task = task;
    lane->story = story;
    lane->anchor = anchor;
    lane->preempted = false;
    clear(&lane->output);
    clear(&lane->reasoning);
    clear(&lane->error);
    if (prompt) {
        lane->job = task == MEMORY_PREFETCH ? memory_start_recall(app, story, prompt) :
            app_inference(app, task == MEMORY_FILE ? "file" : "history", prompt);
        cJSON_Delete(prompt);
    }
    if (!lane->job) {
        append(&lane->error, "Agent unavailable");
        finish_task(app);
        lane->task = MEMORY_IDLE;
    }
    return true;
}

bool memory_tick(App *app)
{
    MemoryLane *lane = &app->memory;
    if (lane->job) {
        if (!inference_read(lane->job, &lane->output, &lane->reasoning, &lane->error)) return false;
        inference_free(lane->job);
        lane->job = NULL;
        finish_task(app);
        lane->task = MEMORY_IDLE;
        return true;
    }
    if (preview_busy(&app->preview) || monotonic_ms() < lane->after) return false;
    lane->after = monotonic_ms() + 250;
    Json *story, *node;
    cJSON_ArrayForEach(story, at(app->ws.root, "stories")) {
        if (!memory_enabled(story)) continue;
        cJSON_ArrayForEach(node, at(story, "nodes")) {
            if (strcmp(js(node, "memoryFileState"), "pending") ||
                strcmp(js(node, "role"), "assistant")) continue;
            int anchor = number(node, "id");
            return start_task(app, story, anchor, MEMORY_FILE, filing_prompt(story, anchor));
        }
    }
    story = app->story;
    int head = number(story, "head");
    if (!memory_enabled(story) || !head) return false;
    Json *path = story_path(story, head);
    int target = memory_history_target(path), covered = 0;
    memory_recap(path, &covered);
    if (target > covered + 12) target = covered + 12;
    if (target <= covered) target = 0;
    node = cJSON_GetArrayItem(path, target - 1);
    if (node && !*js(node, "memoryHistoryState")) {
        int id = number(node, "id");
        Json *prompt = history_prompt(path, target);
        cJSON_Delete(path);
        return start_task(app, story, id, MEMORY_HISTORY, prompt);
    }
    cJSON_Delete(path);
    const char *mode = memory_mode(story);
    if (app->roleplay && (!strcmp(mode, "prefetch") || !strcmp(mode, "auto")) &&
        (lane->cached_story != story || lane->cached_head != head))
        return start_task(app, story, head, MEMORY_PREFETCH, memory_recall_prompt(story, head, ""));
    return false;
}
void memory_close(App *app)
{
    MemoryLane *lane = &app->memory;
    if (lane->job) {
        inference_cancel(lane->job);
        inference_free(lane->job);
    }
    clear_cache(lane);
    release(&lane->output);
    release(&lane->reasoning);
    release(&lane->error);
}

static void invalidate(App *a)
{
    clear_cache(&a->memory);
    memory_pause(a);
    a->ws.dirty = true;
}
void memory_binding(App *a, const char *binding)
{
    if (!a->story) { app_status(a, "Open a roleplay story first."); return; }
    if (!*binding) {
        app_status(a, *js(a->story, "memoryRecall") ? js(a->story, "memoryRecall") : "Recall uses the server default.");
        return;
    }
    if (strcmp(binding, "default") &&
        (strlen(binding) > 160 || (!strchr(binding, ':')) ||
         (strncmp(binding, "default:", 8) && strncmp(binding, "recall:", 7)) || !strchr(binding, ':')[1])) {
        app_status(a, "Use /model recall recall:MODEL, default:MODEL, or default.");
        return;
    }
    set(a->story, "memoryRecall", !strcmp(binding, "default") ? "" : binding);
    invalidate(a);
    app_status(a, "Recall model binding saved");
}

static void list_entries(App *a)
{
    app_info(a, "Compendium");
    int head = number(a->story, "head");
    format(&a->info, "Branch #%d  ·  memory %s  ·  recall %s\n\n", head,
        memory_enabled(a->story) ? "on" : "off", memory_mode(a->story));
    Json *entries = collect_entries(a->story, head), *item;
    cJSON_ArrayForEach(item, entries) {
        format(&a->info, "%s  %s  ·  %s\n%s\n\n", js(item, "id"), js(item, "type"),
               js(item, "name"), js(item, "description"));
    }
    if (!entries->child) append(&a->info, "No entries on this branch yet.\n\n");
    append(&a->info, "/memory new TYPE | NAME | DESCRIPTION\n"
                     "/memory edit ID | DESCRIPTION\n"
                     "/memory delete ID\n"
                     "/memory show ID   /memory status   /memory mode");
    cJSON_Delete(entries);
}
static void show_entry(App *a, const char *id)
{
    const int head = number(a->story, "head");
    Json *entries = collect_entries(a->story, head);
    const Json *item = find_entry(entries, id);
    if (item) {
        app_info(a, "Compendium entry");
        format(&a->info, "%s · %s · %s\nActive on branch #%d\n\n%s\n", js(item, "id"),
               js(item, "type"), js(item, "name"), head, js(item, "description"));
    } else app_status(a, "Choose an active entry ID from /memory.");
    cJSON_Delete(entries);
}

static void manual(App *a, const char *verb, char *args)
{
    bool add = !strcmp(verb, "new"), remove = !strcmp(verb, "delete");
    char *rest = args;
    char *first = piece(&rest, '|');
    int head = number(a->story, "head");
    Json *events = at(a->story, "memoryManual");
    Json *active = add ? NULL : collect_entries(a->story, head);
    Json *current = active ? find_entry(active, first) : NULL;
    const char *type = add ? first : js(current, "type");
    const char *name = add ? piece(&rest, '|') : js(current, "name");
    const char *description = piece(&rest, '\0');
    if ((add && (!valid_kind(type) || !*name || strlen(name) > 160)) ||
        (!add && !current) || (!remove && (!*description || strlen(description) > 2400))) {
        cJSON_Delete(active);
        app_status(a, add ? "/memory new TYPE | NAME | DESCRIPTION (character, place, item, event, note…)" :
            "Use an active entry ID. Edit also requires | DESCRIPTION.");
        return;
    }
    if (!cJSON_IsArray(events)) put(a->story, "memoryManual", events = cJSON_CreateArray());
    char id[40];
    snprintf(id, sizeof id, "M%d", cJSON_GetArraySize(events) + 1);
    Json *change = entry(add ? id : first, type, name, description);
    integer(change, "anchor", head);
    if (remove) put(change, "deleted", cJSON_CreateTrue());
    cJSON_AddItemToArray(events, change);
    cJSON_Delete(active);
    invalidate(a);
    list_entries(a);
    app_status(a, "Compendium updated");
}

static void memory_status(App *a)
{
    app_info(a, "Compendium activity");
    int pending = 0, errors = 0, completed = 0;
    Json *node;
    cJSON_ArrayForEach(node, at(a->story, "nodes")) {
        const char *file = js(node, "memoryFileState"), *history = js(node, "memoryHistoryState");
        if (!strcmp(file, "pending")) pending++;
        if (!strcmp(file, "done")) completed++;
        if (!strcmp(file, "error") || !strcmp(history, "error")) {
            errors++;
            format(&a->info, "Reply #%d  %s%s\n", number(node, "id"),
                !strcmp(file, "error") ? "filing failed " : "", !strcmp(history, "error") ? "history failed" : "");
        }
    }
    format(&a->info, "\n%d filings pending  ·  %d filed  ·  %d nodes with errors\n",
           pending, completed, errors);
    if (a->memory.job && a->memory.story == a->story)
        format(&a->info, "Working on #%d (%s)\n", a->memory.anchor,
            a->memory.task == MEMORY_FILE ? "filing" : a->memory.task == MEMORY_HISTORY ? "history" : "prefetch");
    append(&a->info, "\n/memory retry   /memory refile ID   /memory undo ID");
}

void memory_command(App *a, char *args)
{
    if (!a->story || !a->roleplay) { app_status(a, "Open a roleplay story first."); return; }
    char *rest = args, *verb = piece(&rest, ' ');
    if (!rest) rest = "";
    while (isspace((unsigned char)*rest)) rest++;
    if (!*verb || !strcmp(verb, "list")) list_entries(a);
    else if (!strcmp(verb, "show")) show_entry(a, rest);
    else if (!strcmp(verb, "status")) memory_status(a);
    else if (!strcmp(verb, "new") || !strcmp(verb, "edit") || !strcmp(verb, "delete")) manual(a, verb, rest);
    else if (!strcmp(verb, "mode")) {
        if (!*rest) app_status(a, memory_mode(a->story));
        else if (strcmp(rest, "auto") && strcmp(rest, "prefetch") && strcmp(rest, "lexical") &&
                 strcmp(rest, "sync") && strcmp(rest, "off"))
            app_status(a, "Recall modes: auto, prefetch, sync, lexical, off.");
        else {
            set(a->story, "memoryMode", rest);
            invalidate(a);
            app_status(a, "Recall mode saved");
        }
    } else if (!strcmp(verb, "off") || !strcmp(verb, "on")) {
        put(a->story, "memoryEnabled", cJSON_CreateBool(!strcmp(verb, "on")));
        invalidate(a);
        app_status(a, memory_enabled(a->story) ? "Compendium enabled" : "Compendium paused");
    } else if (!strcmp(verb, "retry")) {
        Json *node;
        cJSON_ArrayForEach(node, at(a->story, "nodes")) {
            if (!strcmp(js(node, "memoryFileState"), "error")) set(node, "memoryFileState", "pending");
            if (!strcmp(js(node, "memoryHistoryState"), "error"))
                cJSON_DeleteItemFromObjectCaseSensitive(node, "memoryHistoryState");
        }
        invalidate(a);
        app_status(a, "Failed agents queued for retry");
    } else if (!strcmp(verb, "refile") || !strcmp(verb, "undo")) {
        char *end;
        long id = strtol(rest, &end, 10);
        Json *node = end != rest && !*end && id >= 1 && id <= INT_MAX ? story_pick(a->story, (int)id) : NULL;
        if (!node) { app_status(a, "Give a message ID from /branch."); return; }
        if (!strcmp(verb, "undo")) {
            cJSON_DeleteItemFromObjectCaseSensitive(node, "memoryFile");
            cJSON_DeleteItemFromObjectCaseSensitive(node, "memoryRecap");
            set(node, "memoryFileState", "undone");
            set(node, "memoryHistoryState", "undone");
            app_status(a, "Agent output removed for this node");
        } else {
            if (strcmp(js(node, "role"), "assistant")) { app_status(a, "Give an assistant reply ID."); return; }
            set(node, "memoryFileState", "pending");
            cJSON_DeleteItemFromObjectCaseSensitive(node, "memoryHistoryState");
            app_status(a, "Reply queued for filing");
        }
        invalidate(a);
    } else if (!strcmp(verb, "recall")) {
        app_info(a, "Recall trace");
        append(&a->info, a->trace.len ? str(&a->trace) : "No model request has been sent in this session.");
    } else app_status(a, "/memory list, show, new, edit, delete, mode, status, retry, refile, undo, on, off");
}
