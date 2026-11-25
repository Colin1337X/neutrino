#include "neutrino.h"

#include <stdio.h>
#include <string.h>

static Json *message(Json *list, const char *role, const char *content)
{
    Json *node = cJSON_CreateObject();
    set(node, "role", role);
    set(node, "content", content);
    cJSON_AddItemToArray(list, node);
    return node;
}

static bool content_is(const Json *messages, int index, const char *expected)
{
    return !strcmp(js(cJSON_GetArrayItem(messages, index), "content"), expected);
}

int main(void)
{
    Workspace ws = {.root = cJSON_CreateObject()};
    bool ok = workspace_context_budget(&ws) == 5000;
    integer(ws.root, "contextBudget", 4096);
    ok &= workspace_context_budget(&ws) == 4096;
    integer(ws.root, "neutrinoBudget", 256);
    ok &= workspace_context_budget(&ws) == 256;
    cJSON_Delete(ws.root);

    Json *path = cJSON_CreateArray();
    message(path, "assistant", "Covered opening");
    message(path, "user", "Older question");
    message(path, "assistant", "Older reply");
    set(message(path, "user", "Look left."), "speaker", "Iona");
    message(path, "assistant", "A bell rang.");
    message(path, "user", "What rang?");

    Json *messages = cJSON_CreateArray();
    message(messages, "system", "Lore, recap, and memory.");
    message(messages, "user", "Reference data.");
    message(messages, "user", "What rang?");
    Text trace = {0};
    narrator_add_history(messages, path, 1, 2, 74, &trace);
    ok &= cJSON_GetArraySize(messages) == 5 &&
        content_is(messages, 0, "Lore, recap, and memory.") &&
        content_is(messages, 1, "Reference data.") &&
        content_is(messages, 2, "[Iona] Look left.") &&
        content_is(messages, 3, "A bell rang.") &&
        content_is(messages, 4, "What rang?") &&
        strstr(str(&trace), "2 older messages omitted") &&
        strstr(str(&trace), "Recap covers the first 1");
    cJSON_Delete(messages);
    release(&trace);

    messages = cJSON_CreateArray();
    message(messages, "system", "Required context.");
    message(messages, "user", "An explicit latest player instruction.");
    narrator_add_history(messages, path, 0, 1, 16, &trace);
    ok &= cJSON_GetArraySize(messages) == 2 &&
        content_is(messages, 1, "An explicit latest player instruction.") &&
        strstr(str(&trace), "5 older messages omitted");
    release(&trace);
    cJSON_Delete(messages);
    cJSON_Delete(path);

    path = cJSON_CreateArray();
    for (int i = 0; i < 300; i++) {
        message(path, "user", "Act.");
        message(path, "assistant", "Done.");
    }
    messages = cJSON_CreateArray();
    message(messages, "system", "System.");
    message(messages, "assistant", "Done.");
    message(messages, "user", "Continue the story.");
    narrator_add_history(messages, path, 0, 1, 32768, &trace);
    ok &= cJSON_GetArraySize(messages) == NEUTRINO_GATEWAY_MESSAGE_LIMIT &&
        content_is(messages, 1, "Act.") &&
        content_is(messages, NEUTRINO_GATEWAY_MESSAGE_LIMIT - 1, "Continue the story.");
    release(&trace);
    cJSON_Delete(messages);
    cJSON_Delete(path);

    if (!ok) fprintf(stderr, "Narrator history budget, required context, or message order changed.\n");
    return ok ? 0 : 1;
}
