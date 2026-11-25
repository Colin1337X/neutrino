#include "neutrino.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

void append_n(Text *t, const char *s, size_t n)
{
    if (n == 0) return;
    if (n >= SIZE_MAX - t->len) abort();
    size_t need = t->len + n + 1;
    if (need > t->cap) {
        size_t cap = t->cap && t->cap <= SIZE_MAX / 2 ? t->cap * 2 : 128;
        if (cap < need) cap = need;
        char *next = realloc(t->data, cap);
        if (!next) abort();
        t->data = next;
        t->cap = cap;
    }
    memcpy(t->data + t->len, s, n);
    t->len += n;
    t->data[t->len] = 0;
}
void append(Text *t, const char *s) { append_n(t, s, strlen(s)); }
void clear(Text *t) { t->len = 0; if (t->data) *t->data = 0; }
void release(Text *t) { free(t->data); *t = (Text){0}; }
const char *str(const Text *t) { return t->data ? t->data : ""; }

void normalize_words(Text *t, const char *value)
{
    bool separator = false;
    for (; *value; value++) {
        unsigned char byte = (unsigned char)*value;
        if (byte < 128 && !isalnum(byte)) {
            separator = t->len > 0;
        } else {
            if (separator) append(t, " ");
            char word_byte = (char)(byte < 128 ? tolower(byte) : byte);
            append_n(t, &word_byte, 1);
            separator = false;
        }
    }
}

void format(Text *t, const char *pattern, ...)
{
    va_list args, measure;
    va_start(args, pattern);
    va_copy(measure, args);
    int n = vsnprintf(NULL, 0, pattern, measure);
    va_end(measure);
    if (n > 0) {
        char *buf = malloc((size_t)n + 1);
        if (!buf) abort();
        vsnprintf(buf, (size_t)n + 1, pattern, args);
        append_n(t, buf, (size_t)n);
        free(buf);
    }
    va_end(args);
}
char *copy(const char *s)
{
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (!out) abort();
    return memcpy(out, s, n);
}
Json *at(const Json *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
const char *js(const Json *o, const char *k)
{
    const char *s = cJSON_GetStringValue(at(o, k));
    return s ? s : "";
}
int number(const Json *o, const char *k)
{
    Json *v = at(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}
void put(Json *o, const char *k, Json *v)
{
    if (!v || !(at(o, k) ? cJSON_ReplaceItemInObjectCaseSensitive(o, k, v)
                         : cJSON_AddItemToObject(o, k, v))) abort();
}
void set(Json *o, const char *k, const char *v) { put(o, k, cJSON_CreateString(v)); }
void integer(Json *o, const char *k, int v) { put(o, k, cJSON_CreateNumber(v)); }
const char *env(const char *k, const char *fallback)
{
    const char *s = getenv(k);
    return s && *s ? s : fallback;
}

#ifdef _WIN32
static wchar_t *wide_path(const char *path)
{
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!size) { errno = EINVAL; return NULL; }
    wchar_t *out = malloc((size_t)size * sizeof(*out));
    if (!out) abort();
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, size);
    return out;
}
#endif

static char *read_bytes(const char *path, size_t *size, size_t limit)
{
#ifdef _WIN32
    wchar_t *wide = wide_path(path);
    if (!wide) return NULL;
    FILE *f = _wfopen(wide, L"rb");
    free(wide);
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f) return NULL;
    Text out = {0};
    char chunk[8192];
    size_t n;
    bool out_of_memory = false;
    while ((n = fread(chunk,1,sizeof chunk,f)) && n <= limit-out.len) {
        const size_t need = out.len+n+1;
        if (need > out.cap) {
            size_t cap = out.cap <= SIZE_MAX/2 ? out.cap*2 : out.cap;
            if (cap < need) cap = need;
            char *next = realloc(out.data,cap);
            if (!next) { out_of_memory = true; break; }
            out.data = next;
            out.cap = cap;
        }
        memcpy(out.data+out.len,chunk,n);
        out.len += n;
        out.data[out.len] = 0;
    }
    bool failed = n != 0 || ferror(f);
    fclose(f);
    if (failed) {
        release(&out);
        errno = out_of_memory ? ENOMEM : EIO;
        return NULL;
    }
    *size = out.len;
    return out.data ? out.data : copy("");
}
static char *read_text(const char *path, size_t limit)
{
    size_t n;
    char *data = read_bytes(path, &n, limit);
    if (data && memchr(data, 0, n)) {
        free(data);
        errno = EINVAL;
        return NULL;
    }
    return data;
}
char *read_file(const char *path) { return read_text(path,FILE_LIMIT); }

bool write_file(const char *path, const char *data, bool replace)
{
    Text temp = {0};
#ifdef _WIN32
    format(&temp, "%s.tmp.%lu", path, (unsigned long)GetCurrentProcessId());
    wchar_t *source = wide_path(str(&temp)), *target = wide_path(path);
    FILE *file = source && target ? _wfopen(source, L"wbx") : NULL;
#else
    format(&temp, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temp.data);
    FILE *file = fd == -1 ? NULL : fdopen(fd, "wb");
    if (!file && fd != -1) { close(fd); unlink(temp.data); }
#endif
    bool ok = file != NULL;
    if (file) {
        size_t len = strlen(data);
        ok = fwrite(data, 1, len, file) == len && fflush(file) == 0;
#ifdef _WIN32
        if (ok) ok = _commit(_fileno(file)) == 0;
#else
        if (ok) ok = fsync(fileno(file)) == 0;
#endif
        if (fclose(file)) ok = false;
#ifdef _WIN32
        if (ok) ok = MoveFileExW(source, target, MOVEFILE_WRITE_THROUGH |
                    (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != 0;
        if (!ok) DeleteFileW(source);
#else
        if (ok) ok = (replace ? rename(str(&temp), path) : link(str(&temp), path)) == 0;
        if (!replace || !ok) unlink(str(&temp));
#endif
    }
#ifdef _WIN32
    free(source);
    free(target);
#endif
    release(&temp);
    return ok;
}

static Json *ensure(Json *obj, const char *key, bool array)
{
    Json *value = at(obj, key);
    if (!value) {
        value = array ? cJSON_CreateArray() : cJSON_CreateObject();
        put(obj, key, value);
    }
    return value;
}

static bool id_valid(const char *id)
{
    if (!*id || strlen(id) > 160) return false;
    for (; *id; id++)
        if (!isalnum((unsigned char)*id) && *id != '_' && *id != '-') return false;
    return true;
}
static char *new_id(void)
{
    static unsigned serial;
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    Text id = {0};
    format(&id, "n-%llx-%08lx-%x", (unsigned long long)ts.tv_sec,
           (unsigned long)ts.tv_nsec, ++serial);
    return id.data;
}
static void assign_id(Json *record)
{
    char *id = new_id();
    set(record, "id", id);
    free(id);
}

static int path_depth(const char *path)
{
    if (*path != '/' || strlen(path) > 1024) return 0;
    int depth = 0;
    while (*path) {
        const char *part = ++path;
        while (*path && *path != '/') {
            if ((unsigned char)*path < 32 || *path == '\\') return 0;
            path++;
        }
        size_t len = (size_t)(path - part);
        if (!len || (len == 1 && *part == '.') ||
            (len == 2 && !strncmp(part, "..", len))) return 0;
        depth++;
    }
    return depth <= 4 ? depth : 0;
}
static Json *find_in(const Json *array, const char *field, const char *s)
{
    Json *item;
    cJSON_ArrayForEach(item, array)
        if (!strcmp(js(item, field), s)) return item;
    return NULL;
}
Json *scene_find(const Workspace *ws, const char *path)
{
    return find_in(at(ws->root, "records"), "path", path);
}
static const char *const record_types[] = {"folder", "book", "chapter", "scene"};

static void scene_normalize(Json *r, const char *path, int depth)
{
    const char *part = strrchr(path, '/');
    char parent[1025];
    size_t n = (size_t)(part - path);
    memcpy(parent, path, n);
    parent[n] = 0;
    if (!at(r, "id")) assign_id(r);
    set(r, "type", record_types[depth - 1]);
    set(r, "name", part + 1);
    set(r, "parentPath", n ? parent : "/");
    if (!at(r, "content")) set(r, "content", "");
    if (!at(r, "version")) integer(r, "version", 0);
    ensure(r, "revisions", true);
    ensure(r, "creativeControls", false);
}

static bool normalize(Json *root)
{
    Json *records = at(root, "records"), *rec, *entry;
    Json *version = at(root, "schemaVersion");
    if (!cJSON_IsObject(root) || !cJSON_IsArray(records) || !cJSON_IsObject(at(root, "config")) ||
        (version && (!cJSON_IsNumber(version) || (version->valuedouble != 1 && version->valuedouble != 2))))
        return false;
    cJSON_ArrayForEach(rec, records) {
        const char *path = js(rec, "path");
        int depth = path_depth(path);
        const char *type = js(rec, "type");
        if (!depth || (*type && strcmp(type, record_types[depth - 1])) ||
            (at(rec, "id") && !id_valid(js(rec, "id"))) ||
            find_in(records, "path", path) != rec ||
            (at(rec, "content") && !cJSON_IsString(at(rec, "content"))) ||
            (at(rec, "version") && !cJSON_IsNumber(at(rec, "version"))) ||
            (at(rec, "revisions") && !cJSON_IsArray(at(rec, "revisions"))) ||
            (at(rec, "creativeControls") && !cJSON_IsObject(at(rec, "creativeControls")))) return false;
        scene_normalize(rec, path, depth);
        if (find_in(records, "id", js(rec, "id")) != rec ||
            (depth > 1 && !find_in(records, "path", js(rec, "parentPath")))) return false;
        cJSON_ArrayForEach(entry, at(rec, "revisions"))
            if (!cJSON_IsString(at(entry, "content"))) return false;
    }
    const char *arrays[] = {"bible", "trackers", "profiles", "stories"};
    for (size_t i = 0; i < sizeof arrays / sizeof *arrays; i++)
        if (!cJSON_IsArray(ensure(root, arrays[i], true))) return false;
    if (at(root, "scratchpad") && !cJSON_IsString(at(root, "scratchpad"))) return false;
    cJSON_ArrayForEach(rec, at(root, "stories")) {
        if (!cJSON_IsArray(at(rec, "nodes")) || !*js(rec, "title")) return false;
        int expected = 1;
        cJSON_ArrayForEach(entry, at(rec, "nodes")) {
            int parent = number(entry, "parent");
            const char *role = js(entry, "role");
            if (number(entry, "id") != expected || parent < 0 || parent >= expected ||
                (strcmp(role, "user") && strcmp(role, "assistant")) ||
                !cJSON_IsString(at(entry, "content"))) return false;
            expected++;
        }
        if (number(rec, "head") < 0 || number(rec, "head") >= expected) return false;
    }
    integer(root, "schemaVersion", 2);
    return true;
}

struct WorkspaceLock {
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

static bool lock_workspace(Workspace *ws)
{
    Text name = {0};
    format(&name, "%s.lock", ws->path);
    struct WorkspaceLock *guard = malloc(sizeof(*guard));
    if (!guard) abort();
#ifdef _WIN32
    wchar_t *wide = wide_path(str(&name));
    guard->handle = wide ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) :
        INVALID_HANDLE_VALUE;
    free(wide);
    OVERLAPPED lock_range = {0};
    bool locked = guard->handle != INVALID_HANDLE_VALUE &&
        LockFileEx(guard->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, 1, 0, &lock_range);
    if (!locked && guard->handle != INVALID_HANDLE_VALUE) CloseHandle(guard->handle);
#else
    guard->fd = open(str(&name), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    bool locked = guard->fd >= 0 && flock(guard->fd, LOCK_EX | LOCK_NB) == 0;
    if (!locked && guard->fd >= 0) close(guard->fd);
#endif
    release(&name);
    if (locked) ws->lock = guard;
    else free(guard);
    return locked;
}

void workspace_close(Workspace *ws)
{
    cJSON_Delete(ws->root);
    ws->root = NULL;
    if (!ws->lock) return;
#ifdef _WIN32
    CloseHandle(ws->lock->handle);
#else
    close(ws->lock->fd);
#endif
    free(ws->lock);
    ws->lock = NULL;
}

bool workspace_open(Workspace *ws, const char *path, bool writable)
{
    *ws = (Workspace){.path = path};
    if (path && writable && !lock_workspace(ws)) return false;
    char *data = path ? read_text(path,SIZE_MAX-1) : NULL;
    if (path && !data && errno != ENOENT) { workspace_close(ws); return false; }
    ws->root = cJSON_ParseWithOpts(data ? data : "{\"records\":[],\"config\":{}}", NULL, 1);
    free(data);
    if (!normalize(ws->root)) {
        workspace_close(ws);
        return false;
    }
    return true;
}
static bool save_as(const Workspace *ws, const char *path, bool replace)
{
    Json *safe = cJSON_Duplicate(ws->root, true);
    cJSON_DeleteItemFromObjectCaseSensitive(at(safe, "config"), "apiKey");
    cJSON_DeleteItemFromObjectCaseSensitive(at(safe, "config"), "api_key");
    Json *profile;
    cJSON_ArrayForEach(profile, at(safe, "profiles")) {
        cJSON_DeleteItemFromObjectCaseSensitive(profile, "apiKey");
        cJSON_DeleteItemFromObjectCaseSensitive(profile, "api_key");
    }
    char *data = cJSON_PrintUnformatted(safe);
    bool ok = data && write_file(path, data, replace);
    cJSON_free(data);
    cJSON_Delete(safe);
    return ok;
}
bool workspace_save(Workspace *ws)
{
    if (!ws->dirty || !ws->path) return true;
    if (!save_as(ws, ws->path, true)) return false;
    ws->dirty = false;
    return true;
}
bool workspace_export(const Workspace *ws, const char *path) { return save_as(ws, path, false); }

Json *scene_add(Workspace *ws, const char *path)
{
    if (path_depth(path) != 4) return NULL;
    char partial[1025];
    strcpy(partial, path);
    Json *rec = NULL;
    for (int depth = 1; depth <= 4; depth++) {
        char *slash = strchr(partial + 1, '/');
        for (int i = 1; slash && i < depth; i++) slash = strchr(slash + 1, '/');
        if (slash) *slash = 0;
        rec = scene_find(ws, partial);
        if (!rec) {
            rec = cJSON_CreateObject();
            set(rec, "path", partial);
            scene_normalize(rec, partial, depth);
            cJSON_AddItemToArray(at(ws->root, "records"), rec);
            ws->dirty = true;
        }
        if (slash) *slash = '/';
    }
    return rec;
}

const char *scene_text(const Workspace *ws, const Json *scene)
{
    return scene ? js(scene, "content") : js(ws->root, "scratchpad");
}
bool scene_edit(Workspace *ws, Json *scene, const char *value, const char *reason)
{
    const char *old = scene_text(ws, scene);
    if (strlen(value) > EDIT_LIMIT) return false;
    if (!strcmp(old, value)) return true;
    if (scene) {
        Json *revision = cJSON_CreateObject();
        assign_id(revision);
        time_t now = time(NULL);
        char iso[32];
        struct tm *utc = gmtime(&now);
        if (utc && strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", utc)) set(revision, "at", iso);
        set(revision, "content", old);
        set(revision, "reason", reason);
        cJSON_AddItemToArray(at(scene, "revisions"), revision);
        int version = number(scene, "version");
        if (version < INT_MAX) integer(scene, "version", version + 1);
        set(scene, "content", value);
    } else set(ws->root, "scratchpad", value);
    ws->dirty = true;
    return true;
}

Json *story_add(Workspace *ws, const char *title, Json *card)
{
    Json *story = cJSON_CreateObject();
    assign_id(story);
    set(story, "title", title);
    put(story, "card", card ? card : cJSON_CreateObject());
    put(story, "nodes", cJSON_CreateArray());
    integer(story, "head", 0);
    cJSON_AddItemToArray(at(ws->root, "stories"), story);
    ws->dirty = true;
    return story;
}
Json *story_pick(const Json *story, int id)
{
    return id > 0 ? cJSON_GetArrayItem(at(story, "nodes"), id - 1) : NULL;
}
Json *story_find(const Workspace *ws, const char *id)
{
    return *id ? find_in(at(ws->root, "stories"), "id", id) : NULL;
}
int story_push(Workspace *ws, Json *story, int parent, const char *role, const char *content)
{
    Json *node = cJSON_CreateObject(), *nodes = at(story, "nodes");
    int id = nodes->child ? cJSON_GetArraySize(nodes) + 1 : 1;
    integer(node, "id", id);
    integer(node, "parent", parent);
    set(node, "role", role);
    set(node, "content", content);
    cJSON_AddItemToArray(nodes, node);
    integer(story, "head", id);
    ws->dirty = true;
    return id;
}
Json *story_path(const Json *story, int head)
{
    Json *path = cJSON_CreateArray();
    int count = cJSON_GetArraySize(at(story, "nodes"));
    if (head <= 0 || head > count) return path;
    Json **index = malloc(((size_t)count + 1) * sizeof(*index));
    if (!index) abort();
    Json *node;
    int i = 1;
    cJSON_ArrayForEach(node, at(story, "nodes")) index[i++] = node;
    Json *first = NULL, *last = NULL;
    for (int id = head; id; id = number(index[id], "parent")) {
        if (cJSON_IsTrue(at(index[id], "hidden"))) continue;
        Json *ref = cJSON_Duplicate(index[id], false);
        ref->child = index[id]->child;
        ref->type |= cJSON_IsReference;
        ref->next = first;
        if (first) first->prev = ref;
        else last = ref;
        first = ref;
    }
    if (first) first->prev = last;
    path->child = first;
    free(index);
    return path;
}

static size_t u32be(const unsigned char *p)
{
    return ((size_t)p[0] << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | p[3];
}
static Json *decode_card(const unsigned char *p, size_t n)
{
    const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Text decoded = {0};
    unsigned bits = 0, acc = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '=') break;
        if (isspace(p[i])) continue;
        const char *val = p[i] ? strchr(alphabet, p[i]) : NULL;
        if (!val) { release(&decoded); return NULL; }
        acc = (acc << 6) | (unsigned)(val - alphabet);
        bits += 6;
        if (bits >= 8) {
            char byte = (char)(acc >> (bits -= 8));
            if (!byte) { release(&decoded); return NULL; }
            append_n(&decoded, &byte, 1);
        }
    }
    Json *card = cJSON_ParseWithOpts(str(&decoded), NULL, 1);
    release(&decoded);
    return card;
}
static Json *png_card(const unsigned char *p, size_t total)
{
    Json *v2 = NULL;
    for (size_t i = 8; total - i >= 12;) {
        size_t len = u32be(p + i);
        if (len > total - i - 12) break;
        const unsigned char *d = p + i + 8;
        if (!memcmp(p + i + 4, "tEXt", 4) || !memcmp(p + i + 4, "iTXt", 4)) {
            const unsigned char *zero = memchr(d, 0, len);
            bool v3 = zero && zero - d == 4 && !memcmp(d, "ccv3", 4);
            if (zero && (v3 || (zero - d == 5 && !memcmp(d, "chara", 5)))) {
                const unsigned char *payload = zero + 1, *end = d + len;
                if (p[i + 4] == 'i') {
                    if (end - payload < 2 || *payload != 0) { i += len + 12; continue; }
                    payload += 2;
                    for (int k = 0; k < 2 && payload; k++) {
                        const unsigned char *sep = memchr(payload, 0, (size_t)(end - payload));
                        payload = sep ? sep + 1 : NULL;
                    }
                }
                Json *card = payload ? decode_card(payload, (size_t)(end - payload)) : NULL;
                if (card && v3) { cJSON_Delete(v2); return card; }
                if (card) { cJSON_Delete(v2); v2 = card; }
            }
        }
        if (!memcmp(p + i + 4, "IEND", 4)) break;
        i += len + 12;
    }
    return v2;
}

void story_expand_names(const Json *story, const char *source, Text *out)
{
    const Json *card = at(story, "card");
    const char *character = js(card, "nickname"), *user = js(at(story, "persona"), "name");
    if (!*character) character = js(card, "name");
    if (!*user) user = "User";
    for (const char *p = source; *p;) {
        if (*character && !strncmp(p, "{{char}}", 8)) {
            append(out, character);
            p += 8;
        } else if (!strncmp(p, "{{user}}", 8)) {
            append(out, user);
            p += 8;
        } else {
            const char *begin = p++;
            while (*p && *p != '{') p++;
            append_n(out, begin, (size_t)(p - begin));
        }
    }
}

static int push_card_greeting(Workspace *ws, Json *story, const char *greeting)
{
    Text expanded = {0};
    story_expand_names(story, greeting, &expanded);
    int id = story_push(ws, story, 0, "assistant", str(&expanded));
    release(&expanded);
    return id;
}

static Json *import_card(Workspace *ws, const Json *input)
{
    Json *data = at(input, "data");
    if (!cJSON_IsObject(data)) data = (Json *)input;
    const char *name = js(data, "name");
    if (!*name) return NULL;
    const char *greeting = js(data, "first_mes");
    if (!*greeting) greeting = js(data, "firstMessage");
    Json *story = story_add(ws, name, cJSON_Duplicate(data, true));
    if (*greeting) push_card_greeting(ws, story, greeting);
    int head = number(story, "head");
    Json *alt, *alternates = at(data, "alternate_greetings");
    if (!alternates) alternates = at(data, "greetings");
    cJSON_ArrayForEach(alt, alternates) {
        if (!cJSON_IsString(alt) || !*alt->valuestring) continue;
        int id = push_card_greeting(ws, story, alt->valuestring);
        if (!head) head = id;
    }
    integer(story, "head", head);
    return story;
}

bool story_lorebook(const Json *input)
{
    const Json *entries = at(input, "entries"), *entry;
    if (!cJSON_IsObject(entries) && !cJSON_IsArray(entries)) return false;
    cJSON_ArrayForEach(entry, entries)
        if (!cJSON_IsObject(entry) || !cJSON_IsString(at(entry, "content"))) return false;
    return true;
}

static Json *import_lorebook(Workspace *ws, const Json *input, const char *path)
{
    const char *title = js(input, "name");
    Text filename = {0};
    if (!*title) {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        const char *extension = strrchr(base, '.');
        if (!extension || extension == base) extension = base + strlen(base);
        append_n(&filename, base, (size_t)(extension - base));
        title = filename.len ? str(&filename) : "World Info";
    }
    Json *story = story_add(ws, title, nullptr);
    put(story, "lore", cJSON_Duplicate(input, true));
    release(&filename);
    return story;
}

static int source_node(const Json *story, const char *id)
{
    return number(find_in(at(story, "nodes"), "sourceId", id), "id");
}
static void journal_content(Json *row, Text *text)
{
    Json *block;
    cJSON_ArrayForEach(block, at(row, "blocks"))
        if (!strcmp(js(block, "k"), "text")) append(text, js(block, "text"));
}
static void journal_reasoning(Json *node, const Json *row)
{
    Text reasoning = {0};
    Json *block;
    cJSON_ArrayForEach(block, at(row, "blocks"))
        if (!strcmp(js(block, "k"), "reasoning")) append(&reasoning, js(block, "text"));
    if (reasoning.len) set(node, "reasoning", str(&reasoning));
    release(&reasoning);
}
static Json *import_journal(Workspace *ws, char *input)
{
    char *next = strchr(input, '\n');
    if (!next) return NULL;
    *next++ = 0;
    Json *header = cJSON_ParseWithOpts(input, NULL, 1);
    Json *cast = at(header, "cast"), *card = at(cast, "character");
    Json *story = NULL;
    if (!strcmp(js(header, "t"), "session") && number(header, "v") == 1 && *js(card, "name")) {
        const char *title = js(header, "title");
        story = story_add(ws, *title ? title : js(card, "name"), cJSON_Duplicate(card, true));
        Json *persona = at(cast, "persona");
        if (persona) put(story, "persona", cJSON_Duplicate(persona, true));
    }
    cJSON_Delete(header);
    if (!story) return NULL;
    while (*next) {
        char *line = next;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        while (isspace((unsigned char)*line)) line++;
        if (!*line) continue;
        Json *row = cJSON_ParseWithOpts(line, NULL, 1);
        const char *type = js(row, "t");
        bool ok = *type != 0;
        if (!strcmp(type, "msg")) {
            const char *role = js(row, "role"), *id = js(row, "id"), *parent = js(row, "parent");
            int pid = *parent ? source_node(story, parent) : 0, head = number(story, "head");
            ok = *id && !source_node(story, id) && (!*parent || pid) &&
                (!strcmp(role, "user") || !strcmp(role, "assistant"));
            if (ok) {
                Text content = {0};
                journal_content(row, &content);
                Json *node = story_pick(story, story_push(ws, story, pid, role, str(&content)));
                set(node, "sourceId", id);
                journal_reasoning(node, row);
                if (cJSON_IsTrue(at(row, "tail"))) integer(story, "head", head);
                release(&content);
            }
        } else if (!strcmp(type, "head")) {
            int leaf = source_node(story, js(row, "leaf"));
            ok = leaf != 0;
            if (ok) integer(story, "head", leaf);
        } else if (!strcmp(type, "hide") || !strcmp(type, "edit")) {
            Json *node = story_pick(story, source_node(story, js(row, "target")));
            ok = node != NULL;
            if (node && !strcmp(type, "hide")) put(node, "hidden", cJSON_CreateBool(cJSON_IsTrue(at(row, "hidden"))));
            else if (node) {
                Text content = {0};
                journal_content(row, &content);
                set(node, "content", str(&content));
                cJSON_DeleteItemFromObjectCaseSensitive(node, "reasoning");
                journal_reasoning(node, row);
                release(&content);
            }
        } else if (!strcmp(type, "name") && !*js(row, "leaf") && *js(row, "title")) {
            set(story, "title", js(row, "title"));
        } else if (!strcmp(type, "persona") && cJSON_IsObject(at(row, "persona"))) {
            put(story, "persona", cJSON_Duplicate(at(row, "persona"), true));
        }
        cJSON_Delete(row);
        if (!ok) {
            cJSON_Delete(cJSON_DetachItemViaPointer(at(ws->root, "stories"), story));
            return NULL;
        }
    }
    return story;
}

static Json *import_sillytavern(Workspace *ws, const char *input)
{
    const char *following = strchr(input, '\n');
    if (!following) return NULL;
    Text first = {0};
    append_n(&first, input, (size_t)(following - input));
    Json *header = cJSON_ParseWithOpts(str(&first), NULL, 1);
    release(&first);
    const char *character = js(header, "character_name"), *player = js(header, "user_name");
    if (!*character || !*player || cJSON_IsTrue(at(header, "is_group")) || at(header, "group_id")) {
        cJSON_Delete(header);
        return NULL;
    }
    bool dirty = ws->dirty;
    Json *card = NULL, *other;
    cJSON_ArrayForEach(other, at(ws->root, "stories")) {
        Json *source = at(other, "card");
        if (!strcmp(js(source, "name"), character)) {
            card = cJSON_Duplicate(source, true);
            break;
        }
    }
    if (!card) {
        card = cJSON_CreateObject();
        set(card, "name", character);
    }
    Json *story = story_add(ws, character, card);
    Json *persona = cJSON_CreateObject();
    set(persona, "name", player);
    put(story, "persona", persona);
    cJSON_Delete(header);
    const char *line = following + 1;
    bool valid = true;
    while (*line && valid) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        Text row = {0};
        append_n(&row, line, (size_t)(end - line));
        line = *end ? end + 1 : end;
        if (!*str(&row) || !strcmp(str(&row), "\r")) { release(&row); continue; }
        Json *message = cJSON_ParseWithOpts(str(&row), NULL, 1);
        release(&row);
        if (!cJSON_IsString(at(message, "mes")) || !cJSON_IsBool(at(message, "is_user")) ||
            at(message, "group_id")) {
            cJSON_Delete(message);
            valid = false;
            break;
        }
        int parent = number(story, "head"), head = 0;
        bool user = cJSON_IsTrue(at(message, "is_user"));
        bool hidden = cJSON_IsTrue(at(message, "is_system")) || cJSON_IsTrue(at(message, "is_hidden"));
        const char *role = user ? "user" : "assistant";
        Json *swipes = at(message, "swipes"), *swipe;
        if (!user && cJSON_IsArray(swipes)) {
            int slot = 0, picked = number(message, "swipe_id");
            cJSON_ArrayForEach(swipe, swipes) {
                if (cJSON_IsString(swipe)) {
                    const char *content = slot == picked ? js(message, "mes") : swipe->valuestring;
                    int id = story_push(ws, story, parent, role, content);
                    if (slot == picked) head = id;
                    if (hidden) put(story_pick(story, id), "hidden", cJSON_CreateTrue());
                }
                slot++;
            }
        }
        if (!head) {
            head = story_push(ws, story, parent, role, js(message, "mes"));
            if (hidden) put(story_pick(story, head), "hidden", cJSON_CreateTrue());
        }
        integer(story, "head", head);
        cJSON_Delete(message);
    }
    if (!valid || !number(story, "head")) {
        cJSON_Delete(cJSON_DetachItemViaPointer(at(ws->root, "stories"), story));
        ws->dirty = dirty;
        return NULL;
    }
    return story;
}

Json *workspace_import(Workspace *ws, const char *path, Text *error)
{
    size_t size;
    char *data = read_bytes(path, &size, FILE_LIMIT);
    if (!data) { append(error, "Could not read import file."); return NULL; }
    Json *result = workspace_import_data(ws, path, data, size, error);
    free(data);
    return result;
}

Json *workspace_import_data(Workspace *ws, const char *name, const char *data, size_t size, Text *error)
{
    Json *input = NULL, *result = NULL;
    if (size >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) input = png_card((const unsigned char *)data, size);
    else if (!memchr(data, 0, size)) {
        const char *json = size >= 3 && !memcmp(data, "\xef\xbb\xbf", 3) ? data + 3 : data;
        input = cJSON_ParseWithOpts(json, NULL, 1);
        if (!input) {
            bool dirty = ws->dirty;
            result = import_sillytavern(ws, json);
            if (!result) {
                char *journal = copy(json);
                result = import_journal(ws, journal);
                free(journal);
            }
            if (!result) ws->dirty = dirty;
        }
    }
    if (at(input, "records")) {
        if (!normalize(input)) append(error, "Invalid Xenon workspace or missing scene parents.");
        else {
            Json *r;
            const char *arrays[] = {"records", "bible", "stories", "profiles", "trackers", "trash"};
            bool collision = false;
            for (size_t i = 0; i < sizeof arrays / sizeof *arrays; i++) {
                cJSON_ArrayForEach(r, at(input, arrays[i])) {
                    Json *existing = at(ws->root, arrays[i]);
                    if ((*js(r, "id") && find_in(existing, "id", js(r, "id"))) ||
                        (*js(r, "path") && find_in(existing, "path", js(r, "path")))) collision = true;
                }
            }
            if (collision) append(error, "Import has conflicting paths or IDs. Open it as a separate workspace.");
            else {
                if (!cJSON_GetArraySize(at(ws->root, "records")) &&
                    !cJSON_GetArraySize(at(ws->root, "stories")) && !*js(ws->root, "scratchpad") &&
                    !cJSON_GetArraySize(at(ws->root, "bible")) &&
                    !cJSON_GetArraySize(at(ws->root, "profiles")) &&
                    !cJSON_GetArraySize(at(ws->root, "trackers")) && !*js(ws->root, "preferences") &&
                    !at(ws->root, "neutrinoSelection") && !at(ws->root, "config")->child) {
                    cJSON_Delete(ws->root);
                    ws->root = input;
                    input = NULL;
                } else {
                    for (size_t i = 0; i < sizeof arrays / sizeof *arrays; i++) {
                        cJSON_ArrayForEach(r, at(input, arrays[i]))
                            cJSON_AddItemToArray(ensure(ws->root, arrays[i], true), cJSON_Duplicate(r, true));
                    }
                }
                result = ws->root;
                ws->dirty = true;
            }
        }
    } else if (story_lorebook(input)) result = import_lorebook(ws, input, name);
    else if (input) result = import_card(ws, input);
    if (!result && !error->len) append(error, "Expected a workspace, journal, solo SillyTavern chat, JSON/PNG card, or World Info JSON.");
    cJSON_Delete(input);
    return result;
}

static bool inside(const char *path, const char *scope)
{
    if (!strcmp(scope, "/")) return *path == '/';
    size_t n = strlen(scope);
    return !strncmp(path, scope, n) && (path[n] == '/' || !path[n]);
}
static bool bounded_source(Text *out, const char *label, const char *value, size_t limit)
{
    if (!*value) return true;
    if (out->len + strlen(value) + strlen(label) + 3 > limit) return false;
    format(out, "%s\n", label);
    format(out, "%s\n\n", value);
    return true;
}
static bool keyword_byte(unsigned char ch)
{
    return isalnum(ch) || ch == '_' || ch >= 0x80;
}

static bool lore_keyword(const char *text, const char *needle, bool sensitive, bool whole)
{
    if (!*needle) return false;
    for (const char *start = text; *start; start++) {
        if (whole && start > text && keyword_byte((unsigned char)start[-1])) continue;
        const char *a = start, *b = needle;
        while (*a && *b && (sensitive ? *a == *b :
               tolower((unsigned char)*a) == tolower((unsigned char)*b))) { a++; b++; }
        if (!*b && (!whole || !keyword_byte((unsigned char)*a))) return true;
    }
    return false;
}

static void lore_matches(const Json *story, const Json *keys, const char *recent, bool sensitive,
                         bool whole, const char **first, bool *all)
{
    Json *key;
    *first = nullptr;
    *all = true;
    cJSON_ArrayForEach(key, keys) {
        if (!cJSON_IsString(key) || !*key->valuestring) continue;
        Text expanded = {0};
        story_expand_names(story, key->valuestring, &expanded);
        if (lore_keyword(recent, str(&expanded), sensitive, whole)) {
            if (!*first) *first = key->valuestring;
        } else *all = false;
        release(&expanded);
    }
}

static const Json *lore_option(const Json *entry, const char *key, const char *portable)
{
    const Json *value = at(entry, key);
    if (!value || cJSON_IsNull(value)) value = at(entry, portable);
    if (!value || cJSON_IsNull(value)) value = at(at(entry, "extensions"), key);
    return value;
}

static void lore_recent(const Json *path, const char *cue, int depth, Text *out)
{
    int skip = cJSON_GetArraySize(path) - depth + (cue != nullptr);
    Json *node;
    cJSON_ArrayForEach(node, path) {
        if (skip-- > 0) continue;
        append(out, js(node, "content"));
        append(out, "\n");
    }
    if (cue && depth) append(out, cue);
}

static void collect_lore(const Json *story, const Json *book, const char *label, const char *recent,
                         const Json *path, const char *cue,
                         Text *out, size_t limit, Text *trace)
{
    Json *entry;
    int index = 0;
    cJSON_ArrayForEach(entry, at(book, "entries")) {
        const char *title = js(entry, "title");
        if (!*title) title = js(entry, "comment");
        if (!*title) title = js(entry, "name");
        if (!*title) title = "Story reference";
        index++;
        if (trace) format(trace, "%s #%d — %s: ", label, index, title);
        bool enabled = !cJSON_IsFalse(at(entry, "enabled")) && !cJSON_IsTrue(at(entry, "disable"));
        bool constant = cJSON_IsTrue(at(entry, "constant"));
        bool sensitive = cJSON_IsTrue(lore_option(entry, "caseSensitive", "case_sensitive"));
        bool whole = cJSON_IsTrue(lore_option(entry, "matchWholeWords", "match_whole_words"));
        const Json *depth = lore_option(entry, "scanDepth", "scan_depth");
        Text scoped = {0};
        int scan = 4;
        const char *haystack = recent;
        if (cJSON_IsNumber(depth) && depth->valueint >= 0 && depth->valueint <= 1000) {
            scan = depth->valueint;
            lore_recent(path, cue, scan, &scoped);
            haystack = str(&scoped);
        }
        Json *keys = at(entry, "keys");
        if (!keys) keys = at(entry, "key");
        const char *match, *second = nullptr;
        bool all;
        lore_matches(story, keys, haystack, sensitive, whole, &match, &all);
        Json *secondary = at(entry, "secondary_keys");
        if (!secondary) secondary = at(entry, "secondaryKeys");
        if (!secondary) secondary = at(entry, "keysecondary");
        const Json *selective = at(entry, "selective");
        bool needs_secondary = !cJSON_IsFalse(selective) && cJSON_IsArray(secondary) && secondary->child;
        const Json *logic = lore_option(entry, "selectiveLogic", "selective_logic");
        int mode = cJSON_IsNumber(logic) ? logic->valueint : 0;
        bool matched_secondary = true;
        if (needs_secondary) {
            lore_matches(story, secondary, haystack, sensitive, whole, &second, &all);
            matched_secondary = mode == 1 ? !all : mode == 2 ? !second : mode == 3 ? all : second != nullptr;
        }
        if (!enabled) { if (trace) append(trace, "skipped (disabled)\n"); }
        else if (!constant && !match) {
            if (trace && scan == 4) append(trace, "skipped (no keyword in the latest four visible messages)\n");
            else if (trace) format(trace, "skipped (no keyword in the latest %d visible messages)\n", scan);
        } else if (!constant && !matched_secondary) {
            if (trace) format(trace, "skipped (matched '%s', %s)\n", match,
                mode ? "secondary keyword condition failed" : "required secondary keyword absent");
        } else if (!*js(entry, "content")) {
            if (trace) append(trace, "skipped (empty content)\n");
        } else {
            Text heading = {0}, content = {0};
            format(&heading, "Lore: %s", title);
            story_expand_names(story, js(entry, "content"), &content);
            bool included = bounded_source(out, str(&heading), str(&content), limit);
            if (trace) {
                if (!included) append(trace, "skipped (matched, but over source budget)\n");
                else if (constant) append(trace, "included (constant entry)\n");
                else if (needs_secondary && mode) format(trace, "included (matched '%s', secondary keyword condition passed)\n", match);
                else if (second) format(trace, "included (matched '%s' and '%s')\n", match, second);
                else format(trace, "included (matched '%s')\n", match);
            }
            release(&heading);
            release(&content);
        }
        release(&scoped);
    }
}
int workspace_context_budget(const Workspace *ws)
{
    int budget = number(ws->root, "neutrinoBudget");
    if (!budget) budget = number(ws->root, "contextBudget");
    return budget >= 128 && budget <= 32768 ? budget : 5000;
}

bool collect_context_with_cue(const Workspace *ws, const Json *scene, const Json *story, int head,
                              const char *cue, Text *out, Text *trace)
{
    Json *rec, *field, *bible = at(ws->root, "bible"), *records = at(ws->root, "records");
    const char *path = scene ? js(scene, "path") : js(story, "scene");
    bool fits = true;
    size_t limit = (size_t)workspace_context_budget(ws) * 4;
    fits &= bounded_source(out, "Writing preferences", js(ws->root, "preferences"), limit);
    Json *controls = cJSON_CreateObject();
    cJSON_ArrayForEach(field, at(at(ws->root, "config"), "creativeControls"))
        put(controls, field->string, cJSON_Duplicate(field, true));
    for (int depth = 1; depth <= 4; depth++) {
        cJSON_ArrayForEach(rec, records) {
            const char *ancestor = js(rec, "path");
            if (path_depth(ancestor) != depth || !inside(path, ancestor)) continue;
            fits &= bounded_source(out, ancestor, js(rec, "premise"), limit);
            fits &= bounded_source(out, ancestor, js(rec, "instructions"), limit);
            cJSON_ArrayForEach(field, at(rec, "creativeControls"))
                put(controls, field->string, cJSON_Duplicate(field, true));
        }
    }
    if (controls->child) {
        char *values = cJSON_PrintUnformatted(controls);
        if (values) fits &= bounded_source(out, "Creative controls", values, limit);
        cJSON_free(values);
    }
    cJSON_Delete(controls);
    Json *linked = story && *path ? scene_find(ws, path) : NULL;
    if (linked) fits &= bounded_source(out, "Linked scene", js(linked, "content"), limit);
    for (int pass = 0; pass < 2; pass++) {
        cJSON_ArrayForEach(rec, bible) {
            const char *book = js(rec, "book"), *id = js(rec, "id"), *status = js(rec, "status");
            const char *policy = js(at(ws->root, "contextPolicy"), id);
            bool pinned = !strcmp(policy, "pin");
            if ((!pinned && !inside(path, book) && strcmp(book, "/")) ||
                !strcmp(policy, "exclude") || (pass == 0) != pinned ||
                (!pinned && (!strcmp(status, "outdated") || !strcmp(status, "contradicted")))) continue;
            Text title = {0};
            format(&title, "%s (%s, %s)", js(rec, "name"), js(rec, "type"), status);
            bool added = bounded_source(out, str(&title), js(rec, "description"), limit);
            if (pinned) fits &= added;
            release(&title);
        }
        cJSON_ArrayForEach(rec, records) {
            const char *other = js(rec, "path"), *policy = js(at(ws->root, "contextPolicy"), js(rec, "id"));
            bool pinned = !strcmp(policy, "pin");
            if ((!*path && !pinned) || strcmp(js(rec, "type"), "scene") || !strcmp(other, path) ||
                (pass == 0) != pinned || !strcmp(policy, "exclude")) continue;
            const char *folder = *path ? strchr(path + 1, '/') : NULL;
            const char *book = folder ? strchr(folder + 1, '/') : NULL;
            if (!pinned && (!book || strncmp(path, other, (size_t)(book - path + 1)) || strcmp(other, path) >= 0)) continue;
            bool added = bounded_source(out, other, js(rec, "content"), limit);
            if (pinned) fits &= added;
        }
        if (pass == 0 && story) {
            Text recent = {0};
            Json *active = story_path(story, head);
            lore_recent(active, cue, 4, &recent);
            Json *card = at(story, "card");
            collect_lore(story, at(card, "character_book"), "Card lore", str(&recent), active, cue, out, limit, trace);
            collect_lore(story, at(card, "book"), "Card book", str(&recent), active, cue, out, limit, trace);
            collect_lore(story, at(at(story, "persona"), "book"), "Persona book", str(&recent), active, cue, out, limit, trace);
            collect_lore(story, at(story, "lore"), "Attached lore", str(&recent), active, cue, out, limit, trace);
            release(&recent);
            cJSON_Delete(active);
        }
    }
    return fits;
}
bool collect_context(const Workspace *ws, const Json *scene, const Json *story, int head, Text *out, Text *trace)
{
    return collect_context_with_cue(ws, scene, story, head, nullptr, out, trace);
}

void workspace_demo(Workspace *ws)
{
    Json *scene = scene_add(ws, "/Signal Archive/The Night Relay/01 Arrivals/The Last Broadcast");
    Json *book = scene_find(ws, "/Signal Archive/The Night Relay");
    set(book, "premise", "An island station receives transmissions from cities that no longer exist.");
    scene_edit(ws, scene,
        "By dusk the tide had swallowed the causeway. Iona watched the last marker disappear, then climbed to the relay room with her coat still wet.\n\n"
        "The receiver was cold. She turned its dial anyway. A strip of green light crawled across the glass, paused, and moved again.\n\n"
        "\"Night Relay,\" she said into the dead microphone. \"Who is this?\"\n\n"
        "Her own voice came back first. Then someone took a breath.", "sample");
    cJSON_DeleteItemFromObjectCaseSensitive(scene, "revisions");
    put(scene, "revisions", cJSON_CreateArray());
    integer(scene, "version", 0);
    scene_edit(ws, scene_add(ws, "/Signal Archive/The Night Relay/01 Arrivals/Low Tide"),
               "There were seven lanterns on the pier. This morning, someone had lit an eighth.", "sample");
    Json *entry = cJSON_CreateObject();
    assign_id(entry);
    set(entry, "book", js(book, "path"));
    set(entry, "type", "location");
    set(entry, "name", "Night Relay");
    set(entry, "status", "canon");
    set(entry, "description", "A disused radio station on an island reachable on foot at low tide.");
    cJSON_AddItemToArray(at(ws->root, "bible"), entry);
    Json *card = cJSON_CreateObject();
    set(card, "name", "Iona Vale");
    set(card, "description", "A relay keeper who records the time of each impossible transmission.");
    Json *story = story_add(ws, "The voice on channel nine", card);
    set(story, "scene", js(scene, "path"));
    story_push(ws, story, 0, "assistant",
               "Iona lifts a finger to her lips. The receiver has been dead for thirteen years, but a voice is counting down on channel nine. She slides a pencil toward you.\n\n\"Write down the next thing you hear.\"");
    ws->dirty = false;
}
