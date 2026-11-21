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
    Json *relay = cJSON_Parse(
        "[{\"type\":\"character\",\"name\":\"Iona\",\"description\":"
        "\"Station keeper hiding the copper key in a red box to prevent access before the tide recedes.\"},"
        "{\"type\":\"event\",\"name\":\"Copper Key Location\",\"description\":"
        "\"Iona secures the copper key within the red box.\"}]");
    const struct Example relay_cases[] = {
        {"character", "Iona", "Station keeper hiding the copper key in a red box.", ""},
        {"item", "Copper key", "The copper key is hidden within the red box.", ""},
        {"character", "Iona", "Station keeper hiding the copper key in the red box. She ties a silver whistle to the railing.",
         "She ties a silver whistle to the railing."},
        {"character", "Iona", "Station keeper hiding the copper key in the red box until sunrise.",
         "Station keeper hiding the copper key in the red box until sunrise."},
        {"character", "Iona", "Station keeper hiding the copper key in another red box.",
         "Station keeper hiding the copper key in another red box."},
        {"character", "Iona", "Station keeper hiding the copper key in the red box to prevent Mara from using it.",
         "Station keeper hiding the copper key in the red box to prevent Mara from using it."},
        {"character", "Iona", "Harbor guard hiding the copper key in the red box.", "Harbor guard."},
        {"character", "Iona", "Station keeper hiding the copper key in the blue drawer.",
         "Station keeper hiding the copper key in the blue drawer."},
    };
    for (size_t i = 0; i < sizeof relay_cases / sizeof *relay_cases; i++) {
        const struct Example *example = &relay_cases[i];
        Text out = {0};
        compendium_novel_claims(relay, NULL, example->type, example->name, example->input, &out);
        if (strcmp(example->expected, str(&out))) {
            fprintf(stderr, "%s\nExpected: [%s]\nReceived: [%s]\n", example->input, example->expected, str(&out));
            rc = 1;
        }
        release(&out);
    }
    cJSON_Delete(relay);
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
    const char *clamped = "Iona's hand trembles as she clamps the silver whistle to the railing, her voice trembling "
                          "with the weight of the signal before the bell's final toll.";
    Json *whistle = cJSON_Parse("[{\"role\":\"assistant\",\"content\":\"Iona places the silver whistle against the curb.\"},"
        "{\"role\":\"user\",\"content\":\"Say she clamps the silver whistle to the railing.\"}]");
    if (!compendium_placement_evidence(nullptr, whistle, clamped, &prop, &evidence) ||
        strcmp(str(&prop), "silver whistle") || strcmp(str(&evidence), clamped)) {
        fputs("A narrator-confirmed move of a known item must retain the original evidence.\n", stderr);
        rc = 1;
    }
    clear(&prop);
    clear(&evidence);
    Json *placed = cJSON_CreateObject();
    set(placed, "role", "assistant");
    set(placed, "content", clamped);
    cJSON_AddItemToArray(whistle, placed);
    if (compendium_placement_evidence(nullptr, whistle, "She fastens the silver whistle to the railing.", &prop, &evidence) ||
        compendium_placement_evidence(nullptr, whistle, "Iona considers the tide. She glances at the railing.", &prop, &evidence)) {
        fputs("Repeating the current attachment or mentioning the destination must not file a placement.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(whistle);
    release(&prop);
    release(&evidence);
    Json *opening = compendium_opening_evidence(
        "Iona watches the copper key tremble in her hands as she hides it beneath the heavy wooden crate, "
        "her breath caught in a tight knot. A solitary figure stands on the west pier, wearing a tattered white coat. "
        "The red box on the far bank seems to pulse with a soft, rhythmic light.");
    Json *first = cJSON_GetArrayItem(opening, 0);
    if (cJSON_GetArraySize(opening) != 1 || strcmp(js(first, "name"), "copper key") ||
        !strstr(js(first, "description"), "beneath the heavy wooden crate")) {
        fputs("An empty opening filing must recover the witnessed key placement without inventing the red-box location.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(opening);
    opening = compendium_opening_evidence("Iona hides the copper key in the red box. Captain Mara is stranded on the west pier. "
                                          "The west gate opens only at low tide.");
    if (cJSON_GetArraySize(opening) != 3) { fputs("Explicit opening facts were missed.\n", stderr); rc = 1; }
    cJSON_Delete(opening);
    opening = compendium_opening_evidence(
        "Iona hides the copper key in the red box and waits for the tide to fall, knowing that the gate will remain shut until the water recedes. "
        "Captain Mara is stranded on the west pier, waiting for the tide to rise enough to open the heavy gate and rescue her from the storm. "
        "The west gate opens only at low tide, requiring her to keep the door ajar as the waves approach the pier.");
    if (cJSON_GetArraySize(opening) != 3) { fputs("Opening facts embedded in compound sentences were missed.\n", stderr); rc = 1; }
    cJSON_Delete(opening);
    opening = compendium_opening_evidence("The water glimmers. Perhaps Captain Mara is stranded on the pier. "
                                          "Iona imagines that she places a blue lantern on the desk.");
    if (opening->child) { fputs("Atmosphere and speculation must not create opening entries.\n", stderr); rc = 1; }
    cJSON_Delete(opening);
    const char *relay_reply = "You step forward and present your name as Iona, the station keeper, and immediately reveal "
        "the three new facts: the red box is concealed beneath your collar, Captain Mara has been pulled ashore at the "
        "west pier, and the current tide is at its lowest, allowing the west gate to be unlocked.";
    Text label = {0};
    const char *type = compendium_filing_identity("item", "Copper Key", "The red box is concealed beneath Iona's collar.",
                                                 relay_reply, &label);
    if (strcmp(type, "item") || strcmp(str(&label), "Red box")) {
        fputs("Use the object established by the narrator, independently of the player's cue.\n", stderr);
        rc = 1;
    }
    clear(&label);
    type = compendium_filing_identity("item", "Copper Key", "The copper key is concealed beneath the red box.",
                                      "The copper key is concealed beneath the red box.", &label);
    if (strcmp(type, "item") || label.len) { fputs("Do not rename an object to its container.\n", stderr); rc = 1; }
    type = compendium_filing_identity("item", "Copper Key", "The red box is concealed beneath Iona's collar.",
                                      "Iona points to the red box. The copper key is concealed beneath her collar.", &label);
    if (strcmp(type, "item") || label.len) { fputs("A mention alone does not ground a replacement name.\n", stderr); rc = 1; }
    type = compendium_filing_identity("creature", "Captain Mara", "Captain Mara has been pulled ashore at the west pier.",
                                      relay_reply, &label);
    if (strcmp(type, "character") || label.len) { fputs("Named captain should file as a character.\n", stderr); rc = 1; }
    type = compendium_filing_identity("creature", "Captain Ember", "Captain Ember watches the bay.",
                                      "Captain Ember, a dragon, watches the bay.", &label);
    if (strcmp(type, "creature") || label.len) { fputs("Preserve explicit species evidence.\n", stderr); rc = 1; }
    const char *tucked = "The copper key remains tucked beneath the red box, a silent guardian of the station's history.";
    type = compendium_filing_identity("character", "Copper Key", tucked, tucked, &label);
    if (strcmp(type, "item") || label.len) { fputs("A figuratively described physical key must file as an item.\n", stderr); rc = 1; }
    const char *hidden = "Iona hides the copper key beneath her coat.";
    type = compendium_filing_identity("character", "Copper Key", hidden, hidden, &label);
    if (strcmp(type, "item")) { fputs("A labeled object handled by a character must file as an item.\n", stderr); rc = 1; }
    const char *person = "Copper Key stepped onto the pier and told Mara his story.";
    type = compendium_filing_identity("character", "Copper Key", person, person, &label);
    if (strcmp(type, "character")) { fputs("An object-like name alone must not change a character's type.\n", stderr); rc = 1; }
    type = compendium_filing_identity("character", "Copper Key", tucked, "Iona stares at the water.", &label);
    if (strcmp(type, "character")) { fputs("Do not use an ungrounded object's description to reclassify it.\n", stderr); rc = 1; }
    release(&label);
    Text quote = {0};
    const char *narration = "Iona considers hiding the copper key, but leaves it on the console. She ties the silver whistle to the railing.";
    if (compendium_filing_evidence(narration, "Iona hides the copper key in the red box.", &quote) ||
        compendium_filing_evidence(narration, "Captain Mara is stranded on the west pier.", &quote) ||
        compendium_filing_evidence(narration, "Iona presses the red button.", &quote)) {
        fputs("Player-only or earlier-turn prose must not ground a filing.\n", stderr);
        rc = 1;
    }
    if (!compendium_filing_evidence(narration, "hiding the copper key", &quote) ||
        strcmp(str(&quote), "Iona considers hiding the copper key, but leaves it on the console.") ||
        compendium_filing_label("Copper key hiding in the red box", str(&quote))) {
        fputs("A quoted fragment must retain its qualifying sentence and ground its entity label.\n", stderr);
        rc = 1;
    }
    if (!compendium_filing_evidence(narration, "ties the silver whistle to the railing", &quote) ||
        strcmp(str(&quote), "She ties the silver whistle to the railing.") ||
        !compendium_filing_label("Silver whistle", str(&quote))) {
        fputs("An exact new action must retain its original supporting prose.\n", stderr);
        rc = 1;
    }
    release(&quote);
    Text listing = {0};
    const char *tagged = "The tide rises.\n\nIona ties the silver whistle to the railing.";
    compendium_filing_index(tagged, nullptr, &listing);
    if (strcmp(str(&listing), "S1 | The tide rises.\nS2 | Iona ties the silver whistle to the railing.\n") ||
        compendium_filing_index(tagged, "S3", &quote) ||
        !compendium_filing_index(tagged, "S2", &quote) ||
        strcmp(str(&quote), "Iona ties the silver whistle to the railing.") ||
        compendium_filing_label("radio", str(&quote))) {
        fputs("Evidence labels must resolve to the original numbered narrator sentence.\n", stderr); rc = 1;
    }
    release(&listing);
    release(&quote);
    Json *signals = compendium_signal_evidence("Iona smiles as she rings the brass bell twice, signaling the approaching tide to the west gate. "
        "Captain Mara's distress flare, once held high above the water, now floats gently on the surface as it descends toward the shore.");
    if (cJSON_GetArraySize(signals) != 2 ||
        strcmp(js(cJSON_GetArrayItem(signals, 0), "name"), "brass bell") ||
        strcmp(js(cJSON_GetArrayItem(signals, 1), "name"), "flare")) {
        fputs("Recover the two explicit signals from the exact historical reply.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(signals);
    signals = compendium_signal_evidence("Iona grips the brass bell in her hand, the vibration shaking her trembling hands "
        "as she strikes the heavy brass with a sharp, rhythmic motion, repeating the strike until the brass resonates clearly. "
        "She watches the flare, the sudden bright white signal contrasting sharply with the murky water around the west gate.");
    if (cJSON_GetArraySize(signals) != 2) { fputs("Witnessed strikes and visible flares must both file.\n", stderr); rc = 1; }
    cJSON_Delete(signals);
    signals = compendium_signal_evidence("Iona's hands moved swiftly as she reached the brass bell, striking it once to summon "
        "the signal, and striking the bell again to complete the two-ring broadcast before stepping back to await the flare.");
    if (cJSON_GetArraySize(signals) != 1 || strcmp(js(cJSON_GetArrayItem(signals, 0), "name"), "brass bell")) {
        fputs("A witnessed bell strike must file without treating an awaited flare as observed.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(signals);
    signals = compendium_signal_evidence("Iona imagines she rings the bell. Would she ring the bell? She watches the water.");
    if (signals->child) { fputs("Imagined or proposed signals must not become facts.\n", stderr); rc = 1; }
    cJSON_Delete(signals);
    const char *bright = "The flare flares bright, illuminating the white line on the water.";
    signals = compendium_signal_evidence(bright);
    const Json *flare = cJSON_GetArrayItem(signals, 0);
    if (cJSON_GetArraySize(signals) != 1 || strcmp(js(flare, "name"), "flare") ||
        strcmp(js(flare, "description"), bright)) {
        fputs("Recover the exact observed flare sentence from the missed live filing.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(signals);
    signals = compendium_signal_evidence("Iona waits for the signal. She expects the flare ignites near the pier.");
    if (signals->child) { fputs("An expected flare must not become a witnessed event.\n", stderr); rc = 1; }
    cJSON_Delete(signals);
    const char *paired = "A bright, rhythmic flare burns brightly over the water as Iona rings the brass bell twice, signaling the approaching tide.";
    signals = compendium_signal_evidence(paired);
    if (cJSON_GetArraySize(signals) != 2 ||
        strcmp(js(cJSON_GetArrayItem(signals, 0), "name"), "brass bell") ||
        strcmp(js(cJSON_GetArrayItem(signals, 1), "name"), "flare") ||
        strcmp(js(cJSON_GetArrayItem(signals, 1), "description"), paired)) {
        fputs("A shared evidence sentence can establish both observed signals.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(signals);
    signals = compendium_signal_evidence("Captain Mara stands on the opposite side, the flare burning with a violent flash.");
    if (cJSON_GetArraySize(signals) != 1 ||
        strcmp(js(cJSON_GetArrayItem(signals, 0), "name"), "flare")) {
        fputs("A burning flare is observed even in a participial clause.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(signals);
    signals = compendium_signal_evidence("A faint, shimmering flare of orange sparks ripples across the water surface.");
    if (cJSON_GetArraySize(signals) != 1 ||
        strcmp(js(cJSON_GetArrayItem(signals, 0), "name"), "flare")) {
        fputs("Intervening descriptive words must not hide a witnessed flare.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(signals);
    signals = compendium_signal_evidence("Iona calls for the flare and rings the brass bell.");
    if (cJSON_GetArraySize(signals) != 1 ||
        strcmp(js(cJSON_GetArrayItem(signals, 0), "name"), "brass bell")) {
        fputs("A completed bell action must not turn a requested flare into evidence.\n", stderr);
        rc = 1;
    }
    cJSON_Delete(signals);
    const char *opening_miss = "Iona presses the red lock on the crate, her fingers brushing the metal as the heavy door creaks open. "
        "Outside, the wind howls through the debris field, and the station clock's second hand is paused mid-twist, "
        "indicating the clock is currently seven minutes behind the real time.";
    Json *scene = compendium_scene_evidence(opening_miss);
    if (cJSON_GetArraySize(scene) != 2 ||
        strcmp(js(cJSON_GetArrayItem(scene, 0), "name"), "door") ||
        strcmp(js(cJSON_GetArrayItem(scene, 1), "name"), "station clock")) {
        fputs("Recover the opened door and quantified clock offset.\n", stderr); rc = 1;
    }
    cJSON_Delete(scene);
    scene = compendium_scene_evidence("The ledger revealed that the station clock runs seven minutes slow, "
        "a fact only Iona could verify. Iona relayed a command: \"Wait for the bell.\" "
        "Her voice was quiet and steady as the radio crackled on channel nine.");
    if (cJSON_GetArraySize(scene) != 2 ||
        strcmp(js(cJSON_GetArrayItem(scene, 0), "name"), "station clock") ||
        strcmp(js(cJSON_GetArrayItem(scene, 1), "description"),
               "Her voice was quiet and steady as the radio crackled on channel nine.")) {
        fputs("Recover a stated clock offset and a radio observed after dialogue.\n", stderr); rc = 1;
    }
    cJSON_Delete(scene);
    scene = compendium_scene_evidence("The relay radio crackled, the voice clear and urgent, "
        "\"Wait, wait, wait. The second bell is coming, Captain.\" The station clock ticked.");
    if (cJSON_GetArraySize(scene) != 1 || strcmp(js(cJSON_GetArrayItem(scene, 0), "name"), "radio") ||
        !strstr(js(cJSON_GetArrayItem(scene, 0), "description"), "Captain.\"")) {
        fputs("Preserve a witnessed radio sound and its dialogue boundary.\n", stderr); rc = 1;
    }
    cJSON_Delete(scene);
    const char *placement_miss = "Iona's fingers press the silver whistle against the railing, its corked end already lodged "
        "in the steel, as the tide visibly recedes and the rusted latch of the west gate clicks shut behind her. "
        "The copper key, still hidden in the crate, is not visible in the dim light.";
    scene = compendium_scene_evidence(placement_miss);
    Text pressed_name = {0}, pressed_evidence = {0};
    const Json *hidden_key = cJSON_GetArrayItem(scene, 1);
    if (cJSON_GetArraySize(scene) != 2 || strcmp(js(cJSON_GetArrayItem(scene, 0), "name"), "gate") ||
        strcmp(js(hidden_key, "name"), "copper key") || strcmp(js(hidden_key, "type"), "item") ||
        !compendium_placement_evidence(nullptr, nullptr, placement_miss, &pressed_name, &pressed_evidence) ||
        strcmp(str(&pressed_name), "silver whistle")) {
        fputs("Recover the pressed whistle, closed gate and hidden key from narration.\n", stderr); rc = 1;
    }
    cJSON_Delete(scene);
    release(&pressed_name);
    release(&pressed_evidence);
    signals = compendium_signal_evidence("Iona repeats the rhythmic clinking of the brass bell against the dock, "
        "her trembling hands still gripping the striking mechanism as the station clock's second hand pauses mid-twist, "
        "indicating the time has shifted slightly. The red crate is now clear, and Captain Mara's distress signal is now "
        "clearly audible through the heavy steel railing, signaling she is waiting for the flare.");
    if (cJSON_GetArraySize(signals) != 1 || strcmp(js(cJSON_GetArrayItem(signals, 0), "name"), "brass bell")) {
        fputs("Recover the bell action while the flare is still awaited.\n", stderr); rc = 1;
    }
    cJSON_Delete(signals);
    scene = compendium_scene_evidence("Iona imagines the door creaks open. Would the gate click shut? "
        "The station clock is not seven minutes behind. The copper key is not hidden in the crate. "
        "Mara says, \"The door creaks open.\"");
    if (scene->child) { fputs("Unestablished scenery must not become an observation.\n", stderr); rc = 1; }
    cJSON_Delete(scene);
    opening = compendium_opening_evidence("The ledger's ink seems to stain the aged pages. "
        "Iona holds the red box, her fingers hovering over the shallow crescent mark on the copper key's bow.");
    if (cJSON_GetArraySize(opening) != 1 || strcmp(js(cJSON_GetArrayItem(opening, 0), "name"), "red box")) {
        fputs("File a directly held prop while preserving its identity.\n", stderr); rc = 1;
    }
    cJSON_Delete(opening);
    opening = compendium_opening_evidence("The red box lay hidden inside my pocket, untouched by my hand this morning.");
    if (cJSON_GetArraySize(opening) != 1 || strcmp(js(cJSON_GetArrayItem(opening, 0), "name"), "red box")) {
        fputs("A concealed prop remains grounded without guessing the narrator's identity.\n", stderr); rc = 1;
    }
    cJSON_Delete(opening);
    signals = compendium_signal_evidence("The brass bell rings twice, a rhythmic echo that seems to vibrate the water.");
    if (cJSON_GetArraySize(signals) != 1 || strcmp(js(cJSON_GetArrayItem(signals, 0), "name"), "brass bell")) {
        fputs("Uncertainty about the echo must preserve a witnessed bell.\n", stderr); rc = 1;
    }
    cJSON_Delete(signals);
    cJSON_Delete(batch);
    cJSON_Delete(known);
    return rc;
}
