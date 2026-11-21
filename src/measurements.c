#include "app.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum { UNKNOWN_UNIT, SECONDS, LITERS, METERS, LITERS_PER_SECOND, MILLISECONDS, DECIBELS,
    CUBIC_METERS, PERCENT } Unit;

static bool token(const char *text, const char *word)
{
    size_t n = strlen(word);
    return !strncmp(text, word, n) && !isalnum((unsigned char)text[n]) && text[n] != '_';
}

static const char *space(const char *text)
{
    while (isspace((unsigned char)*text)) ++text;
    return text;
}

static bool mentions(const char *text, const char *word)
{
    for (const char *p = text; (p = strstr(p, word)); ++p)
        if ((p == text || !isalnum((unsigned char)p[-1])) && token(p, word)) return true;
    return false;
}

static bool quantity(const char *text, double *value, double *rounding, const char **end)
{
    static const char *const words[] = {"zero", "one", "two", "three", "four", "five", "six",
        "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
        "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};
    static const char *const tens[] = {"twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    *rounding = 0.5;
    bool unicode_minus = !strncmp(text, "−", sizeof "−" - 1);
    if (unicode_minus) text += sizeof "−" - 1;
    if (isdigit((unsigned char)*text) || *text == '-' || *text == '+' || *text == '.') {
        char *stop;
        *value = strtod(text, &stop);
        if (stop == text || !isfinite(*value)) return false;
        if (unicode_minus) *value = -*value;
        int decimals = 0;
        const char *dot = memchr(text, '.', (size_t)(stop - text));
        for (const char *p = dot ? dot + 1 : stop; p < stop && isdigit((unsigned char)*p); ++p)
            ++decimals;
        *rounding *= pow(10, -decimals);
        *end = stop;
        return true;
    }
    if (unicode_minus) return false;
    for (size_t i = 0; i < sizeof tens / sizeof *tens; ++i) {
        if (!token(text, tens[i])) continue;
        const char *tail = text + strlen(tens[i]);
        const char *units = space(*tail == '-' ? tail + 1 : tail);
        *value = 20 + 10 * (double)i;
        *end = tail;
        for (int j = 1; j < 10; ++j) {
            if (!token(units, words[j])) continue;
            *value += j;
            *end = units + strlen(words[j]);
            break;
        }
        return true;
    }
    for (size_t i = 0; i < sizeof words / sizeof *words; ++i) {
        if (!token(text, words[i])) continue;
        *value = (double)i;
        *end = text + strlen(words[i]);
        return true;
    }
    return false;
}

static Unit measurement_unit(const char *end)
{
    const char *unit = space(*end == '-' ? end + 1 : end);
    if (token(unit, "l/s") || token(unit, "liters per second") || token(unit, "litres per second") ||
        token(unit, "liter per second") || token(unit, "litre per second")) return LITERS_PER_SECOND;
    if (token(unit, "seconds") || token(unit, "second") || token(unit, "s")) return SECONDS;
    if (token(unit, "liters") || token(unit, "litres") || token(unit, "liter") ||
        token(unit, "litre") || token(unit, "l")) return LITERS;
    if (token(unit, "cubic meter") || token(unit, "cubic meters") ||
        token(unit, "cubic metre") || token(unit, "cubic metres") ||
        token(unit, "m3") || !strncmp(unit, "m³", sizeof "m³" - 1)) return CUBIC_METERS;
    if (*unit == '%' || token(unit, "percent")) return PERCENT;
    if (token(unit, "meters") || token(unit, "metres") || token(unit, "meter") ||
        token(unit, "metre") || token(unit, "m")) return METERS;
    if (token(unit, "milliseconds") || token(unit, "millisecond") || token(unit, "ms")) return MILLISECONDS;
    if (token(unit, "decibels") || token(unit, "decibel") || token(unit, "db")) return DECIBELS;
    return UNKNOWN_UNIT;
}

static bool close_reading(double written, double measured, double rounding)
{
    return fabs(written - measured) <= rounding + fabs(measured) * 0.00005 + 1e-10;
}

typedef enum { ANY_ARRIVAL, DIRECT_ARRIVAL, WALL_ECHO, FIRST_WALL_ECHO } ArrivalReference;

static ArrivalReference arrival_reference(const char *start, const char *number, bool direct, bool echo)
{
    const char *last_direct = nullptr, *last_echo = nullptr;
    bool first = false;
    for (const char *p = start; p < number; ++p) {
        if (p != start && isalnum((unsigned char)p[-1])) continue;
        if (token(p, "direct")) last_direct = p;
        if (token(p, "first") || token(p, "earliest")) first = true;
        if (token(p, "echo") || token(p, "reflection")) last_echo = p;
    }
    if (last_direct && (!last_echo || last_direct > last_echo)) return DIRECT_ARRIVAL;
    if (last_echo || (echo && !direct)) return first ? FIRST_WALL_ECHO : WALL_ECHO;
    return direct && !echo ? DIRECT_ARRIVAL : ANY_ARRIVAL;
}

static int earliest_echo(const PhysicsReading *r)
{
    int first = -1;
    for (int i = 1; i < r->arrivals; ++i)
        if (!r->arrival[i].blocked && (first < 0 || r->arrival[i].delay_ms < r->arrival[first].delay_ms)) first = i;
    return first;
}

static bool remaining_volume(const char *start, const char *number, const char *end)
{
    bool remaining = false;
    for (const char *p = start; p < number; ++p) {
        if (p != start && isalnum((unsigned char)p[-1])) continue;
        if (token(p, "holds") || token(p, "holding") || token(p, "contains") ||
            token(p, "remaining") || token(p, "left")) remaining = true;
        if (token(p, "lost") || token(p, "drained") || token(p, "drainage") || token(p, "discharged"))
            remaining = false;
    }
    const char *after = space(end);
    while (isalpha((unsigned char)*after)) ++after;
    after = space(after);
    return remaining || token(after, "left") || token(after, "remaining");
}

typedef enum { NO_TOPIC, DRAIN_TOPIC, SOUND_TOPIC } MeasurementTopic;

static MeasurementTopic topic_at(const char *p)
{
    if (token(p, "tank") || token(p, "drain") || token(p, "drained") ||
        token(p, "drainage") || token(p, "outlet") || token(p, "flow") || token(p, "water"))
        return DRAIN_TOPIC;
    if (token(p, "direct") || token(p, "echo") || token(p, "echoes") ||
        token(p, "reflection") || token(p, "acoustic") || token(p, "sound"))
        return SOUND_TOPIC;
    return NO_TOPIC;
}

static MeasurementTopic seconds_topic(const char *sentence, const char *number, const char *end)
{
    MeasurementTopic topic = NO_TOPIC;
    for (const char *p = sentence; p < number; ++p) {
        if (p != sentence && isalnum((unsigned char)p[-1])) continue;
        MeasurementTopic found = topic_at(p);
        if (found != NO_TOPIC) topic = found;
    }
    if (topic != NO_TOPIC) return topic;
    /* In "over ten seconds the tank drained", the subject follows the reading. */
    for (const char *p = end; *p; ++p) {
        if (isalnum((unsigned char)p[-1])) continue;
        topic = topic_at(p);
        if (topic != NO_TOPIC) break;
    }
    return topic;
}

static bool sentence_matches(const PhysicsReading *readings, int count, const char *sentence)
{
    bool has_fluid = false, has_acoustic = false;
    for (int i = 0; i < count; ++i) {
        has_fluid |= readings[i].kind == READING_FLUID;
        has_acoustic |= readings[i].kind == READING_ACOUSTIC;
    }
    const bool fluid = strstr(sentence, "drain") || strstr(sentence, "tank") ||
        strstr(sentence, "outlet") || strstr(sentence, "water level") || strstr(sentence, "flow") ||
        strstr(sentence, "simulation") || strstr(sentence, "took") || strstr(sentence, "elapsed");
    const bool level = strstr(sentence, "level") || strstr(sentence, "depth") || strstr(sentence, "remaining");
    if (strstr(sentence, "tank emptied") || strstr(sentence, "tank is empty") ||
        strstr(sentence, "tank was empty") || strstr(sentence, "tank ran dry")) {
        bool empty = false;
        for (int i = 0; i < count; ++i)
            if (readings[i].kind == READING_FLUID && readings[i].remaining_l == 0) empty = true;
        if (has_fluid && !empty) return false;
    }
    const bool direct = mentions(sentence, "direct");
    const bool echo = mentions(sentence, "echo") || mentions(sentence, "echoes") ||
                      mentions(sentence, "reflection") || mentions(sentence, "reflections");
    if (strstr(sentence, "flow accelerating") || strstr(sentence, "flow was accelerating") ||
        strstr(sentence, "flow is accelerating") || strstr(sentence, "flow increased") ||
        strstr(sentence, "flow increases") || strstr(sentence, "flow sped up")) {
        bool measured = false, compatible = false;
        for (int i = 0; i < count; ++i) {
            if (readings[i].kind != READING_FLUID) continue;
            measured = true;
            compatible |= readings[i].flow_lps > readings[i].initial_flow_lps;
        }
        if (measured && !compatible) return false;
    }
    const bool direct_blocked = strstr(sentence, "direct sound was blocked") || strstr(sentence, "direct path was blocked") ||
        strstr(sentence, "direct sound is blocked") || strstr(sentence, "direct path is blocked") ||
        strstr(sentence, "direct path blocked") || strstr(sentence, "direct sound never reached");
    const bool direct_clear = strstr(sentence, "direct path was clear") || strstr(sentence, "direct path is clear") ||
        strstr(sentence, "direct sound reached") || strstr(sentence, "direct sound reaches") ||
        strstr(sentence, "direct sound was audible") || strstr(sentence, "direct sound arrives") ||
        strstr(sentence, "direct sound arrived") || strstr(sentence, "direct sound is unobstructed") ||
        strstr(sentence, "direct path is not blocked") || strstr(sentence, "direct sound is not blocked");
    if (direct_blocked || direct_clear) {
        bool have_acoustic = false, compatible = false;
        for (int i = 0; i < count; ++i) {
            if (readings[i].kind != READING_ACOUSTIC) continue;
            have_acoustic = true;
            compatible |= readings[i].arrival[0].blocked == direct_blocked;
        }
        if (have_acoustic && (!compatible || (direct_blocked && direct_clear))) return false;
    }
    for (const char *p = sentence; *p; ++p) {
        if (p > sentence && (isalnum((unsigned char)p[-1]) || p[-1] == '.')) continue;
        double value, rounding;
        const char *end;
        if (!quantity(p, &value, &rounding, &end)) continue;
        const char *number = p;
        p = end - 1;
        Unit unit = measurement_unit(end);
        MeasurementTopic seconds = NO_TOPIC;
        if (unit == SECONDS) {
            if (fluid && (direct || echo)) seconds = seconds_topic(sentence, number, end);
            else if (fluid) seconds = DRAIN_TOPIC;
            else if (direct || echo) seconds = SOUND_TOPIC;
        }
        /* Do not pass a measurement copied from the other simulator off as
           the current result. A reference explicitly set in the past can
           still appear as part of the story. */
        bool prior = strstr(sentence, "earlier") || strstr(sentence, "previous");
        if (!prior && has_acoustic && !has_fluid &&
            (unit == LITERS || unit == CUBIC_METERS || unit == LITERS_PER_SECOND ||
             seconds == DRAIN_TOPIC)) return false;
        if (!prior && has_fluid && !has_acoustic && (direct || echo) &&
            (unit == MILLISECONDS || unit == DECIBELS)) return false;
        bool have_reading = false, matches = false;
        for (int i = 0; i < count; ++i) {
            const PhysicsReading *r = readings + i;
            if (r->kind == READING_FLUID && fluid) {
                double expected;
                if (unit == SECONDS && seconds == DRAIN_TOPIC) expected = r->seconds;
                else if (unit == LITERS || unit == CUBIC_METERS) {
                    expected = remaining_volume(sentence, number, end) ? r->remaining_l : r->drained_l;
                    if (unit == CUBIC_METERS) expected /= 1000;
                }
                else if (unit == LITERS_PER_SECOND) expected = r->flow_lps;
                else if (unit == METERS && level) expected = r->level_m;
                else if (unit == PERCENT && (strstr(sentence, "full") || strstr(sentence, "capacity"))) {
                    /* The drain tool does not receive the tank's usable capacity. */
                    have_reading = true;
                    continue;
                }
                else continue;
                have_reading = true;
                matches |= close_reading(value, expected, rounding);
            } else if (r->kind == READING_ACOUSTIC &&
                       (unit == MILLISECONDS || unit == DECIBELS || seconds == SOUND_TOPIC)) {
                have_reading = true;
                ArrivalReference reference = arrival_reference(sentence, number, direct, echo);
                int first = reference == WALL_ECHO ? 1 : 0;
                int last = reference == DIRECT_ARRIVAL ? 1 : r->arrivals;
                if (reference == FIRST_WALL_ECHO) {
                    first = earliest_echo(r);
                    if (first < 0) continue;
                    last = first + 1;
                }
                double scale = unit == SECONDS ? 1000 : 1;
                for (int j = first; j < last; ++j) {
                    if (r->arrival[j].blocked) continue;
                    double expected = unit == DECIBELS ? r->arrival[j].level_db : r->arrival[j].delay_ms;
                    matches |= close_reading(value * scale, expected, rounding * scale);
                }
            }
        }
        if (have_reading && !matches) return false;
    }
    return true;
}

bool physics_prose_matches(const PhysicsReading *readings, int count, const char *body)
{
    if (!count) return true;
    Text sentence = {0};
    bool ok = true;
    for (const char *p = body; ok; ++p) {
        if (!*p || *p == '\n' || *p == '!' || *p == '?' ||
            (*p == '.' && !isdigit((unsigned char)p[1]))) {
            ok = sentence_matches(readings, count, str(&sentence));
            clear(&sentence);
            if (!*p) break;
        } else {
            char lower = (char)tolower((unsigned char)*p);
            append_n(&sentence, &lower, 1);
        }
    }
    release(&sentence);
    return ok;
}

void physics_prose_readouts(const PhysicsReading *readings, int count, Text *body)
{
    for (int i = 0; i < count; ++i) {
        const PhysicsReading *r = readings + i;
        if (r->kind == READING_FLUID) {
            format(body, "\n\nThe drain estimate gives %.5g liters over %.9g seconds, with a final water level "
                         "of %.5g meters and a final flow of %.5g liters per second.",
                   r->drained_l, r->seconds, r->level_m, r->flow_lps);
        } else if (r->kind == READING_ACOUSTIC) {
            if (r->arrival[0].faded) append(body, "\n\nThe direct sound falls below the modeled audibility cutoff.");
            else if (r->arrival[0].blocked) append(body, "\n\nThe acoustic reading shows that the direct path is blocked.");
            else format(body, "\n\nThe acoustic reading puts the direct arrival at %.4g milliseconds and %+.3g decibels "
                              "relative to the source at one meter.", r->arrival[0].delay_ms, r->arrival[0].level_db);
            int first_echo = earliest_echo(r);
            const char *echo = r->material_acoustics ? "modeled reflection" : "audible wall echo";
            if (first_echo >= 0)
                format(body, " The first %s arrives at %.4g milliseconds and %+.3g decibels.",
                       echo, r->arrival[first_echo].delay_ms, r->arrival[first_echo].level_db);
            else if (r->material_acoustics) append(body, " No modeled reflection reaches the listener above the cutoff.");
            else append(body, " The reading shows no wall echo reaching the listener.");
        }
    }
}
