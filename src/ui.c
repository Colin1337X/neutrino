#include "app.h"
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT "\x1b[48;2;15;19;23m"
#define RAIL "\x1b[48;2;21;26;30m"
#define FIELD "\x1b[48;2;30;36;41m"
#define PICK "\x1b[48;2;29;56;54m"
#define PLUM "\x1b[48;2;54;43;29m"
#define TXT "\x1b[38;2;236;232;220m"
#define DIM "\x1b[38;2;173;184;180m"
#define QUIET "\x1b[38;2;144;159;155m"
#define LINE "\x1b[38;2;58;72;74m"
#define MINT "\x1b[38;2;147;227;204m"
#define AMBER "\x1b[38;2;246;200;139m"
#define GOLD "\x1b[38;2;252;215;155m"
#define BOLD "\x1b[1m"
#define NORMAL "\x1b[22m"

typedef struct {
    int left, width, top, height, rail, main, main_x, detail, detail_x, input_y, content_end;
    bool dense, library_only;
} Layout;
typedef struct { size_t start, end; } Row;
typedef struct { Row *items; int count, capacity; } Rows;

static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }
static int clamp(int value, int low, int high) { return minimum(maximum(value, low), high); }
static Layout layout(const App *a)
{
    Layout l = {0};
    l.width = minimum(a->cols, 164);
    l.left = (a->cols - l.width) / 2;
    l.dense = a->rows < 19 || l.width < 54;
    l.top = l.dense ? 2 : 3;
    l.input_y = a->rows - (l.dense ? 2 : 3);
    l.content_end = l.input_y - (l.dense ? 0 : 2);
    l.height = a->rows - l.top - 1;
    bool show_sides = !a->compact && !l.dense && !(a->view_panel && a->panel_neofetch);
    if (show_sides && l.width >= 80) l.rail = clamp(l.width / 5, 22, 29);
    if (show_sides && !a->view_panel && l.width >= 120) l.detail = clamp(l.width / 6, 23, 28);
    if (a->focus == LIBRARY && !l.rail) {
        l.rail = l.width;
        l.library_only = true;
    }
    l.main_x = l.left + l.rail;
    l.main = l.width - l.rail - l.detail;
    l.detail_x = l.main_x + l.main;
    return l;
}
static void position(App *a, int x, int y) { format(&a->screen, "\x1b[%d;%dH", y + 1, x + 1); }
static void styled(App *a, const char *style) { append(&a->screen, style); }
static void spaces(App *a, int n)
{
    static const char space[] = "                                                                ";
    while (n > 0) {
        int size = minimum(n, (int)sizeof space - 1);
        append_n(&a->screen, space, (size_t)size);
        n -= size;
    }
}
static int display_width(const char *s, size_t len)
{
    int width = 0;
    for (size_t i = 0; i < len;) {
        size_t j = utf_next(s, len, i);
        width += utf_width(s + i, j - i);
        i = j;
    }
    return width;
}
static int display(App *a, const char *s, size_t n, int width)
{
    int used = 0;
    for (size_t i = 0; i < n;) {
        size_t j = utf_next(s, n, i);
        int w = utf_width(s + i, j - i);
        if (used + w > width) break;
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) spaces(a, w);
        else if ((c >= 0x80 && c < 0xc2) || c > 0xf4 ||
                 (c >= 0xc2 && j - i != (c < 0xe0 ? 2u : c < 0xf0 ? 3u : 4u)))
            append(&a->screen, "�");
        else if ((c == 0xc2 && j - i == 2 && (unsigned char)s[i + 1] < 0xa0) ||
                 (c == 0xe2 && j - i == 3 && (unsigned char)s[i + 1] == 0x80 &&
                  (unsigned char)s[i + 2] >= 0xaa && (unsigned char)s[i + 2] <= 0xae))
            spaces(a, w);
        else append_n(&a->screen, s + i, j - i);
        used += w;
        i = j;
    }
    return used;
}
static void cell(App *a, int x, int y, int width, const char *style, const char *label)
{
    if (width <= 0 || x < 0 || y < 0 || y >= a->rows) return;
    position(a, x, y);
    styled(a, style);
    display(a, label, strlen(label), minimum(width, a->cols - x));
}
static void clipped_cell(App *a, int x, int y, int width, const char *style, const char *label)
{
    if (width < 2 || display_width(label, strlen(label)) <= width) {
        cell(a, x, y, width, style, label);
        return;
    }
    cell(a, x, y, width - 1, style, label);
    cell(a, x + width - 1, y, 1, style, "…");
}
static void paint(App *a, int x, int y, int width, const char *style)
{
    if (width <= 0 || y < 0 || y >= a->rows) return;
    position(a, x, y);
    styled(a, style);
    spaces(a, width);
}
static void rule(App *a, int x, int y, int width, const char *style)
{
    position(a, x, y);
    styled(a, style);
    for (int i = 0; i < width; i++) append(&a->screen, "─");
}
static void right(App *a, int edge, int y, int max, const char *style, const char *label)
{
    int width = minimum(display_width(label, strlen(label)), max);
    cell(a, edge - width, y, width, style, label);
}
static void edge(App *a, int x, int y, int width, const char *style, bool top)
{
    cell(a, x, y, 1, style, top ? "╭" : "╰");
    rule(a, x + 1, y, width - 2, style);
    cell(a, x + width - 1, y, 1, style, top ? "╮" : "╯");
}
static void card_row(App *a, int x, int y, int width, const char *fill, const char *border)
{
    paint(a, x + 1, y, width - 2, fill);
    cell(a, x, y, 1, border, "│");
    cell(a, x + width - 1, y, 1, border, "│");
}
static void card(App *a, int x, int y, int width, int height, const char *border)
{
    edge(a, x, y, width, border, true);
    for (int row = 1; row < height - 1; row++) card_row(a, x, y + row, width, RAIL, border);
    edge(a, x, y + height - 1, width, border, false);
}

static void add_row(Rows *r, size_t start, size_t end)
{
    if (r->count == r->capacity) {
        int capacity = r->capacity ? r->capacity * 2 : 64;
        Row *items = realloc(r->items, (size_t)capacity * sizeof(*items));
        if (!items) abort();
        r->items = items;
        r->capacity = capacity;
    }
    r->items[r->count++] = (Row){start, end};
}
static Rows wrap(const char *s, size_t len, int width)
{
    Rows rows = {0};
    size_t start = 0, i = 0, space = SIZE_MAX;
    int used = 0;
    width = maximum(4, width);
    while (i < len) {
        if (s[i] == '\r' && i + 1 < len && s[i + 1] == '\n') {
            add_row(&rows, start, i);
            i += 2;
            start = i;
            used = 0;
            space = SIZE_MAX;
            continue;
        }
        if (s[i] == '\n') {
            add_row(&rows, start, i++);
            start = i;
            used = 0;
            space = SIZE_MAX;
            continue;
        }
        size_t next = utf_next(s, len, i);
        int col = utf_width(s + i, next - i);
        if (used && used + col > width) {
            if (s[i] == ' ') {
                add_row(&rows, start, i++);
            } else {
                if (space != SIZE_MAX && space > start) i = space;
                add_row(&rows, start, i);
            }
            start = i;
            used = 0;
            space = SIZE_MAX;
            continue;
        }
        used += col;
        i = next;
        if (s[i - 1] == ' ' || s[i - 1] == '\t') space = i;
    }
    add_row(&rows, start, len);
    return rows;
}
static int row_of(const Rows *r, size_t pos)
{
    int lo = 0, hi = r->count - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (r->items[mid].start <= pos) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}
static int content_width(const Layout *l) { return maximum(8, minimum(78, l->main - (l->dense ? 4 : 8))); }

static int content_x(const Layout *l) { return l->main_x + (l->main - content_width(l)) / 2; }
size_t ui_move_editor(const App *a, int delta)
{
    Layout l = layout(a);
    const char *s = str(&a->edit);
    Rows rows = wrap(s, a->edit.len, content_width(&l));
    int from = row_of(&rows, a->caret), to = clamp(from + delta, 0, rows.count - 1);
    int width = display_width(s + rows.items[from].start, a->caret - rows.items[from].start);
    Row row = rows.items[to];
    size_t pos = row.start;
    int col = 0;
    while (pos < row.end && col < width) {
        size_t next = utf_next(s, row.end, pos);
        col += utf_width(s + pos, next - pos);
        if (col > width) break;
        pos = next;
    }
    if (to + 1 < rows.count && pos > row.start && pos == rows.items[to + 1].start)
        pos = utf_prev(s, pos);
    free(rows.items);
    return pos;
}

int ui_library_size(const App *a)
{
    if (a->roleplay) return cJSON_GetArraySize(at(a->ws.root, "stories"));
    int n = 1;
    Json *rec;
    cJSON_ArrayForEach(rec, at(a->ws.root, "records"))
        if (!strcmp(js(rec, "type"), "scene")) n++;
    return n;
}
Json *ui_library_at(const App *a, int i)
{
    if (a->roleplay) return cJSON_GetArrayItem(at(a->ws.root, "stories"), i);
    if (i < 1) return NULL;
    Json *rec;
    cJSON_ArrayForEach(rec, at(a->ws.root, "records"))
        if (!strcmp(js(rec, "type"), "scene") && --i == 0) return rec;
    return NULL;
}
void ui_clamp_rail(App *a)
{
    Json *selected = a->roleplay ? a->story : a->scene;
    int end = ui_library_size(a);
    a->rail_cursor = 0;
    a->rail_offset = 0;
    for (int i = 0; selected && i < end; i++) {
        if (ui_library_at(a, i) == selected) { a->rail_cursor = i; break; }
    }
}
int ui_command_matches(const App *a, int *results, int cap)
{
    int count = 0;
    const char *needle = a->prompt.len ? str(&a->prompt) + 1 : "";
    size_t len = strlen(needle);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < command_count && count < cap; i++) {
            bool begins = !strncmp(commands[i].name, needle, len);
            if ((pass == 0 && begins) || (pass == 1 && !begins && strstr(commands[i].name, needle)))
                results[count++] = (int)i;
        }
    }
    return count;
}

static void header(App *a, const Layout *l)
{
    const int y = l->dense ? 0 : 1;
    const int x = l->left + (l->dense ? 1 : 2);
    for (int row = 0; row <= y; ++row) paint(a, l->left, row, l->width, RAIL);
    cell(a, x, y, 12, RAIL TXT BOLD, "▰ Neutrino");
    if (l->width >= 48) {
        cell(a, x + 15, y, 14, a->roleplay ? RAIL DIM NORMAL : PICK MINT BOLD, "  Manuscript  ");
        cell(a, x + 30, y, 12, a->roleplay ? PLUM AMBER BOLD : RAIL DIM NORMAL, "  Roleplay  ");
    } else {
        cell(a, x + 12, y, l->width - 13, a->roleplay ? PLUM AMBER BOLD : PICK MINT BOLD,
             a->roleplay ? " Roleplay " : " Manuscript ");
    }
    if (l->width >= 71)
        right(a, l->left + l->width - 2, y, 24, a->ssh_session ? RAIL MINT NORMAL : RAIL QUIET NORMAL,
              a->demo ? "Demo workspace" : a->ssh_session ? "SSH workspace" : "Local workspace");
    else if (a->ssh_session && (l->width >= 58 || (l->width >= 36 && l->width < 48)))
        right(a, l->left + l->width - 2, y, 4, RAIL MINT NORMAL, "SSH");
    if (l->width >= 53 && l->width < 90) cell(a, x + 44, y, 2, RAIL QUIET NORMAL, "F2");
    rule(a, l->left, l->top - 1, l->width, RAIL LINE NORMAL);
}

static void library(App *a, const Layout *l)
{
    if (!l->rail || l->height < 4) return;
    int x = l->left, width = l->rail - (l->library_only ? 0 : 1);
    for (int row = 0; row < l->height; ++row) {
        paint(a, x, l->top + row, width, RAIL);
        if (!l->library_only) cell(a, x + width, l->top + row, 1, RAIL LINE NORMAL, "│");
    }
    cell(a, x + 2, l->top, width - 4, RAIL TXT BOLD, a->roleplay ? "Stories" : "Library");
    if (width >= 17 && a->focus != LIBRARY)
        right(a, x + width - 2, l->top, 2, RAIL QUIET NORMAL, "F3");
    int capacity = maximum(1, l->height - 3), count = ui_library_size(a);
    if (a->rail_cursor >= a->rail_offset + capacity) a->rail_offset = a->rail_cursor - capacity + 1;
    if (a->rail_cursor < a->rail_offset) a->rail_offset = a->rail_cursor;
    int y = l->top + 2;
    for (int i = a->rail_offset; i < count && i < a->rail_offset + capacity; i++, y++) {
        Json *item = ui_library_at(a, i);
        const char *name = a->roleplay ? js(item, "title") : item ? js(item, "name") : "Scratchpad";
        bool active = item == (a->roleplay ? a->story : a->scene);
        bool cursor = a->focus == LIBRARY && i == a->rail_cursor;
        const char *style = active || cursor ? (a->roleplay ? PLUM AMBER : PICK MINT) : RAIL DIM;
        paint(a, x, y, width, style);
        cell(a, x + 1, y, 1, style, cursor ? "▸" : active ? "▏" : " ");
        clipped_cell(a, x + 3, y, width - 4, style, name);
    }
    if (!count) cell(a, x + 2, y, width - 4, RAIL QUIET, "No stories yet");
    if (a->focus == LIBRARY)
        cell(a, x + 2, l->top + l->height - 1, width - 4, RAIL QUIET NORMAL,
             "↑↓ Select  Enter Open");
}
static int word_count(const char *s)
{
    int n = 0;
    bool inside = false;
    for (; *s; s++) {
        bool nonspace = !isspace((unsigned char)*s);
        if (nonspace && !inside) n++;
        inside = nonspace;
    }
    return n;
}
static void detail(App *a, const Layout *l)
{
    if (!l->detail || l->height < 4) return;
    int x = l->detail_x + 3, y = l->top, w = l->detail - 5;
    for (int row = 0; row < l->height; ++row) {
        paint(a, l->detail_x, y + row, l->detail, RAIL);
        cell(a, l->detail_x, y + row, 1, RAIL LINE NORMAL, "│");
    }
    cell(a, x, y, w, RAIL TXT BOLD, "Context");
    if (l->height < 12) return;
    char stat[80];
    cell(a, x, y + 2, w, RAIL QUIET NORMAL, a->roleplay ? "Story history" : "Draft");
    if (a->roleplay) {
        int messages = cJSON_GetArraySize(at(a->story, "nodes"));
        snprintf(stat, sizeof stat, "#%d / %d stored", number(a->story, "head"), messages);
    }
    else snprintf(stat, sizeof stat, "%d words  ·  rev %d", word_count(a->editing ?
                  str(&a->edit) : scene_text(&a->ws, a->scene)), number(a->scene, "version"));
    cell(a, x, y + 3, w, RAIL TXT, stat);
    if (a->roleplay && *js(a->story, "scene")) {
        const char *path = js(a->story, "scene");
        cell(a, x, y + 5, w, RAIL QUIET, "Linked scene");
        clipped_cell(a, x, y + 6, w, RAIL TXT, strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    } else if (a->roleplay) {
        Text room = {0};
        const char *name = world_room(a->story, number(a->story, "head"), &room);
        if (*name) {
            cell(a, x, y + 5, w, RAIL QUIET, "Room");
            clipped_cell(a, x, y + 6, w, RAIL TXT, name);
        }
        release(&room);
    } else if (a->selected) {
        snprintf(stat, sizeof stat, "%zu bytes selected", a->selection_b - a->selection_a);
        cell(a, x, y + 5, w, RAIL MINT, stat);
    }
    cell(a, x, y + 8, w, RAIL QUIET, "Model");
    clipped_cell(a, x, y + 9, w, RAIL TXT, ui_model(a));
    if (l->height > 16) {
        int n = cJSON_GetArraySize(at(a->ws.root, "bible"));
        rule(a, x, y + 11, w, RAIL LINE NORMAL);
        snprintf(stat, sizeof stat, "F10 Story Bible %d", n);
        clipped_cell(a, x, y + 12, w, RAIL TXT, a->roleplay ? "/memory Compendium" : stat);
        cell(a, x, y + 13, w, RAIL TXT, a->roleplay ? "/trace   Prompt" : "F6   Context");
        cell(a, x, y + 14, w, RAIL TXT, a->roleplay ? "/world   Room" : "/revisions History");
    }
}
static void reading_context(App *a, const Layout *l, int x, int width)
{
    Text crumb = {0};
    if (a->roleplay) {
        const char *character = js(at(a->story, "card"), "name");
        Text room = {0};
        const char *place = world_room(a->story, number(a->story, "head"), &room);
        if (*character) append(&crumb, character);
        if (*place) format(&crumb, "%s%s", crumb.len ? " · " : "", place);
        if (!crumb.len && a->story) append(&crumb, "Story transcript");
        release(&room);
    } else if (a->scene) {
        const char *path = js(a->scene, "path");
        const char *end = strrchr(path, '/');
        for (const char *s = path + 1; end && s < end; s++) {
            if (*s == '/') append(&crumb, "  /  ");
            else append_n(&crumb, s, 1);
        }
    }
    if (a->editing) append(&crumb, crumb.len ? " · Editing" : "Editing");
    if (crumb.len) clipped_cell(a, x, l->top + 1, width, ROOT QUIET NORMAL, str(&crumb));
    release(&crumb);
}

static int draw_lines(App *a, const char *s, Rows rows, int first, int x, int y,
                      int width, int height, size_t mark_a, size_t mark_b)
{
    int last = minimum(rows.count, first + height);
    for (int i = first; i < last; i++) {
        Row row = rows.items[i];
        position(a, x, y + i - first);
        styled(a, ROOT TXT NORMAL);
        if (mark_a < row.end && mark_b > row.start) {
            size_t start = mark_a > row.start ? mark_a : row.start;
            size_t end = mark_b < row.end ? mark_b : row.end;
            int used = display(a, s + row.start, start - row.start, width);
            styled(a, PICK MINT);
            used += display(a, s + start, end - start, width - used);
            styled(a, ROOT TXT);
            display(a, s + end, row.end - end, width - used);
        } else display(a, s + row.start, row.end - row.start, width);
    }
    return last;
}
static void scroll_mark(App *a, const Layout *l, int line, int count, int height, int start_y)
{
    if (height <= 1 || count <= height) return;
    int x = l->main_x + l->main - 2;
    cell(a, x, start_y, 1, ROOT LINE, "╷");
    cell(a, x, start_y + height - 1, 1, ROOT LINE, "╵");
    int knob = (int)((long long)line * (height - 1) / (count - height));
    cell(a, x, start_y + knob, 1, ROOT MINT, "│");
}
static void editor_cursor(App *a, int x, int y)
{
    if (y < 0 || y >= a->rows) return;
    position(a, minimum(x, a->cols - 1), y);
    append(&a->screen, "\x1b[?25h");
}
static void manuscript(App *a, const Layout *l, int x, int y, int width, int height,
                       int *cursor_x, int *cursor_y)
{
    const char *s = a->editing ? str(&a->edit) : scene_text(&a->ws, a->scene);
    if (!*s && !a->editing) {
        if (height < 3) {
            cell(a, x, y, width, ROOT MINT, "F4 edit  ·  /draft generate");
            a->scroll = a->scroll_max = 0;
            return;
        }
        if (a->scene) {
            cell(a, x, y, width, ROOT DIM NORMAL, "This scene is empty.");
            cell(a, x, y + 2, width, ROOT MINT, "F4  Start writing     /draft  Generate a first pass");
        } else {
            cell(a, x, y, width, ROOT MINT BOLD, "Write a scene or play it out.");
            cell(a, x, y + 2, width, ROOT TXT NORMAL, "F4          Write in the scratchpad");
            if (height >= 4) cell(a, x, y + 3, width, ROOT TXT, "/scene      Create a manuscript scene");
            if (height >= 5) cell(a, x, y + 4, width, ROOT TXT, "/import     Bring in a project, session or character");
        }
        a->scroll = a->scroll_max = 0;
        return;
    }
    Rows rows = wrap(s, strlen(s), width);
    a->scroll_max = maximum(0, rows.count - height);
    int line = clamp(a->scroll, 0, a->scroll_max);
    size_t start = 0, end = 0;
    if (a->editing) {
        int caret_row = row_of(&rows, a->caret);
        if (caret_row < line) line = caret_row;
        if (caret_row >= line + height) line = caret_row - height + 1;
        Row cursor = rows.items[caret_row];
        *cursor_x = x + minimum(width, display_width(s + cursor.start, a->caret - cursor.start));
        *cursor_y = y + caret_row - line;
        if (a->mark != SIZE_MAX) {
            start = a->mark < a->caret ? a->mark : a->caret;
            end = a->mark > a->caret ? a->mark : a->caret;
        }
    } else if (a->selected) { start = a->selection_a; end = a->selection_b; }
    a->scroll = line;
    draw_lines(a, s, rows, line, x, y, width, height, start, end);
    scroll_mark(a, l, line, rows.count, height, y);
    free(rows.items);
}

static void roleplay_rows(App *a, Text *body)
{
    Json *path = story_path(a->story, number(a->story, "head"));
    Json *node;
    const char *character = js(at(a->story, "card"), "name");
    const char *persona = js(at(a->story, "persona"), "name");
    cJSON_ArrayForEach(node, path) {
        bool player = !strcmp(js(node, "role"), "user");
        const char *name = player ? (*persona ? persona : "You") : (*character ? character : "Narrator");
        if (!player && *js(node, "speaker")) name = js(node, "speaker");
        const char *addressed = player ? js(node, "addressed") : "";
        if (body->len) append(body, "\n\n");
        format(body, "%s  %s%s%s  ·  #%d\n", player ? "›" : "◆", name,
               *addressed ? " → " : "", addressed, number(node, "id"));
        append(body, js(node, "content"));
        while (body->len && (body->data[body->len - 1] == '\n' || body->data[body->len - 1] == '\r'))
            body->data[--body->len] = 0;
    }
    Preview *p = &a->preview;
    if (preview_busy(p) && p->story && p->story == a->story) {
        if (body->len) append(body, "\n\n");
        const char *state = p->auto_research_job ? (p->planning_research ? "checking sources" : "researching") :
                            p->recalling ? "recalling memory" : p->definition_research ? "selecting a definition" : "responding";
        format(body, "◆  %s  ·  %s\n", p->speaker.len ? str(&p->speaker) : *character ? character : "Narrator",
               state);
        const char *draft = str(&p->candidates[0].body);
        while (isspace((unsigned char)*draft)) draft++;
        if (!p->definition_research && (!simulation_enabled(p->story) ||
            (*draft != '{' && strncmp(draft, "[{", 2) && strncmp(draft, "```json", 7))))
            append(body, str(&p->candidates[0].body));
    }
    cJSON_Delete(path);
    if (!body->len) {
        append(body, a->story ? "No turns yet.\n\nWrite your first action below, or use /narrate to open the story."
                            : "Start with /story and a title, or /import a character card.\n\nUse /play to set a roleplay inside the current manuscript scene.");
    }
}
static void roleplay(App *a, const Layout *l, int x, int y, int width, int height)
{
    Text body = {0};
    roleplay_rows(a, &body);
    Rows rows = wrap(str(&body), body.len, width);
    bool tail = a->scroll == INT_MAX;
    a->scroll_max = maximum(0, rows.count - height);
    int line = clamp(a->scroll, 0, a->scroll_max);
    if (!tail) a->scroll = line;
    for (int i = line; i < minimum(rows.count, line + height); i++) {
        Row r = rows.items[i];
        const char *s = body.data + r.start;
        const char *style = !strncmp(s, "◆  ", 5) ? ROOT AMBER BOLD :
                            !strncmp(s, "›  ", 5) ? ROOT MINT BOLD : ROOT TXT NORMAL;
        position(a, x, y + i - line);
        styled(a, style);
        display(a, s, r.end - r.start, width);
    }
    scroll_mark(a, l, line, rows.count, height, y);
    free(rows.items);
    release(&body);
}

static void panel(App *a, const Layout *l, int x, int y, int width, int height)
{
    Preview *p = &a->preview;
    Text text = {0};
    if (a->panel_preview) {
        Candidate *c = &p->candidates[p->current];
        char label[140];
        int ready = 0;
        for (int i = 0; i < p->count; i++)
            if (p->candidates[i].finished && p->candidates[i].body.len &&
                !p->candidates[i].error.len) ready++;
        snprintf(label, sizeof label, "%s  ·  %d of %d ready",
                 p->reasoning ? "Reasoning" : "Candidate review", ready, p->count);
        cell(a, x, l->top, width, ROOT TXT BOLD, label);
        const char *state = c->job ? "Streaming" : c->error.len ? "Interrupted" :
                            !c->finished ? "Queued" : c->body.len ? "Ready" : "Empty";
        snprintf(label, sizeof label, "%d/%d  %s  ·  %s", p->current + 1, p->count, c->label, state);
        if (!l->dense) cell(a, x, l->top + 1, width, c->job ? ROOT GOLD BOLD : ROOT DIM NORMAL, label);
        if (p->reasoning) append(&text, c->thinking.len ? str(&c->thinking) : "The provider has supplied no reasoning.");
        else if (c->body.len) append(&text, str(&c->body));
        else append(&text, c->job ? "Waiting for the model…" :
                    !c->finished ? "Waiting for a worker…" : "No text received.");
        if (c->error.len) format(&text, "\n\n%s", str(&c->error));
    } else {
        clipped_cell(a, x, l->top, width, ROOT TXT BOLD, a->info_title);
        append(&text, str(&a->info));
    }
    rule(a, x, y - 1, width, ROOT LINE NORMAL);
    Rows rows = wrap(str(&text), text.len, width);
    a->panel_scroll = clamp(a->panel_scroll, 0, maximum(0, rows.count - height));
    draw_lines(a, str(&text), rows, a->panel_scroll, x, y, width, height, 0, 0);
    scroll_mark(a, l, a->panel_scroll, rows.count, height, y);
    free(rows.items);
    release(&text);
}

static int compare_sample(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}
static double median(double samples[], int count)
{
    qsort(samples, (size_t)count, sizeof *samples, compare_sample);
    return (samples[(count - 1) / 2] + samples[count / 2]) / 2;
}
static void metric(App *a, int x, int y, int width, const char *label, const char *value, const char *detail)
{
    for (int row = 0; row < 3; ++row) paint(a, x, y + row, width, FIELD);
    clipped_cell(a, x + 2, y, width - 4, FIELD DIM NORMAL, label);
    clipped_cell(a, x + 2, y + 1, width - 4, FIELD MINT BOLD, value);
    clipped_cell(a, x + 2, y + 2, width - 4, FIELD QUIET NORMAL, detail);
}

static void neofetch(App *a, const Layout *l, int x, int y, int width, int height)
{
    const Neofetch *neo = &a->neo;
    const Json *server = neo->server;
    const char *gateway = server ? "Online" : neo->error.len ? "Unavailable" : "Checking…";
    const char *gateway_style = server ? ROOT MINT BOLD : neo->error.len ? ROOT GOLD BOLD : ROOT DIM BOLD;
    cell(a, x, l->top, width, ROOT TXT BOLD, "Neofetch");
    if (width >= 28) right(a, x + width, l->top, 13, gateway_style, gateway);
    rule(a, x, y - 1, width, ROOT LINE NORMAL);
    double rates[NEOFETCH_SAMPLES], ttfts[NEOFETCH_SAMPLES];
    int rate_count = 0, ttft_count = 0;
    for (int i = 0; i < neo->count; i++) {
        InferenceMetrics sample = neo->samples[i];
        if (sample.tokens_per_second > 0) rates[rate_count++] = sample.tokens_per_second;
        if (sample.has_ttft) ttfts[ttft_count++] = (double)sample.ttft_ms;
    }
    bool tiles = width >= 56 && height >= 13;
    if (tiles) {
        int half = (width - 2) / 2;
        char rate[48] = "—", ttft[48] = "—", rate_detail[32], ttft_detail[32];
        if (rate_count) snprintf(rate, sizeof rate, "%.1f tok/s", median(rates, rate_count));
        if (ttft_count) snprintf(ttft, sizeof ttft, "%.0f ms", median(ttfts, ttft_count));
        snprintf(rate_detail, sizeof rate_detail, "%d measured", rate_count);
        snprintf(ttft_detail, sizeof ttft_detail, "%d measured", ttft_count);
        metric(a, x, y, half, "Median throughput", rate, rate_detail);
        metric(a, x + half + 2, y, width - half - 2, "Median time to first token", ttft, ttft_detail);
        y += 4;
        height -= 4;
    }
    const char *const mark[] = {
        "       .----.       ",
        "    .-'      '-.    ",
        " .-+------------+-. ",
        "(  |      +     |  )",
        " '-+------------+-' ",
        "    '-.______.-'    ",
    };
    if (width >= 65 && height >= 8) {
        const int art_rows = (int)(sizeof mark / sizeof *mark);
        for (int row = 0; row < minimum(art_rows, height); row++)
            cell(a, x, y + row, 20, row == 3 ? ROOT AMBER BOLD : ROOT MINT NORMAL, mark[row]);
        x += 24;
        width -= 24;
    }
    Text body = {0};
    format(&body, "Gateway  %s%s%s\n", gateway, server ? " · " : "", js(server, "version"));
    if (server) {
        const char *provider = js(server, "provider");
        if (!strcmp(provider, "ok") || !strcmp(provider, "failed"))
            format(&body, "Provider  Last request %s · %ds ago\n",
                   !strcmp(provider, "ok") ? "succeeded" : "failed", number(server, "provider_age_s"));
        else append(&body, "Provider  No requests observed\n");
        format(&body, "Model  %s\n", *js(server, "model") ? js(server, "model") : ui_model(a));
        format(&body, "Load  %d active · %d queued\n", number(server, "active"), number(server, "queued"));
        if (cJSON_IsTrue(at(server, "pinned_slots"))) append(&body, "Routing  Pinned llama.cpp slots\n");
        else format(&body, "Workers  %d on the default provider\n", number(server, "workers"));
    } else if (neo->error.len) format(&body, "%s\n", str(&neo->error));
    if (!tiles) {
        append(&body, "\nSession performance\n");
        if (rate_count)
            format(&body, "Median tok/s  %.1f · %d measured\n", median(rates, rate_count), rate_count);
        else format(&body, "Median tok/s  %s\n", neo->count ? "Unavailable" : "Waiting for a generation");
        if (ttft_count) format(&body, "Median TTFT  %.0f ms · %d measured\n", median(ttfts, ttft_count), ttft_count);
        else append(&body, "Median TTFT  Unavailable\n");
    }
    format(&body, "Sample window  %d/%d completed\n\n", neo->count, NEOFETCH_SAMPLES);
    format(&body, "Terminal  %d×%d · %s\nClient  %s · %s\n", a->cols, a->rows,
           a->roleplay ? "Roleplay" : "Manuscript", NEUTRINO_VERSION, a->ssh_session ? "SSH" : "Local");
    if (neo->count) {
        append(&body, "\nRecent generations (oldest first)\n");
        int recent = minimum(neo->count, 5);
        for (int i = 0; i < recent; i++) {
            InferenceMetrics sample = neo->samples[(neo->next + NEOFETCH_SAMPLES - recent + i) % NEOFETCH_SAMPLES];
            format(&body, "%d  ", i + 1);
            if (sample.tokens_per_second > 0) format(&body, "%.1f tok/s", sample.tokens_per_second);
            else append(&body, "— tok/s");
            if (sample.has_ttft) format(&body, "  ·  %llu ms TTFT", (unsigned long long)sample.ttft_ms);
            append(&body, "\n");
        }
    }
    Rows rows = wrap(str(&body), body.len, width);
    a->panel_scroll = clamp(a->panel_scroll, 0, maximum(0, rows.count - height));
    draw_lines(a, str(&body), rows, a->panel_scroll, x, y, width, height, 0, 0);
    scroll_mark(a, l, a->panel_scroll, rows.count, height, y);
    free(rows.items);
    release(&body);
}

static void composer(App *a, const Layout *l, const Rows *wrapped, int *cx, int *cy)
{
    int x = content_x(l) - 1, y = l->input_y, w = content_width(l) + 2;
    const char *accent = a->roleplay ? FIELD AMBER BOLD : FIELD MINT BOLD;
    const char *style = a->focus == COMPOSER || a->editing ? accent : FIELD QUIET NORMAL;
    if (!l->dense) {
        cell(a, x + 1, y - 1, w - 2, ROOT DIM NORMAL,
             a->editing ? "Editing manuscript" : a->roleplay ? "Next action" : "Writing direction");
        if (w >= 37 && !a->editing)
            right(a, x + w - 1, y - 1, 22, ROOT QUIET NORMAL,
                  a->roleplay ? "Enter Send" : "F4 Edit   Enter Send");
    }
    paint(a, x, y, w, FIELD);
    cell(a, x, y, 1, style, "▏");
    cell(a, x + 1, y, 2, style, a->editing ? "✎" : "›");
    int width = w - 5;
    if (a->editing) {
        clipped_cell(a, x + 3, y, width, FIELD TXT NORMAL,
                     "Ctrl+S Save   Ctrl+Space Select   Esc Close");
        return;
    }
    const char *s = str(&a->prompt);
    if (wrapped->count > 1) {
        int line = row_of(wrapped, a->cursor);
        int lines = minimum(3, wrapped->count);
        int first = clamp(line - lines + 1, 0, wrapped->count - lines);
        for (int i = 0; i < lines; ++i) {
            if (i) paint(a, x, y + i, w, FIELD);
            if (i) cell(a, x, y + i, 1, style, "▏");
            Row row = wrapped->items[first + i];
            position(a, x + 3, y + i);
            styled(a, FIELD TXT NORMAL);
            display(a, s + row.start, row.end - row.start, width);
        }
        if (a->focus == COMPOSER) {
            size_t offset = wrapped->items[line].start;
            *cx = x + 3 + minimum(width, display_width(s + offset, a->cursor - offset));
            *cy = y + line - first;
        }
        return;
    }
    size_t start = 0;
    int used = 0;
    for (size_t i = a->cursor; i;) {
        size_t prev = utf_prev(s, i);
        int glyph = utf_width(s + prev, i - prev);
        if (used + glyph >= width - 1) { start = i; break; }
        used += glyph;
        i = prev;
    }
    const char *placeholder = width < 31 ? "Write or type /" :
                              a->roleplay ? "What do you do?" : "What should happen next?";
    cell(a, x + 3, y, width, a->prompt.len ? FIELD TXT NORMAL : FIELD QUIET NORMAL,
         a->prompt.len ? s + start : placeholder);
    if (a->focus == COMPOSER) {
        *cx = x + 3 + used;
        *cy = y;
    }
}
static void palette(App *a, const Layout *l)
{
    if (!a->slash || a->focus != COMPOSER) return;
    int matches[COMMAND_MATCH_LIMIT], count = ui_command_matches(a, matches, COMMAND_MATCH_LIMIT);
    int bottom = l->input_y - 1;
    int shown = minimum(count ? count : 1, minimum(8, maximum(1, bottom - l->top - 2)));
    int first = clamp(a->command_cursor - shown + 1, 0, maximum(0, count - shown));
    int x = content_x(l) - 1, y = bottom - 1 - shown, w = content_width(l) + 2;
    const char *border = a->roleplay ? RAIL AMBER NORMAL : RAIL MINT NORMAL;
    card(a, x, y, w, shown + 2, border);
    cell(a, x + 2, y, w - 4, border, " Commands ");
    if (w >= 52) right(a, x + w - 2, y, 24, border, " ↑↓ select  Esc close ");
    for (int i = 0; i < shown; i++) {
        int n = first + i;
        bool focused = n == a->command_cursor && count;
        const char *bg = focused ? (a->roleplay ? PLUM : PICK) : RAIL;
        paint(a, x + 1, y + i + 1, w - 2, bg);
        if (count) {
            const Command *cmd = &commands[matches[n]];
            Text name = {0};
            format(&name, "/%s", cmd->name);
            cell(a, x + 2, y + i + 1, minimum(18, w - 3),
                 focused ? (a->roleplay ? PLUM AMBER BOLD : PICK MINT BOLD) : RAIL TXT NORMAL, str(&name));
            if (w > 34) cell(a, x + 20, y + i + 1, w - 22,
                focused ? (a->roleplay ? PLUM TXT NORMAL : PICK TXT NORMAL) : RAIL DIM NORMAL, cmd->summary);
            release(&name);
        } else cell(a, x + 2, y + i + 1, w - 4, RAIL DIM, "No matching commands");
    }
}

static void frame(App *a)
{
    clear(&a->screen);
    styled(a, "\x1b[?25l" ROOT NORMAL "\x1b[2J\x1b[H");
    Layout l = layout(a);
    if (a->cols < 26 || a->rows < 10) {
        cell(a, 0, 0, a->cols, ROOT MINT BOLD, "Neutrino · enlarge terminal");
        return;
    }
    Rows prompt = {0};
    if (!l.dense && !l.library_only && !a->editing && a->prompt.len) {
        prompt = wrap(str(&a->prompt), a->prompt.len, content_width(&l) - 3);
        l.input_y -= minimum(3, prompt.count) - 1;
        l.content_end = l.input_y - 2;
    }
    header(a, &l);
    library(a, &l);
    detail(a, &l);
    int x = content_x(&l), width = content_width(&l);
    int y = l.top + (l.dense ? 2 : 3);
    int height = maximum(1, l.content_end - y);
    int cx = -1, cy = -1;
    if (l.library_only) {
        /* The library replaces the reading pane on narrow or short terminals. */
    } else if (a->view_panel && a->panel_neofetch) {
        neofetch(a, &l, x, y, width, height);
    } else if (a->view_panel) panel(a, &l, x, y, width, height);
    else {
        clipped_cell(a, x, l.top, width, ROOT TXT BOLD,
                     a->roleplay ? ui_story_name(a) : ui_scene_name(a));
        if (!l.dense) reading_context(a, &l, x, width);
        rule(a, x, y - 1, width, ROOT LINE NORMAL);
        if (a->roleplay) roleplay(a, &l, x, y, width, height);
        else manuscript(a, &l, x, y, width, height, &cx, &cy);
    }
    if (!l.library_only) composer(a, &l, &prompt, &cx, &cy);
    free(prompt.items);
    const char *state = a->status.len ? str(&a->status) : "Ready";
    const char *keys = a->focus == LIBRARY ? "Esc Close" : a->editing ? "Esc Close" :
                       a->view_panel ? (l.width >= 72 && a->panel_preview ? "←→ Candidates   F8 Apply   F9 Reject   Esc Close" :
                                        l.width >= 54 ? "PgUp/PgDn Scroll   Esc Close" : "Esc Close") :
                       l.width >= 90 ? "F2 Mode   F3 Library   Ctrl+P Commands" :
                       l.width >= 53 ? "F3 Library   Ctrl+P Commands" : "F1 Help";
    int reserve = display_width(keys, strlen(keys)) + 3;
    paint(a, l.left, a->rows - 1, l.width, RAIL);
    clipped_cell(a, l.left + 2, a->rows - 1, l.width - reserve - 3,
                 preview_busy(&a->preview) || a->research_job ? RAIL GOLD NORMAL : RAIL DIM NORMAL, state);
    right(a, l.left + l.width - 2, a->rows - 1, reserve, RAIL QUIET NORMAL, keys);
    palette(a, &l);
    styled(a, ROOT TXT NORMAL);
    if (cx >= 0 && cy >= 0) editor_cursor(a, cx, cy);
}
void ui_draw(App *a)
{
    frame(a);
    terminal_write(str(&a->screen), a->screen.len);
}
void ui_dump(App *a, int cols, int rows)
{
    bound_terminal_viewport(&cols, &rows);
    a->cols = cols;
    a->rows = rows;
    frame(a);
    fwrite(str(&a->screen), 1, a->screen.len, stdout);
    fputs("\x1b[0m\x1b[?25h\n", stdout);
}
