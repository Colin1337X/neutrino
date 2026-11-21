#include "neutrino.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Text subject, object, place, role;
    const char *action, *relation;
    bool past;
    size_t role_end;
} Claim;

static bool prefix(const char **s, const char *words)
{
    size_t n = strlen(words);
    if (strncmp(*s, words, n)) return false;
    *s += n;
    return true;
}
static const char *word(const char *s, const char *target)
{
    size_t n = strlen(target);
    for (const char *p = s; (p = strstr(p, target)) != NULL; p++)
        if ((p == s || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return p;
    return NULL;
}
static bool simple(const char *s)
{
    static const char *const qualifiers[] = {"and", "or", "but", "not", "never", "no",
        "if", "unless", "until", "before", "after", "again", "still", "formerly",
        "yesterday", "tomorrow", "probably", "possibly", "maybe", "allegedly", "near",
        "now", "soon", "later", "without", "only", "might", "may", "will", "would",
        "could", "should", "must", "seems", "apparently"};
    for (size_t i = 0; i < sizeof qualifiers / sizeof *qualifiers; i++)
        if (word(s, qualifiers[i])) return false;
    return *s != 0;
}
static void phrase(Text *out, const char *s, const char *end)
{
    while (s < end && *s == ' ') s++;
    while (end > s && end[-1] == ' ') end--;
    const char *start = s;
    if (end - s > 4 && prefix(&start, "the ")) s = start;
    append_n(out, s, (size_t)(end - s));
}
static bool title_is(const char *title, const char *left, const char *right)
{
    Text name = {0};
    phrase(&name, left, right);
    bool same = !strcmp(title, str(&name));
    release(&name);
    return same;
}
static void noun_phrase(Text *out, const char *start, const char *end)
{
    while (start < end && *start == ' ') start++;
    if (end - start > 2 && !memcmp(start, "a ", 2)) start += 2;
    else if (end - start > 3 && !memcmp(start, "an ", 3)) start += 3;
    phrase(out, start, end);
}

static const char *action_at(const char *s, const char **verb, const char **action)
{
    static const struct { const char *word, *action; } predicates[] = {
        {"stranded", "stranded"}, {"marooned", "stranded"},
        {"hiding", "hidden"}, {"hides", "hidden"}, {"conceals", "hidden"},
        {"concealing", "hidden"}, {"hidden", "concealed"}, {"stores", "stored"}, {"storing", "stored"},
        {"finds", "found"}, {"discovered", "found"}, {"found", "found"},
        {"located", "located"}
    };
    for (const char *p = s; *p; p++) {
        if (p != s && p[-1] != ' ') continue;
        for (size_t i = 0; i < sizeof predicates / sizeof *predicates; i++) {
            size_t n = strlen(predicates[i].word);
            if (!strncmp(p, predicates[i].word, n) && (!p[n] || p[n] == ' ')) {
                *verb = predicates[i].word;
                *action = predicates[i].action;
                return p;
            }
        }
        const char *const copulas[] = {"is", "are", "was", "were"};
        for (size_t i = 0; i < sizeof copulas / sizeof *copulas; i++) {
            size_t n = strlen(copulas[i]);
            if (strncmp(p, copulas[i], n) || p[n] != ' ') continue;
            const char *where = p + n + 1;
            if (strncmp(where, "in ", 3) && strncmp(where, "inside ", 7) &&
                strncmp(where, "within ", 7) && strncmp(where, "on ", 3) &&
                strncmp(where, "at ", 3)) continue;
            *verb = copulas[i];
            *action = "located";
            return p;
        }
    }
    return NULL;
}

static const char *subject_end(const char *start, const char *end, bool *past)
{
    static const struct { const char *suffix; bool past; } auxiliaries[] = {
        {" is currently ", false}, {" was currently ", true},
        {" is ", false}, {" are ", false}, {" was ", true}, {" were ", true},
        {" currently ", false}
    };
    if (start == end) return end;
    for (size_t i = 0; i < sizeof auxiliaries / sizeof *auxiliaries; i++) {
        size_t n = strlen(auxiliaries[i].suffix);
        if ((size_t)(end - start) >= n && !memcmp(end - n, auxiliaries[i].suffix, n)) {
            *past = auxiliaries[i].past;
            return end - n;
        }
    }
    return end;
}

static const char *location_at(const char *suffix, const char **place, const char **relation,
                               const char *action)
{
    static const struct { const char *word, *relation; } prepositions[] = {
        {"inside ", "in"}, {"within ", "in"}, {"in ", "in"},
        {"on ", "on"}, {"upon ", "on"}, {"at ", "at"}
    };
    for (const char *p = suffix; *p; p++) {
        if (p != suffix && p[-1] != ' ') continue;
        for (size_t i = 0; i < sizeof prepositions / sizeof *prepositions; i++) {
            const char *value = p;
            if (!prefix(&value, prepositions[i].word) || !*value) continue;
            *relation = prepositions[i].relation;
            *place = value;
            /* Ordinary at/on descriptions use the same location for stranded people. */
            if (!strcmp(action, "stranded") &&
                (!strcmp(*relation, "on") || !strcmp(*relation, "at"))) *relation = "at";
            return p;
        }
    }
    return NULL;
}

static void claim_release(Claim *c)
{
    release(&c->subject);
    release(&c->object);
    release(&c->place);
    release(&c->role);
}

/* Interpret a narrow set of explicit state and location frames. Unknown words
   remain part of the claim; a near match never licenses deleting a fact. */
static bool parse_claim(Claim *c, const char *type, const char *name, const char *text, bool ledger)
{
    Text words = {0}, title = {0};
    normalize_words(&words, text);
    normalize_words(&title, name);
    const char *s = str(&words), *normalized_title = str(&title);
    bool ok = false;
    /* An explicit purpose in an existing filing can entail the unqualified
       concealment. Keep purpose clauses intact in newly proposed filings. */
    char *purpose = ledger && words.data ? strstr(words.data, " to prevent ") : NULL;
    if (purpose) *purpose = 0;
    if (!simple(s) || words.len > 350) goto finish;
    const char *clause = s, *implicit_place = NULL, *place_end = NULL;
    const char *where = word(s, "where");
    if (where && where[5] == ' ' && !strcmp(type, "place") && title.len) {
        const char *locator = s;
        prefix(&locator, "the ");
        if (where == s || title_is(normalized_title, s, where) ||
            title_is("location", locator, where) || title_is("place", locator, where)) {
            implicit_place = normalized_title;
            place_end = normalized_title + title.len;
            clause = where + sizeof "where " - 1;
        }
    }
    const char *verb = NULL, *action = NULL, *predicate = action_at(clause, &verb, &action);
    if (!predicate) goto finish;
    bool past = false;
    const char *end_subject = subject_end(clause, predicate, &past);
    phrase(&c->subject, clause, end_subject);
    if (title.len && !implicit_place) {
        bool character = !strcmp(type, "character");
        if (!c->subject.len && (character || !strcmp(type, "item"))) append(&c->subject, normalized_title);
        else if (character && c->subject.len) {
            if (c->subject.len > 4 && !strcmp(c->subject.data + c->subject.len - 4, " who")) {
                c->subject.len -= 4;
                c->subject.data[c->subject.len] = 0;
                end_subject = word(clause, "who");
            }
            const char *role = str(&c->subject);
            /* A bare role before a participle is extra information about the titled character. */
            const char *role_suffix = strrchr(role, ' ');
            if (!strcmp(action, "hidden") && role_suffix &&
                (!strcmp(role_suffix, " keeper") || !strcmp(role_suffix, " guard") ||
                 !strcmp(role_suffix, " officer") || !strcmp(role_suffix, " captain"))) {
                while (end_subject > clause && end_subject[-1] == ' ') end_subject--;
                c->role_end = (size_t)(end_subject - s);
                noun_phrase(&c->role, clause, end_subject);
                clear(&c->subject);
                append(&c->subject, normalized_title);
            }
        }
    }
    if (!c->subject.len) goto finish;
    const char *suffix = predicate + strlen(verb);
    if (*suffix == ' ') suffix++;
    const char *location = implicit_place, *relation = !strcmp(action, "stranded") ? "at" : "in";
    const char *explicit_place = NULL, *explicit_relation = NULL;
    const char *prep = location_at(suffix, &explicit_place, &explicit_relation, action);
    if (prep) {
        if (implicit_place && strcmp(implicit_place, explicit_place)) goto finish;
        location = explicit_place;
        place_end = explicit_place + strlen(explicit_place);
        relation = explicit_relation;
        noun_phrase(&c->object, suffix, prep);
    } else if (implicit_place) {
        noun_phrase(&c->object, suffix, suffix + strlen(suffix));
    } else goto finish;
    noun_phrase(&c->place, location, place_end);
    if (!c->place.len) goto finish;
    if ((!strcmp(action, "stranded") || !strcmp(action, "located") || !strcmp(action, "concealed")) && c->object.len) goto finish;
    if ((!strcmp(action, "hidden") || !strcmp(action, "stored")) && !c->object.len) goto finish;
    if (purpose && strcmp(action, "hidden") && strcmp(action, "concealed")) goto finish;
    c->action = action;
    c->relation = relation;
    c->past = past || !strcmp(verb, "was") || !strcmp(verb, "were");
    ok = true;
finish:
    release(&words);
    release(&title);
    return ok;
}

static bool covered_claim(const Claim *candidate, const Claim *known)
{
    if (candidate->past != known->past || strcmp(candidate->relation, known->relation) ||
        strcmp(str(&candidate->place), str(&known->place))) return false;
    if (!strcmp(candidate->action, known->action) &&
        !strcmp(str(&candidate->subject), str(&known->subject)) &&
        !strcmp(str(&candidate->object), str(&known->object))) return true;
    bool hides = !strcmp(known->action, "hidden"), stores = !strcmp(known->action, "stored");
    if (hides || stores) {
        return (!strcmp(candidate->action, "located") || (hides && !strcmp(candidate->action, "concealed"))) &&
               !strcmp(str(&candidate->subject), str(&known->object));
    }
    return !strcmp(known->action, "concealed") && !strcmp(candidate->action, "located") &&
           !strcmp(str(&candidate->subject), str(&known->subject));
}

static const char *located_subject(const Claim *c)
{
    if (!strcmp(c->action, "hidden") || !strcmp(c->action, "stored")) return str(&c->object);
    if (!strcmp(c->action, "located") || !strcmp(c->action, "concealed") ||
        !strcmp(c->action, "stranded")) return str(&c->subject);
    return "";
}

/* Keep original wording for every unmatched sentence, including its qualifiers.
   Treat strong sentence boundaries only; an unparsed compound stays intact. */
static const char *sentence_end(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p == '\n') return p + 1;
        if ((*p == '.' || *p == '!' || *p == '?') && p[1]) {
            const char *next = p + 1;
            while (*next == ' ' || *next == '\t') next++;
            if (next > p + 1 && isupper((unsigned char)*next) &&
                !(p > s && isupper((unsigned char)p[-1]) &&
                  (p - s == 1 || p[-2] == ' '))) return next;
        }
    }
    return s + strlen(s);
}
static void copy_sentence(Text *out, const char *start, const char *end)
{
    clear(out);
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    append_n(out, start, (size_t)(end - start));
}

static void compare_ledger(const Json *entries, const Claim *candidate, bool *matched, bool *conflict,
                           bool *known_role)
{
    Json *item;
    Text sentence = {0};
    const char *subject = located_subject(candidate);
    cJSON_ArrayForEach(item, entries) {
        const char *description = js(item, "description");
        for (const char *s = description; *s;) {
            const char *end = sentence_end(s);
            copy_sentence(&sentence, s, end);
            Claim known = {0};
            if (parse_claim(&known, js(item, "type"), js(item, "name"), str(&sentence), true)) {
                if (candidate->role.len && !strcmp(str(&candidate->role), str(&known.role)) &&
                    !strcmp(str(&candidate->subject), str(&known.subject)) && candidate->past == known.past)
                    *known_role = true;
                if (covered_claim(candidate, &known)) *matched = true;
                /* When the ledger contains different locations for the same subject,
                   a repeated location may describe a return. Keep it. */
                else if (*subject && candidate->past == known.past &&
                         !strcmp(subject, located_subject(&known)) &&
                         strcmp(str(&candidate->place), str(&known.place))) *conflict = true;
            }
            claim_release(&known);
            s = end;
        }
    }
    release(&sentence);
}

static bool keep_role(Text *out, const char *text, size_t normalized_end)
{
    Text normalized = {0};
    for (const char *p = text; *p; p++) {
        if (*p != ' ') continue;
        clear(&normalized);
        append_n(&normalized, text, (size_t)(p - text));
        Text words = {0};
        normalize_words(&words, str(&normalized));
        bool found = words.len == normalized_end;
        release(&words);
        if (found) {
            if (out->len) append(out, "\n");
            append(out, str(&normalized));
            append(out, ".");
            release(&normalized);
            return true;
        }
    }
    release(&normalized);
    return false;
}

bool compendium_novel_claims(const Json *known, const Json *batch, const char *type,
                             const char *name, const char *description, Text *out)
{
    Text sentence = {0};
    bool changed = false, ambiguous = false;
    for (const char *s = description; *s;) {
        const char *end = sentence_end(s);
        copy_sentence(&sentence, s, end);
        Claim candidate = {0};
        bool matched = false, conflict = false, known_role = false;
        if (parse_claim(&candidate, type, name, str(&sentence), false)) {
            compare_ledger(known, &candidate, &matched, &conflict, &known_role);
            compare_ledger(batch, &candidate, &matched, &conflict, &known_role);
        }
        if (conflict) ambiguous = true;
        if (matched && !conflict && (!candidate.role_end || known_role ||
            keep_role(out, str(&sentence), candidate.role_end))) {
            changed = true;
        } else if (sentence.len) {
            if (out->len) append(out, "\n");
            append(out, str(&sentence));
        }
        claim_release(&candidate);
        s = end;
    }
    release(&sentence);
    if (!changed) {
        clear(out);
        append(out, description);
    }
    return ambiguous;
}

static bool mentions(const char *text, const char *name)
{
    Text words = {0};
    normalize_words(&words, text);
    bool found = word(str(&words), name) != NULL;
    release(&words);
    return found;
}

static bool known_prop(const Json *known, const Json *history, const char *name)
{
    Json *item;
    cJSON_ArrayForEach(item, known)
        if (mentions(js(item, "name"), name) || mentions(js(item, "description"), name)) return true;
    cJSON_ArrayForEach(item, history)
        if (!strcmp(js(item, "role"), "assistant") && mentions(js(item, "content"), name)) return true;
    return false;
}

static bool placement_object(const char *sentence, Text *name)
{
    /* A complete leading action is required. Keep the full sentence as evidence. */
    static const char *const verbs[] = {"places", "puts", "sets", "slides", "ties", "fastens", "secures"};
    static const char *const prepositions[] = {"into", "inside", "within", "onto", "on", "in", "to"};
    static const char *const vague[] = {"it", "them", "something", "anything", "everything", "nothing",
        "this", "that", "these", "those", "him", "her", "me", "us", "himself", "herself", "yourself"};
    Text words = {0}, full = {0};
    const char *comma = strchr(sentence, ',');
    Text first = {0};
    if (comma) append_n(&first, sentence, (size_t)(comma - sentence));
    normalize_words(&words, comma ? str(&first) : sentence);
    normalize_words(&full, sentence);
    const char *s = str(&words), *begin = NULL, *end = NULL;
    if (words.len > 250 || !simple(s) || word(str(&full), "or") || word(str(&full), "imagines") ||
        word(str(&full), "pretends") || word(str(&full), "perhaps") || word(str(&full), "hypothetically")) goto end;
    for (size_t i = 0; i < sizeof verbs / sizeof *verbs; i++) {
        const char *at = word(s, verbs[i]);
        if (!at || at == s || !at[strlen(verbs[i])]) continue;
        const char *object = at + strlen(verbs[i]) + 1;
        if (!begin || object < begin) begin = object;
    }
    if (!begin) goto end;
    for (size_t i = 0; i < sizeof prepositions / sizeof *prepositions; i++) {
        const char *at = word(begin, prepositions[i]);
        if (!at || at == begin || !at[strlen(prepositions[i])]) continue;
        if (!end || at < end) end = at;
    }
    if (!end) goto end;
    if (!prefix(&begin, "the ") && !prefix(&begin, "a ") && !prefix(&begin, "an ")) goto end;
    if (begin >= end) goto end;
    phrase(name, begin, end);
    for (size_t i = 0; i < sizeof vague / sizeof *vague; i++)
        if (!strcmp(str(name), vague[i])) clear(name);
    if (name->len > 100 || !strchr(str(name), ' ')) clear(name);
end:
    release(&words);
    release(&full);
    release(&first);
    return name->len != 0;
}

bool compendium_placement_evidence(const Json *known, const Json *history, const char *reply,
                                   Text *name, Text *evidence)
{
    Text sentence = {0};
    for (const char *s = reply; *s;) {
        const char *end = sentence_end(s);
        copy_sentence(&sentence, s, end);
        if (sentence.len <= 700 && placement_object(str(&sentence), name) &&
            !known_prop(known, history, str(name))) {
            append(evidence, str(&sentence));
            release(&sentence);
            return true;
        }
        clear(name);
        s = end;
    }
    release(&sentence);
    return false;
}
