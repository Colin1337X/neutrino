#include "neutrino.h"

#include <ctype.h>
#include <string.h>

static bool abbreviated(const char *start, const char *period)
{
    const char *word = period;
    while (word > start && isalpha((unsigned char)word[-1])) --word;
    size_t length = (size_t)(period - word);
    if (length == 1 && isupper((unsigned char)*word)) {
        const char *next = period + 1;
        while (*next == ' ' || *next == '\t') ++next;
        if (isupper((unsigned char)*next) && next > period + 1 &&
            (next[1] == '.' || islower((unsigned char)next[1]))) return true;
    }
    static const char *const titles[] = {"Mr", "Mrs", "Ms", "Dr", "Prof", "Capt", "Lt", "St", "vs"};
    for (size_t i = 0; i < sizeof titles / sizeof *titles; ++i)
        if (strlen(titles[i]) == length && !memcmp(word, titles[i], length)) return true;
    return false;
}

static const char *after_citation(const char *text)
{
    if (strncmp(text, "[[", 2) || !isdigit((unsigned char)text[2])) return text;
    const char *end = text + 2;
    while (isdigit((unsigned char)*end)) ++end;
    return !strncmp(end, "]]", 2) ? end + 2 : text;
}

size_t narrator_complete_prefix(const char *text)
{
    size_t complete = 0;
    for (const char *p = text; *p; ++p) {
        bool ellipsis = !strncmp(p, "…", 3) || !strncmp(p, "...", 3);
        if (!ellipsis && *p != '.' && *p != '?' && *p != '!') continue;
        if (p == text || isspace((unsigned char)p[-1]) || (*p == '.' && !ellipsis && abbreviated(text, p))) continue;
        const char *end = p + (ellipsis ? 3 : 1);
        while (*end && strchr(".?!", *end)) ++end;
        const char *punctuation_end = end;
        while (*end) {
            if (strchr("\"')]}*_`", *end)) ++end;
            else if (!strncmp(end, "”", 3) || !strncmp(end, "’", 3)) end += 3;
            else break;
        }
        const char *suffix = end;
        while (*suffix == ' ' || *suffix == '\t') ++suffix;
        const char *citation = after_citation(suffix);
        if (citation != suffix) end = citation;
        if (*end && !isspace((unsigned char)*end)) continue;
        if (ellipsis && end == punctuation_end && *suffix && *suffix != '\n' &&
            islower((unsigned char)*suffix)) continue;
        complete = (size_t)(end - text);
        p = end - 1;
    }
    return complete;
}

static bool asks_for_code(const char *cue)
{
    const char *const phrases[] = {"code only", "only the code", "only with the code"};
    for (; *cue; ++cue) {
        for (size_t i = 0; i < sizeof phrases / sizeof *phrases; ++i) {
            const char *a = cue, *b = phrases[i];
            while (*b && tolower((unsigned char)*a) == *b) { ++a; ++b; }
            if (!*b) return true;
        }
    }
    return false;
}

bool narrator_short_answer(const char *reply, size_t length, const char *cue)
{
    while (length && isspace((unsigned char)*reply)) { ++reply; --length; }
    if (!length || length > 64) return false;
    size_t number = reply[0] == '+' || reply[0] == '-';
    while (number < length && isdigit((unsigned char)reply[number])) ++number;
    if (number == length) return isdigit((unsigned char)reply[length - 1]);
    if (number > 0 && number + 1 < length && reply[number] == '.' &&
        isdigit((unsigned char)reply[number - 1])) {
        for (++number; number < length && isdigit((unsigned char)reply[number]); ++number) {}
        if (number == length) return true;
    }
    if (!asks_for_code(cue)) return false;
    bool alnum = false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)reply[i];
        if ((c >= 'A' && c <= 'Z') || isdigit(c)) alnum = true;
        else if (c != ' ' && c != '-' && c != '_') return false;
    }
    return alnum;
}
