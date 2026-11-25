#include "app.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool link_matches(const Json *link, const char *a, const char *b);

static void replay(Json *state, const Json *event)
{
    const char *type = js(event, "type"), *name = js(event, "name");
    if (!strcmp(type, "move")) set(state, "room", name);
    else if (!strcmp(type, "weather")) set(state, "weather", js(event, "text"));
    else if (!strcmp(type, "room")) {
        Json *rooms = at(state, "rooms"), *room = named(rooms, name);
        if (!room) {
            room = cJSON_CreateObject();
            set(room, "name", name);
            cJSON_AddItemToArray(rooms, room);
        }
        set(room, "text", js(event, "text"));
        if (!*js(state, "room")) set(state, "room", name);
    } else if (!strcmp(type, "roomdelete")) {
        Json *rooms = at(state, "rooms"), *room = named(rooms, name);
        if (room) cJSON_Delete(cJSON_DetachItemViaPointer(rooms, room));
    } else if (!strcmp(type, "link") || !strcmp(type, "unlink")) {
        Json *links = at(state, "links"), *link;
        const char *to = js(event, "room");
        cJSON_ArrayForEach(link, links)
            if (link_matches(link, name, to)) break;
        if (link && !strcmp(type, "unlink"))
            cJSON_Delete(cJSON_DetachItemViaPointer(links, link));
        if (!link && !strcmp(type, "link")) {
            link = cJSON_CreateObject();
            set(link, "from", name);
            set(link, "to", to);
            cJSON_AddItemToArray(links, link);
        }
    } else if (!strcmp(type, "itemdelete") || !strcmp(type, "npcdelete")) {
        Json *items = at(state, *type == 'i' ? "item" : "npc"), *item = named(items, name);
        if (item) cJSON_Delete(cJSON_DetachItemViaPointer(items, item));
    } else if (!strcmp(type, "npcplace") || !strcmp(type, "itemplace")) {
        Json *items = at(state, *type == 'i' ? "item" : "npc"), *item = named(items, name);
        if (!item) {
            item = cJSON_CreateObject();
            set(item, "name", name);
            cJSON_AddItemToArray(items, item);
        }
        if (*js(event, "text")) set(item, "text", js(event, "text"));
        set(item, "room", js(event, "room"));
        if (*type == 'i') {
            put(item,"carried",cJSON_CreateFalse());
            put(item,"worn",cJSON_CreateFalse());
        }
    }
    else if (!strcmp(type, "item") || !strcmp(type, "npc")) {
        Json *bucket = at(state, type);
        if (*name && !named(bucket, name)) cJSON_AddItemToArray(bucket, cJSON_Duplicate(event, true));
    } else if (!strcmp(type, "take") || !strcmp(type, "drop")) {
        Json *item = named(at(state, "item"), name);
        if (item) {
            put(item, "carried", cJSON_CreateBool(!strcmp(type, "take")));
            if (!strcmp(type, "drop")) {
                put(item, "worn", cJSON_CreateFalse());
                set(item, "room", js(event, "room"));
            }
        }
    } else if (!strcmp(type, "wear") || !strcmp(type, "remove")) {
        Json *item = named(at(state, "item"), name);
        if (cJSON_IsTrue(at(item, "carried"))) put(item, "worn", cJSON_CreateBool(!strcmp(type, "wear")));
    } else if (!strcmp(type, "itemedit")) {
        Json *item = named(at(state, "item"), name);
        if (item) set(item, "text", js(event, "text"));
    }
}
static const char *item_state(const Json *item, const char *nearby)
{
    return cJSON_IsTrue(at(item, "worn")) ? "Worn" : cJSON_IsTrue(at(item, "carried")) ? "Carried" : nearby;
}
static Json *world_state(const Json *story, int head);
static void inventory_panel(App *a, const char *filter)
{
    bool held = !strcmp(filter, "held"), worn = !strcmp(filter, "worn"), nearby = !strcmp(filter, "nearby");
    if (*filter && !held && !worn && !nearby) {
        app_status(a, "Use /inventory [held|worn|nearby].");
        return;
    }
    Json *state = world_state(a->story, number(a->story, "head"));
    const char *room = js(state, "room");
    app_info(a, "Inventory");
    format(&a->info, "%s · %s\n\n", *room ? room : "No current room", *filter ? filter : "All available items");
    int shown = 0;
    Json *item;
    cJSON_ArrayForEach(item, at(state, "item")) {
        bool being_carried = cJSON_IsTrue(at(item, "carried"));
        bool being_worn = cJSON_IsTrue(at(item, "worn"));
        bool within_reach = !being_carried && *room && equal_name(room, js(item, "room"));
        if (!(held ? being_carried && !being_worn : worn ? being_worn :
              nearby ? within_reach : being_carried || within_reach)) continue;
        format(&a->info, "%s · %s\n%s\n\n", item_state(item, "Nearby"), js(item, "name"), js(item, "text"));
        shown++;
    }
    if (!shown) append(&a->info, "No items in this view.\n\n");
    append(&a->info, "/inspect ITEM   /take ITEM   /wear ITEM   /remove ITEM   /drop ITEM");
    cJSON_Delete(state);
}

static Json *world_state(const Json *story, int head)
{
    Json *state = cJSON_CreateObject();
    const Json *world = at(story,"world"), *rooms = at(world,"rooms"), *links = at(world,"links");
    put(state,"rooms",cJSON_IsArray(rooms) ? cJSON_Duplicate(rooms,true) : cJSON_CreateArray());
    put(state,"links",cJSON_IsArray(links) ? cJSON_Duplicate(links,true) : cJSON_CreateArray());
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

static void describe_world(const Json *state, Text *out)
{
    const char *room = js(state, "room");
    if (!*room) return;
    Json *r = named(at(state, "rooms"), room);
    format(out, "Room: %s\n%s\n", room, js(r, "text"));
    if (*js(state, "weather")) format(out, "Weather: %s\n", js(state, "weather"));
    Json *link, *item;
    int count = 0;
    cJSON_ArrayForEach(link, at(state, "links")) {
        if (out->len > 10000) break;
        const char *from = js(link, "from"), *to = js(link, "to");
        if (equal_name(from, room) || equal_name(to, room))
            format(out, "%s%s", count++ ? ", " : "Exits: ", equal_name(from, room) ? to : from);
    }
    if (count) append(out, "\n");
    cJSON_ArrayForEach(item, at(state, "item")) {
        if (out->len > 12000) break;
        bool carried = cJSON_IsTrue(at(item, "carried"));
        if (carried || equal_name(js(item, "room"), room))
            format(out, "%s: %s — %s\n", item_state(item, "Nearby"), js(item, "name"), js(item, "text"));
    }
    cJSON_ArrayForEach(item, at(state, "npc")) {
        if (out->len > 14000) break;
        if (equal_name(js(item, "room"), room))
            format(out, "Present character: %s — %s\n", js(item, "name"), js(item, "text"));
    }
    if (out->len > 10000) append(out,"Use world_inspect to see more saved state.\n");
}

void world_context(const Json *story, int head, Text *out)
{
    Json *state = world_state(story, head);
    describe_world(state, out);
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
    Json *state = world_state(story,head);
    append(out,js(state,"room"));
    cJSON_Delete(state);
    return str(out);
}
const char *world_character_room(const Json *story, int head, const char *name, Text *out)
{
    Json *state = world_state(story, head);
    append(out, js(named(at(state, "npc"), name), "room"));
    cJSON_Delete(state);
    return str(out);
}

static bool save_event(Json *story, int head, const char *type, const char *name,
                       const char *text, const char *room)
{
    Json *events = array(world_object(story),"events");
    Json *event = cJSON_CreateObject();
    integer(event,"at",head);
    set(event,"type",type);
    set(event,"name",name);
    set(event,"text",text);
    set(event,"room",room);
    cJSON_AddItemToArray(events,event);
    return true;
}

bool world_inspect(const Json *story, int head, const Json *args, Text *out)
{
    const Json *offset_value = at(args,"offset");
    int offset = 0;
    if (offset_value) {
        if (!cJSON_IsNumber(offset_value) || !(offset_value->valuedouble >= 0 && offset_value->valuedouble <= INT_MAX) ||
            offset_value->valuedouble != (double)(int)offset_value->valuedouble) return false;
        offset = (int)offset_value->valuedouble;
    }
    Json *state = world_state(story,head), *item;
    format(out,"Current room: %s\n",*js(state,"room") ? js(state,"room") : "none");
    if (*js(state,"weather")) format(out,"Weather: %s\n",js(state,"weather"));
    const char *const buckets[] = {"rooms","links","npc","item"};
    size_t index = 0, shown = 0;
    for (size_t bucket=0; bucket<sizeof buckets/sizeof *buckets; ++bucket) {
        cJSON_ArrayForEach(item,at(state,buckets[bucket])) {
            if (index++ < (size_t)offset) continue;
            if (shown++ == 64) {
                format(out,"Next offset: %zu.\n",index-1);
                cJSON_Delete(state);
                return true;
            }
            if (bucket == 0) format(out,"Room: %s\n",js(item,"name"));
            if (bucket == 1) format(out,"Exit: %s ↔ %s\n",js(item,"from"),js(item,"to"));
            if (bucket == 2) format(out,"Character: %s in %s\n",js(item,"name"),js(item,"room"));
            if (bucket == 3) format(out,"Item: %s · %s\n",js(item,"name"),item_state(item,js(item,"room")));
        }
    }
    cJSON_Delete(state);
    return true;
}

bool world_edit(Json *story, int head, const Json *args)
{
    const char *action = js(args,"action"), *name = js(args,"name");
    const char *room = js(args,"room"), *description = js(args,"description");
    if (strlen(description) > 600) return false;
    Json *state = world_state(story,head);
    Json *rooms = at(state,"rooms"), *place = named(rooms,name);
    const char *here = js(state,"room"), *type = nullptr;
    if (!strcmp(action,"room") && valid_name(name) && *description) type = "room";
    else if (!strcmp(action,"remove_room") && place && !equal_name(name,here)) {
        Json *item;
        type = "roomdelete";
        cJSON_ArrayForEach(item,at(state,"links"))
            if (equal_name(js(item,"from"),name) || equal_name(js(item,"to"),name)) type = nullptr;
        cJSON_ArrayForEach(item,at(state,"npc"))
            if (equal_name(js(item,"room"),name)) type = nullptr;
        cJSON_ArrayForEach(item,at(state,"item"))
            if (equal_name(js(item,"room"),name) && !cJSON_IsTrue(at(item,"carried"))) type = nullptr;
    } else if ((!strcmp(action,"connect") || !strcmp(action,"disconnect")) &&
               place && named(rooms,room) && !equal_name(name,room)) {
        const bool remove = !strcmp(action,"disconnect");
        if (linked(state,name,room) == remove) type = remove ? "unlink" : "link";
    } else if (place && (!strcmp(action,"relocate") ||
                        (!strcmp(action,"move") && linked(state,here,name)))) type = "move";
    else if (!strcmp(action,"weather") && at(args,"description") && cJSON_IsString(at(args,"description")))
        type = "weather";
    else if (!strcmp(action,"npc") || !strcmp(action,"item")) {
        Json *bucket = at(state,action), *item = named(bucket,name);
        if (!*room) room = here;
        if (valid_name(name) && named(rooms,room) && (item || *description))
            type = !strcmp(action,"npc") ? "npcplace" : "itemplace";
    } else if (!strcmp(action,"remove_npc") && named(at(state,"npc"),name)) type = "npcdelete";
    else if (!strcmp(action,"remove_item") && named(at(state,"item"),name)) type = "itemdelete";
    bool valid = type && save_event(story,head,type,name,description,room);
    cJSON_Delete(state);
    return valid;
}

bool world_inventory_tool(Json *story, int head, const Json *args)
{
    const char *action = js(args, "action"), *name = js(args, "name"), *text = js(args, "description");
    if (!valid_name(name)) return false;
    bool add = !strcmp(action, "add"), edit = !strcmp(action, "edit");
    bool take = !strcmp(action, "take"), drop = !strcmp(action, "drop");
    bool wear = !strcmp(action,"wear"), remove = !strcmp(action,"remove");
    if (!(add || edit || take || drop || wear || remove) ||
        ((add || edit) && (!*text || strlen(text) > 600))) return false;
    Json *state = world_state(story, head);
    const char *room = js(state, "room");
    Json *item = named(at(state, "item"), name);
    bool carried = cJSON_IsTrue(at(item, "carried"));
    bool worn = cJSON_IsTrue(at(item,"worn"));
    bool nearby = item && *room && equal_name(room, js(item, "room")) && !carried;
    bool valid = *room && (add ? !item :
                          edit ? carried || nearby : take ? nearby : carried && (!wear || !worn) && (!remove || worn));
    if (valid) valid = save_event(story,head,add ? "item" : edit ? "itemedit" : action,
                                  item ? js(item,"name") : name,add || edit ? text : "",room);
    cJSON_Delete(state);
    return valid;
}

bool world_address(const Json *story, const char *line, Text *speaker, const char **message)
{
    *message = line;
    if (*line != '@') return true;
    Json *state = world_state(story, number(story, "head")), *npc, *chosen = NULL;
    if (!*js(state,"room")) { cJSON_Delete(state); return true; }
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
        describe_world(state, &a->info);
        if (!a->info.len) append(&a->info, "Create the starting room with /room NAME | DESCRIPTION.\n");
        append(&a->info, "\n/room  /connect  /go  /item  /take  /drop  /wear  /remove  /npc\n"
                         "@Name MESSAGE addresses a character in this room.\n/world log shows branch activity. /map shows room links.");
    }
    cJSON_Delete(state);
}

static void inspect_world(App *a, const char *name)
{
    Json *state = world_state(a->story, number(a->story, "head"));
    const char *room = js(state, "room");
    if (!*room) { app_status(a, "Create a starting room with /room first."); cJSON_Delete(state); return; }
    const Json *place = !*name || equal_name(name, room) ? named(at(state, "rooms"), room) : nullptr;
    const Json *item = named(at(state, "item"), name);
    const Json *character = named(at(state, "npc"), name);
    const bool carried = cJSON_IsTrue(at(item, "carried"));
    const bool item_visible = item && (carried || equal_name(room, js(item, "room")));
    const bool character_visible = character && equal_name(room, js(character, "room"));
    if (place || item_visible || character_visible) {
        app_info(a, "Inspect world");
        if (place) format(&a->info, "Room  %s\n%s\n\n", room, js(place, "text"));
        if (item_visible) format(&a->info, "%s  %s\n%s\n\n", item_state(item, "Nearby item"),
                                 js(item, "name"), js(item, "text"));
        if (character_visible) format(&a->info, "Present character  %s\n%s\n", js(character, "name"),
                                      js(character, "text"));
    } else app_status(a, "Nothing with that name is here or carried.");
    cJSON_Delete(state);
}

static void room_roster(App *a)
{
    Json *state = world_state(a->story, number(a->story, "head"));
    const char *room = js(state, "room");
    app_info(a, "Who is here");
    format(&a->info, "%s\n\n", *room ? room : "No current room");
    Json *character;
    int count = 0;
    cJSON_ArrayForEach(character, at(state, "npc")) {
        if (!*room || !equal_name(js(character, "room"), room)) continue;
        format(&a->info, "%s\n%s\n\n", js(character, "name"), js(character, "text"));
        count++;
    }
    append(&a->info, count ? "@NAME MESSAGE addresses a character here." : "No characters are present.");
    cJSON_Delete(state);
}
static void room_exits(App *a)
{
    Text current = {0};
    const char *room = world_room(a->story, number(a->story, "head"), &current);
    app_info(a, "Room exits");
    if (*room) format(&a->info, "%s\n\n", room);
    Json *state = world_state(a->story,number(a->story,"head"));
    const Json *rooms = at(state, "rooms");
    int exits = 0;
    Json *link;
    cJSON_ArrayForEach(link, at(state, "links")) {
        const char *from = js(link, "from"), *to = js(link, "to");
        const char *destination = equal_name(room, from) ? to : equal_name(room, to) ? from : "";
        const Json *place = named(rooms, destination);
        if (!place) continue;
        format(&a->info, "/go %s\n%s\n\n", js(place, "name"), js(place, "text"));
        exits++;
    }
    if (!exits) append(&a->info, "No exits are available.");
    cJSON_Delete(state);
    release(&current);
}

typedef struct {
    const char *name;
    int parent, distance, queue;
} RoomVisit;

static int map_index(const RoomVisit *rooms, int count, const char *name)
{
    for (int i = 0; i < count; i++)
        if (equal_name(rooms[i].name, name)) return i;
    return -1;
}

static void world_map(App *a, const char *destination)
{
    Json *world = world_state(a->story,number(a->story,"head")), *item;
    const int count = cJSON_GetArraySize(at(world,"rooms"));
    RoomVisit *rooms = count ? calloc((size_t)count,sizeof *rooms) : nullptr;
    if (!rooms) {
        app_status(a,count ? "Could not allocate the world map." : "Create a starting room with /room first.");
        cJSON_Delete(world);
        return;
    }
    int index = 0;
    cJSON_ArrayForEach(item,at(world,"rooms")) {
        rooms[index].name = js(item,"name");
        rooms[index].distance = -1;
        ++index;
    }
    int start = map_index(rooms,count,js(world,"room"));
    int end = *destination ? map_index(rooms,count,destination) : -1;
    if (start < 0 || (*destination && end < 0)) {
        app_status(a,start < 0 ? "The current room is missing from the map." : "Unknown destination.");
        goto done;
    }
    rooms[0].queue = start;
    rooms[start].parent = -1;
    rooms[start].distance = 0;
    for (int head = 0, tail = 1; head < tail; ++head) {
        int from = rooms[head].queue;
        if (from == end) break;
        cJSON_ArrayForEach(item,at(world,"links")) {
            const char *left = js(item,"from"), *right = js(item,"to");
            const char *neighbor = equal_name(left,rooms[from].name) ? right :
                                   equal_name(right,rooms[from].name) ? left : nullptr;
            if (!neighbor) continue;
            int next = map_index(rooms,count,neighbor);
            if (next < 0 || rooms[next].distance >= 0) continue;
            rooms[next].parent = from;
            rooms[next].distance = rooms[from].distance+1;
            rooms[tail++].queue = next;
        }
    }
    app_info(a,"World map");
    format(&a->info,"You are here: %s\n\n",rooms[start].name);
    if (end >= 0) {
        if (rooms[end].distance < 0) format(&a->info,"No route to %s.\n",rooms[end].name);
        else if (end == start) format(&a->info,"You are already at %s.\n",rooms[end].name);
        else {
            const int distance = rooms[end].distance;
            format(&a->info,"Shortest route to %s (%d %s)\n\n",rooms[end].name,distance,distance == 1 ? "step" : "steps");
            int route = end;
            for (int i = distance; i >= 0; --i) {
                rooms[i].queue = route;
                route = rooms[route].parent;
            }
            for (int i = 0; i <= distance; ++i) format(&a->info,"%s%s",i ? " → " : "",rooms[rooms[i].queue].name);
            format(&a->info,"\n\nNext move: /go %s\n",rooms[rooms[1].queue].name);
        }
    } else for (int i = 0; i < count; ++i) {
        format(&a->info,"%s%s",i == start ? "● " : "○ ",rooms[i].name);
        if (rooms[i].distance < 0) append(&a->info," (unreachable)");
        int exits = 0;
        cJSON_ArrayForEach(item,at(world,"links")) {
            const char *left = js(item,"from"), *right = js(item,"to");
            if (equal_name(left,rooms[i].name)) format(&a->info,"%s%s",exits++ ? ", " : " → ",right);
            else if (equal_name(right,rooms[i].name)) format(&a->info,"%s%s",exits++ ? ", " : " → ",left);
        }
        append(&a->info,"\n");
    }
done:
    free(rooms);
    cJSON_Delete(world);
}

static bool add_event(App *a, const char *type, const char *name,
                      const char *text, const char *room)
{
    if (!save_event(a->story,number(a->story,"head"),type,name,text,room)) {
        app_status(a,"World activity limit reached.");
        return false;
    }
    a->ws.dirty = true;
    app_status(a, "World action saved");
    return true;
}

void world_command(App *a, const char *verb, char *argument)
{
    if (!a->roleplay || !a->story) { app_status(a, "Open a roleplay story to use its world."); return; }
    if (!strcmp(verb, "world")) { world_panel(a, !strcmp(argument, "log")); return; }
    if (!strcmp(verb, "map")) { world_map(a, trim(argument)); return; }
    if (!strcmp(verb, "inspect")) { inspect_world(a, trim(argument)); return; }
    if (!strcmp(verb, "inventory")) { inventory_panel(a, trim(argument)); return; }
    if (!strcmp(verb, "who")) { room_roster(a); return; }
    if (!strcmp(verb, "exits")) { room_exits(a); return; }
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
    Json *state = world_state(a->story,number(a->story,"head")), *rooms = at(state,"rooms");
    if (room_cmd) {
        if (named(rooms,name)) app_status(a,"A room with that name already exists.");
        else if (add_event(a,"room",name,text,"")) app_status(a,"Room created");
        cJSON_Delete(state);
        return;
    }
    if (!*js(state,"room")) {
        app_status(a,"Create a starting room with /room first.");
        cJSON_Delete(state);
        return;
    }
    if (link_cmd) {
        Json *from = named(rooms, name), *to = named(rooms, text);
        if (!from || !to || from == to) app_status(a,"Choose two distinct existing rooms.");
        else if (linked(state,name,text)) app_status(a,"These rooms are already connected.");
        else if (add_event(a,"link",js(from,"name"),"",js(to,"name"))) app_status(a,"Rooms connected");
        cJSON_Delete(state);
        return;
    }

    const char *room = js(state, "room");
    if (prop_cmd) {
        if (named(at(state, verb), name)) app_status(a, "That name already exists on this branch.");
        else add_event(a, verb, name, text, room);
    } else if (!strcmp(verb, "go")) {
        Json *target = named(rooms, name);
        if (!target || !linked(state, room, js(target, "name"))) app_status(a, "Choose an exit shown by /world.");
        else add_event(a, "move", js(target, "name"), "", room);
    } else if (!strcmp(verb, "take") || !strcmp(verb, "drop")) {
        Json *object = named(at(state, "item"), name);
        bool carried = cJSON_IsTrue(at(object, "carried")), taking = !strcmp(verb, "take");
        if (!object || (taking ? carried || !equal_name(room, js(object, "room")) : !carried))
            app_status(a, taking ? "That item is unavailable in this room." : "That item is outside your inventory.");
        else add_event(a, verb, js(object, "name"), "", room);
    } else if (!strcmp(verb, "wear") || !strcmp(verb, "remove")) {
        Json *object = named(at(state, "item"), name);
        bool wearing = !strcmp(verb, "wear"), worn = cJSON_IsTrue(at(object, "worn"));
        if (!cJSON_IsTrue(at(object, "carried")) || wearing == worn)
            app_status(a, wearing ? "Choose a carried item you are not wearing." : "Choose a worn item.");
        else add_event(a, verb, js(object, "name"), "", room);
    } else if (!strcmp(verb, "say") || !strcmp(verb, "weather")) {
        if (!*name || strlen(name) > 600) app_status(a, "Enter text up to 600 bytes.");
        else add_event(a, verb, "", name, room);
    }
    cJSON_Delete(state);
}
