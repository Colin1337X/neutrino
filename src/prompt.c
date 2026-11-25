#include "neutrino.h"

#include <string.h>

static size_t text_tokens(const char *text)
{
    /* The chat API does not provide a portable tokenizer. Weight UTF-8 bytes
       more heavily and charge each message for the chat template's framing. */
    size_t units = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c)
        units += *c < 0x80 ? 1 : 2;
    return (units + 3) / 4;
}

static const char *player_speaker(const Json *node)
{
    return !strcmp(js(node, "role"), "user") ? js(node, "speaker") : "";
}

static size_t message_tokens(const Json *message, const char *speaker)
{
    enum { MESSAGE_FRAMING = 6 };
    return MESSAGE_FRAMING + text_tokens(js(message, "content")) +
        (*speaker ? text_tokens(speaker) + 2 : 0);
}

void narrator_add_history(Json *messages, const Json *path, int covered, int insertion,
                          int budget, Text *trace)
{
    enum { GENERATION_FRAMING = 12 };
    size_t used = GENERATION_FRAMING;
    const Json *message;
    cJSON_ArrayForEach(message, messages) used += message_tokens(message, "");
    size_t required = used;
    int room = NEUTRINO_GATEWAY_MESSAGE_LIMIT - cJSON_GetArraySize(messages);
    int count = cJSON_GetArraySize(path), first = count > 0 ? count - 1 : 0;
    const Json *last = count ? path->child->prev : NULL;
    const Json *start = last;

    /* The caller already rendered the last turn and the current instruction.
       Add the newest contiguous history suffix within the remaining budget. */
    while (first > covered && room > 0) {
        const Json *node = start->prev;
        size_t cost = message_tokens(node, player_speaker(node));
        if (used + cost > (size_t)budget) break;
        used += cost;
        start = node;
        first--;
        room--;
    }
    if (start != last && first > covered && !strcmp(js(start, "role"), "assistant") &&
        !strcmp(js(start->prev, "role"), "user")) {
        used -= message_tokens(start, "");
        start = start->next;
        first++;
    }
    for (const Json *node = last; node != start;) {
        node = node->prev;
        Json *entry = cJSON_CreateObject();
        set(entry, "role", js(node, "role"));
        const char *speaker = player_speaker(node);
        if (*speaker) {
            Text labeled = {0};
            format(&labeled, "[%s] %s", speaker, js(node, "content"));
            set(entry, "content", str(&labeled));
            release(&labeled);
        } else set(entry, "content", js(node, "content"));
        cJSON_InsertItemInArray(messages, insertion, entry);
    }
    if (first > covered)
        format(trace, "%d older messages omitted by the history budget or gateway message limit.\n", first - covered);
    if (covered) format(trace, "Recap covers the first %d visible messages.\n", covered);
    format(trace, "Narrator prompt estimate: %zu / %d tokens (%zu required).\n", used, budget, required);
}
