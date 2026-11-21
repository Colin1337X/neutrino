#include "app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WORLD_ROOM_LIMIT = 128 };

static bool equal_name(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return !*a && !*b;
}

static Json *named(const Json *items, const char *name)
{
    if (!*name) return NULL;
    Json *item;
    cJSON_ArrayForEach(item, items)
        if (equal_name(js(item, "name"), name)) return item;
    return NULL;
}

static Json *array(Json *object, const char *key)
{
    Json *value = at(object, key);
    if (!cJSON_IsArray(value)) put(object, key, value = cJSON_CreateArray());
    return value;
}

static Json *world_object(Json *story)
{
    Json *world = at(story, "world");
    if (!cJSON_IsObject(world)) put(story, "world", world = cJSON_CreateObject());
    return world;
}

static const char *default_room(const Json *story)
{
    return js(cJSON_GetArrayItem(at(at(story, "world"), "rooms"), 0), "name");
}

static bool *path_ids(const Json *story, int head, int *count)
{
    const Json *nodes = at(story, "nodes");
    *count = cJSON_GetArraySize(nodes);
    bool *visible = calloc((size_t)*count + 1, sizeof(*visible));
    int *parents = calloc((size_t)*count + 1, sizeof(*parents));
    if (!visible || !parents) abort();
    int index = 1;
    Json *node;
    cJSON_ArrayForEach(node, nodes) {
        int parent = number(node, "parent");
        parents[index] = parent >= 0 && parent < index ? parent : 0;
        index++;
    }
    visible[0] = true;
    for (int id = head; id >= 1 && id <= *count; id = parents[id]) visible[id] = true;
    free(parents);
    return visible;
}

static void replay(Json *state, const Json *event)
{
    const char *type = js(event, "type"), *name = js(event, "name");
    if (!strcmp(type, "move")) set(state, "room", name);
    else if (!strcmp(type, "weather")) set(state, "weather", js(event, "text"));
    else if (!strcmp(type, "item") || !strcmp(type, "npc")) {
        Json *bucket = at(state, type);
        if (*name && !named(bucket, name)) cJSON_AddItemToArray(bucket, cJSON_Duplicate(event, true));
    } else if (!strcmp(type, "take") || !strcmp(type, "drop")) {
        Json *item = named(at(state, "item"), name);
        if (item) {
            put(item, "carried", cJSON_CreateBool(!strcmp(type, "take")));
            if (!strcmp(type, "drop")) set(item, "room", js(event, "room"));
        }
    }
}

static Json *world_state(const Json *story, int head)
{
    Json *state = cJSON_CreateObject();
    put(state, "item", cJSON_CreateArray());
    put(state, "npc", cJSON_CreateArray());
    put(state, "events", cJSON_CreateArray());
    set(state, "room", default_room(story));
    Json *events = at(at(story, "world"), "events");
    if (events && events->child) {
        int count = 0;
        bool *visible = path_ids(story, head, &count);
        Json *event;
        cJSON_ArrayForEach(event, events) {
            int anchor = number(event, "at");
            if (anchor < 0 || anchor > count || !visible[anchor]) continue;
            replay(state, event);
            cJSON_AddItemReferenceToArray(at(state, "events"), event);
        }
        free(visible);
    }
    return state;
}

static bool link_matches(const Json *link, const char *a, const char *b)
{
    const char *from = js(link, "from"), *to = js(link, "to");
    return (equal_name(from, a) && equal_name(to, b)) || (equal_name(from, b) && equal_name(to, a));
}

static bool linked(const Json *world, const char *a, const char *b)
{
    Json *link;
    cJSON_ArrayForEach(link, at(world, "links")) if (link_matches(link, a, b)) return true;
    return false;
}

static bool valid_name(const char *text)
{
    if (!*text || strlen(text) > 70) return false;
    for (; *text; text++)
        if ((unsigned char)*text < 32 || (unsigned char)*text == 127 || *text == '|' || *text == '@') return false;
    return true;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = 0;
    return text;
}

static void describe_world(const Json *story, const Json *state, Text *out)
{
    const char *room = js(state, "room");
    if (!*room) return;
    Json *world = at(story, "world"), *r = named(at(world, "rooms"), room);
    format(out, "Room: %s\n%s\n", room, js(r, "text"));
    if (*js(state, "weather")) format(out, "Weather: %s\n", js(state, "weather"));
    Json *link, *item;
    int count = 0;
    cJSON_ArrayForEach(link, at(world, "links")) {
        const char *from = js(link, "from"), *to = js(link, "to");
        if (equal_name(from, room) || equal_name(to, room))
            format(out, "%s%s", count++ ? ", " : "Exits: ", equal_name(from, room) ? to : from);
    }
    if (count) append(out, "\n");
    cJSON_ArrayForEach(item, at(state, "item")) {
        bool carried = cJSON_IsTrue(at(item, "carried"));
        if (carried || equal_name(js(item, "room"), room))
            format(out, "%s: %s — %s\n", carried ? "Carried" : "Nearby", js(item, "name"), js(item, "text"));
    }
    cJSON_ArrayForEach(item, at(state, "npc"))
        if (equal_name(js(item, "room"), room))
            format(out, "Present character: %s — %s\n", js(item, "name"), js(item, "text"));
}

void world_context(const Json *story, int head, Text *out)
{
    if (!*default_room(story)) return;
    Json *state = world_state(story, head);
    describe_world(story, state, out);
    const Json *events = at(state, "events");
    int skipped = cJSON_GetArraySize(events) - 10;
    Json *event;
    cJSON_ArrayForEach(event, events) {
        if (skipped-- > 0 || strcmp(js(event, "type"), "say") ||
            !equal_name(js(event, "room"), js(state, "room"))) continue;
        format(out, "Player said here: %s\n", js(event, "text"));
    }
    cJSON_Delete(state);
}

const char *world_room(const Json *story, int head, Text *out)
{
    const char *room = default_room(story);
    Json *events = at(at(story, "world"), "events");
    if (*room && events && events->child) {
        int count = 0;
        bool *visible = path_ids(story, head, &count);
        Json *event = events->child->prev;
        while (event) {
            int anchor = number(event, "at");
            if (anchor >= 0 && anchor <= count && visible[anchor] && !strcmp(js(event, "type"), "move")) {
                room = js(event, "name");
                break;
            }
            event = event == events->child ? NULL : event->prev;
        }
        free(visible);
    }
    append(out, room);
    return str(out);
}

bool world_address(const Json *story, const char *line, Text *speaker, const char **message)
{
    *message = line;
    if (*line != '@' || !*default_room(story)) return true;
    Json *state = world_state(story, number(story, "head")), *npc, *chosen = NULL;
    size_t length = 0;
    const char *requested = line + 1;
    cJSON_ArrayForEach(npc, at(state, "npc")) {
        if (!equal_name(js(npc, "room"), js(state, "room"))) continue;
        const char *name = js(npc, "name");
        size_t n = strlen(name), i = 0;
        if (n <= length) continue;
        while (i < n && requested[i] &&
               tolower((unsigned char)name[i]) == tolower((unsigned char)requested[i])) i++;
        if (i == n && (!requested[i] || isspace((unsigned char)requested[i]) || requested[i] == ':')) {
            chosen = npc;
            length = n;
        }
    }
    if (chosen) {
        const char *content = requested + length;
        while (isspace((unsigned char)*content) || *content == ':') content++;
        if (*content) {
            append(speaker, js(chosen, "name"));
            *message = content;
        }
    }
    cJSON_Delete(state);
    return speaker->len > 0;
}

static void world_panel(App *a, bool log)
{
    app_info(a, log ? "World activity" : "World & inventory");
    Json *state = world_state(a->story, number(a->story, "head"));
    if (log) {
        Json *events = at(state, "events"), *event;
        int skip = cJSON_GetArraySize(events) - 60;
        cJSON_ArrayForEach(event, events) {
            if (skip-- > 0) continue;
            format(&a->info, "#%d  %s", number(event, "at"), js(event, "type"));
            if (*js(event, "name")) format(&a->info, "  %s", js(event, "name"));
            if (*js(event, "text")) format(&a->info, "  %s", js(event, "text"));
            append(&a->info, "\n");
        }
        if (!a->info.len) append(&a->info, "No world actions on this branch.");
    } else {
        describe_world(a->story, state, &a->info);
        if (!a->info.len) append(&a->info, "Create the starting room with /room NAME | DESCRIPTION.\n");
        append(&a->info, "\n/room  /connect  /go  /item  /take  /drop  /npc  /weather  /say\n"
                         "@Name MESSAGE addresses a character in this room.\n/world log shows branch activity. /map shows room links.");
    }
    cJSON_Delete(state);
}

static int map_index(const char *const rooms[], int count, const char *name)
{
    for (int i = 0; i < count; i++)
        if (equal_name(rooms[i], name)) return i;
    return -1;
}

static void world_map(App *a, const char *destination)
{
    const Json *world = at(a->story, "world");
    const char *rooms[WORLD_ROOM_LIMIT];
    int count = 0;
    Json *item;
    cJSON_ArrayForEach(item, at(world, "rooms")) {
        if (count == WORLD_ROOM_LIMIT) break;
        if (*js(item, "name")) rooms[count++] = js(item, "name");
    }
    if (!count) { app_status(a, "Create a starting room with /room first."); return; }
    Text current = {0};
    world_room(a->story, number(a->story, "head"), &current);
    int start = map_index(rooms, count, str(&current));
    release(&current);
    if (start < 0) { app_status(a, "The current room is missing from the map."); return; }
    int end = *destination ? map_index(rooms, count, destination) : -1;
    if (*destination && end < 0) { app_status(a, "Unknown destination. Use /map to list the rooms."); return; }

    bool links[WORLD_ROOM_LIMIT][WORLD_ROOM_LIMIT] = {0};
    cJSON_ArrayForEach(item, at(world, "links")) {
        int from = map_index(rooms, count, js(item, "from"));
        int to = map_index(rooms, count, js(item, "to"));
        if (from >= 0 && to >= 0 && from != to) links[from][to] = links[to][from] = true;
    }
    int parents[WORLD_ROOM_LIMIT], distance[WORLD_ROOM_LIMIT], queue[WORLD_ROOM_LIMIT] = {start};
    for (int i = 0; i < count; i++) distance[i] = -1;
    parents[start] = -1;
    distance[start] = 0;
    for (int head = 0, tail = 1; head < tail; head++) {
        int from = queue[head];
        for (int to = 0; to < count; to++) {
            if (!links[from][to] || distance[to] >= 0) continue;
            parents[to] = from;
            distance[to] = distance[from] + 1;
            queue[tail++] = to;
        }
    }

    app_info(a, "World map");
    format(&a->info, "You are here: %s\n\n", rooms[start]);
    if (end >= 0) {
        if (distance[end] < 0) format(&a->info, "No route to %s.\n", rooms[end]);
        else if (end == start) format(&a->info, "You are already at %s.\n", rooms[end]);
        else {
            format(&a->info, "Shortest route to %s (%d %s)\n", rooms[end], distance[end],
                   distance[end] == 1 ? "step" : "steps");
            int route[WORLD_ROOM_LIMIT], steps = 0;
            for (int at = end; at >= 0; at = parents[at]) route[steps++] = at;
            for (int i = steps - 1; i >= 0; i--)
                format(&a->info, "%s%s", i == steps - 1 ? "" : " → ", rooms[route[i]]);
            format(&a->info, "\n\nNext move: /go %s\n", rooms[route[steps - 2]]);
        }
        return;
    }
    for (int i = 0; i < count; i++) {
        format(&a->info, "%s%s", i == start ? "● " : "○ ", rooms[i]);
        if (distance[i] < 0) append(&a->info, " (unreachable)");
        int exits = 0;
        for (int to = 0; to < count; to++)
            if (links[i][to]) format(&a->info, "%s%s", exits++ ? ", " : " → ", rooms[to]);
        append(&a->info, "\n");
    }
    append(&a->info, "\n/map ROOM  Plot the shortest route");
}

static void add_event(App *a, Json *world, const char *type, const char *name,
                      const char *text, const char *room)
{
    Json *events = array(world, "events");
    if (cJSON_GetArraySize(events) >= 8192) { app_status(a, "World activity limit reached."); return; }
    Json *event = cJSON_CreateObject();
    integer(event, "at", number(a->story, "head"));
    set(event, "type", type);
    set(event, "name", name);
    set(event, "text", text);
    set(event, "room", room);
    cJSON_AddItemToArray(events, event);
    a->ws.dirty = true;
    app_status(a, "World action saved");
}

void world_command(App *a, const char *verb, char *argument)
{
    if (!a->roleplay || !a->story) { app_status(a, "Open a roleplay story to use its world."); return; }
    if (!strcmp(verb, "world")) { world_panel(a, !strcmp(argument, "log")); return; }
    if (!strcmp(verb, "map")) { world_map(a, trim(argument)); return; }
    if (preview_busy(&a->preview)) { app_status(a, "Finish the active reply before changing the world."); return; }

    bool room_cmd = !strcmp(verb, "room"), link_cmd = !strcmp(verb, "connect");
    bool prop_cmd = !strcmp(verb, "item") || !strcmp(verb, "npc");
    char *name = trim(argument), *text = "";
    if (room_cmd || link_cmd || prop_cmd) {
        char *separator = strchr(name, '|');
        if (separator) {
            *separator++ = 0;
            text = trim(separator);
            name = trim(name);
        }
        if (!valid_name(name) || !*text || strlen(text) > 600) {
            app_status(a, link_cmd ? "Use /connect ROOM | ROOM." : "Use NAME | DESCRIPTION (up to 600 bytes).");
            return;
        }
    }
    Json *world = at(a->story, "world"), *rooms = at(world, "rooms");
    if (room_cmd) {
        if (named(rooms, name)) { app_status(a, "A room with that name already exists."); return; }
        if (cJSON_GetArraySize(rooms) >= WORLD_ROOM_LIMIT) { app_status(a, "Room limit reached."); return; }
        Json *r = cJSON_CreateObject();
        set(r, "name", name);
        set(r, "text", text);
        cJSON_AddItemToArray(array(world_object(a->story), "rooms"), r);
        a->ws.dirty = true;
        app_status(a, "Room created");
        return;
    }
    if (!*default_room(a->story)) { app_status(a, "Create a starting room with /room first."); return; }
    if (link_cmd) {
        Json *from = named(rooms, name), *to = named(rooms, text);
        if (!from || !to || from == to) { app_status(a, "Choose two distinct existing rooms."); return; }
        if (linked(world, name, text)) { app_status(a, "These rooms are already connected."); return; }
        Json *link = cJSON_CreateObject();
        set(link, "from", js(from, "name"));
        set(link, "to", js(to, "name"));
        cJSON_AddItemToArray(array(world, "links"), link);
        a->ws.dirty = true;
        app_status(a, "Rooms connected");
        return;
    }

    Json *state = world_state(a->story, number(a->story, "head"));
    const char *room = js(state, "room");
    if (prop_cmd) {
        if (named(at(state, verb), name)) app_status(a, "That name already exists on this branch.");
        else if (cJSON_GetArraySize(at(state, verb)) >= (!strcmp(verb, "npc") ? 32 : 128))
            app_status(a, "World entity limit reached.");
        else add_event(a, world, verb, name, text, room);
    } else if (!strcmp(verb, "go")) {
        Json *target = named(rooms, name);
        if (!target || !linked(world, room, js(target, "name"))) app_status(a, "Choose an exit shown by /world.");
        else add_event(a, world, "move", js(target, "name"), "", room);
    } else if (!strcmp(verb, "take") || !strcmp(verb, "drop")) {
        Json *object = named(at(state, "item"), name);
        bool carried = cJSON_IsTrue(at(object, "carried")), taking = !strcmp(verb, "take");
        if (!object || (taking ? carried || !equal_name(room, js(object, "room")) : !carried))
            app_status(a, taking ? "That item is unavailable in this room." : "That item is outside your inventory.");
        else add_event(a, world, verb, js(object, "name"), "", room);
    } else if (!strcmp(verb, "say") || !strcmp(verb, "weather")) {
        if (!*name || strlen(name) > 600) app_status(a, "Enter text up to 600 bytes.");
        else add_event(a, world, verb, "", name, room);
    }
    cJSON_Delete(state);
}
