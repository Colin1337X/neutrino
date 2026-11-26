#include "neutrino.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const struct { const char *reply, *prefix; } examples[] = {
        {"The bell rang. The clock hands began to", "The bell rang."},
        {"Iona opened the", ""},
        {"She called to Capt. Mara, then", ""},
        {"The door opened. J. Smith pushed the", "The door opened."},
        {"She said, “Wait.”\nThen she", "She said, “Wait.”"},
        {"The bell rang. [[2]]\nThe clock was", "The bell rang. [[2]]"},
        {"Listen on channel A.", "Listen on channel A."},
        {"The gauge read 3.14. She reached", "The gauge read 3.14."},
        {"Iona stopped… then turned to", ""},
        {"*She drew back.* ", "*She drew back.*"},
    };
    for (size_t i = 0; i < sizeof examples / sizeof *examples; ++i) {
        size_t kept = narrator_complete_prefix(examples[i].reply);
        if (kept != strlen(examples[i].prefix) ||
            memcmp(examples[i].reply, examples[i].prefix, kept)) {
            fprintf(stderr, "Unexpected sentence boundary for %s\n", examples[i].reply);
            return 1;
        }
    }
    const char *code = "LANTERN QUARTZ SEVEN", *partial = "She opened the";
    if (!narrator_short_answer(code, strlen(code), "What is the locker code? Reply with the code only.") ||
        narrator_short_answer(code, strlen(code), "Continue the scene.") ||
        narrator_short_answer(partial, strlen(partial), "Return only the code.") ||
        !narrator_short_answer("-3.14", 5, "") || narrator_short_answer("+.", 2, "")) return 1;
    int cols = 138, rows = 34;
    bound_terminal_viewport(&cols, &rows);
    if (cols != 138 || rows != 34) return 1;
    cols = rows = INT_MAX;
    bound_terminal_viewport(&cols, &rows);
    if (cols != 240 || rows != 120) return 1;
    return 0;
}
