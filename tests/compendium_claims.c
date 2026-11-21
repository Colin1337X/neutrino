#include "neutrino.h"

#include <stdio.h>
#include <string.h>

struct Example {
    const char *type, *name, *input, *expected;
};

static const struct Example cases[] = {
    {"character", "Captain Mara", "Marooned on the west pier.", ""},
    {"place", "West Pier", "The location where Captain Mara is currently stranded.", ""},
    {"place", "Red Box", "The location where Iona is concealing the copper key.", ""},
    {"character", "Iona", "Station keeper hiding the copper key in the red box.", "Station keeper."},
    {"event", "Key location", "The copper key is inside the red box.", ""},
    {"item", "Copper key", "The copper key is hidden within the red box.", ""},
    {"place", "Red Box", "Where Iona hides the copper key.", ""},
    {"character", "Captain Mara", "Stranded on the west pier. She lights a blue lantern.", "She lights a blue lantern."},
    {"character", "Captain Mara", "Stranded on the east pier.", "Stranded on the east pier."},
    {"character", "Captain Mara", "Stranded under the west pier.", "Stranded under the west pier."},
    {"character", "Iona", "Hiding the silver whistle inside the red box.", "Hiding the silver whistle inside the red box."},
    {"character", "Captain Mara", "Hiding the copper key in the red box.", "Hiding the copper key in the red box."},
    {"character", "Captain Mara", "She is not stranded on the west pier.", "She is not stranded on the west pier."},
    {"character", "Captain Mara", "Captain Mara was stranded on the west pier.", "Captain Mara was stranded on the west pier."},
    {"character", "Captain Mara", "Stranded on the west pier until midnight.", "Stranded on the west pier until midnight."},
    {"character", "Captain Mara", "Stranded on the west pier and injured by the storm.", "Stranded on the west pier and injured by the storm."},
};

int main(void)
{
    Json *known = cJSON_Parse(
        "[{\"type\":\"place\",\"name\":\"West Pier\",\"description\":\"The west pier where Captain Mara is currently stranded.\"},"
        "{\"type\":\"place\",\"name\":\"Red Box\",\"description\":\"The red box where Iona is hiding the copper key.\"}]");
    if (!known) return 2;
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const struct Example *example = &cases[i];
        Text out = {0};
        compendium_novel_claims(known, NULL, example->type, example->name, example->input, &out);
        if (strcmp(example->expected, str(&out))) {
            fprintf(stderr, "%s\nExpected: [%s]\nReceived: [%s]\n", example->input, example->expected, str(&out));
            rc = 1;
        }
        release(&out);
    }
    Json *batch = cJSON_Parse(
        "[{\"type\":\"note\",\"name\":\"Key moved\",\"description\":\"The copper key is in the blue drawer.\"}]");
    Text returned = {0};
    const char *line = "The copper key is inside the red box.";
    if (!compendium_novel_claims(known, batch, "item", "Copper key", line, &returned) ||
        strcmp(line, str(&returned))) {
        fputs("Revisiting a location after a conflicting update must preserve the claim.\n", stderr);
        rc = 1;
    }
    release(&returned);
    Json *role = cJSON_Parse(
        "[{\"type\":\"character\",\"name\":\"Iona\",\"description\":\"Station keeper who hides the copper key in the red box.\"}]");
    Text container = {0};
    compendium_novel_claims(role, NULL, "place", "Red Box", "Where Iona hides the copper key.", &container);
    if (container.len) {
        fputs("Container and character entries should share the same concealment claim.\n", stderr);
        rc = 1;
    }
    release(&container);
    cJSON_Delete(role);
    Text prop = {0}, evidence = {0};
    const char *placement = "Iona carefully places the silver whistle on the railing, securing it to the edge of the pier.";
    if (!compendium_placement_evidence(known, NULL, placement, &prop, &evidence) ||
        strcmp(str(&prop), "silver whistle") || strcmp(str(&evidence), placement)) {
        fputs("An explicit first placement should retain the new item and its full evidence.\n", stderr);
        rc = 1;
    }
    clear(&prop);
    clear(&evidence);
    if (compendium_placement_evidence(known, NULL, "Iona places the copper key inside the red box.", &prop, &evidence) ||
        compendium_placement_evidence(known, NULL, "Iona imagines she places the silver whistle on the railing.", &prop, &evidence) ||
        compendium_placement_evidence(known, NULL, "Iona places the silver whistle on the railing, or imagines it.", &prop, &evidence)) {
        fputs("Known, imagined or ambiguous placements must stay out of evidence filing.\n", stderr);
        rc = 1;
    }
    release(&prop);
    release(&evidence);
    cJSON_Delete(batch);
    cJSON_Delete(known);
    return rc;
}
