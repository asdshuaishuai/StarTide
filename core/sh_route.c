/*
 * sh_route.c - the skill router (see sh_route.h).
 */
#include "sh_route.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define ROUTE_MAX_TOKENS 8
#define ROUTE_TOKEN_CAP 48
#define ROUTE_QUERY_CAP 256

typedef struct {
    char whole[ROUTE_QUERY_CAP];                    /* lowered, spaces collapsed */
    char tokens[ROUTE_MAX_TOKENS][ROUTE_TOKEN_CAP]; /* lowered, unique */
    size_t count;
} route_query;

static void lower_copy(const char *src, char *dst, size_t cap) {
    size_t i = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (; src[i] && i + 1 < cap; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

static void query_parse(const char *q, route_query *rq) {
    size_t out = 0;
    int pending_space = 0;

    memset(rq, 0, sizeof(*rq));
    for (size_t i = 0; q[i] && out + 1 < ROUTE_QUERY_CAP; i++) {
        unsigned char c = (unsigned char)q[i];
        if (isspace(c)) {
            pending_space = out > 0;
            continue;
        }
        if (pending_space) {
            rq->whole[out++] = ' ';
            pending_space = 0;
        }
        rq->whole[out++] = (char)tolower(c);
    }
    rq->whole[out] = '\0';

    const char *p = q;
    while (*p && rq->count < ROUTE_MAX_TOKENS) {
        char tok[ROUTE_TOKEN_CAP];
        size_t n = 0;
        int dup = 0;

        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        while (*p && !isspace((unsigned char)*p)) {
            if (n + 1 < ROUTE_TOKEN_CAP) tok[n++] = (char)tolower((unsigned char)*p);
            p++;
        }
        tok[n] = '\0';
        if (n == 0) continue;
        for (size_t t = 0; t < rq->count && !dup; t++)
            if (strcmp(rq->tokens[t], tok) == 0) dup = 1;
        if (!dup) memcpy(rq->tokens[rq->count++], tok, n + 1);
    }
}

static int contains_ci(const char *haystack_lower, const char *needle) {
    return haystack_lower && needle[0] && strstr(haystack_lower, needle) != NULL;
}

/*
 * Score one entry. Name evidence outweighs description evidence, which
 * outweighs path evidence; a stored unit and multiple sources add a
 * small tie-breaking bonus so the canonical copy wins.
 */
static long entry_score(const sh_catalog_entry *e, const route_query *rq) {
    char name[ROUTE_QUERY_CAP];
    char desc[2048];
    size_t name_hits = 0;
    size_t desc_hits = 0;
    long score = 0;

    lower_copy(e->name, name, sizeof(name));
    lower_copy(e->description, desc, sizeof(desc));

    if (rq->whole[0] && name[0]) {
        size_t wlen = strlen(rq->whole);
        if (strcmp(name, rq->whole) == 0)
            score += 1000;
        else if (strncmp(name, rq->whole, wlen) == 0)
            score += 400;
        else if (strstr(name, rq->whole))
            score += 250;
    }

    for (size_t t = 0; t < rq->count; t++) {
        const char *tok = rq->tokens[t];
        if (contains_ci(name, tok)) {
            score += 120;
            name_hits++;
        }
        if (contains_ci(desc, tok)) {
            score += 25;
            desc_hits++;
        }
        for (size_t s = 0; s < e->source_count; s++) {
            char path[1024];
            lower_copy(e->sources[s], path, sizeof(path));
            if (contains_ci(path, tok)) {
                score += 8;
                break;
            }
        }
    }

    /* Every word of a multi-word intent present somewhere is a real hit. */
    if (rq->count > 1 && name_hits + desc_hits >= rq->count) score += 60;

    if (score <= 0) return 0;
    if (e->in_store) score += 10;
    score += (long)(e->source_count > 5 ? 5 : e->source_count) * 4;
    return score;
}

static int entry_name_before(const sh_catalog *cat, size_t a, size_t b) {
    const char *na = cat->entries[a].name ? cat->entries[a].name : "";
    const char *nb = cat->entries[b].name ? cat->entries[b].name : "";
    return strcmp(na, nb) < 0;
}

size_t sh_route_search(const sh_catalog *cat, const char *query, size_t limit,
                       sh_route_hit **out) {
    route_query rq;
    sh_route_hit *hits;
    size_t n = 0;

    if (out) *out = NULL;
    if (!cat || !query || limit == 0) return 0;

    query_parse(query, &rq);
    if (rq.count == 0) return 0;

    hits = calloc(limit, sizeof(sh_route_hit));
    if (!hits) return 0;

    for (size_t i = 0; i < cat->count; i++) {
        long score = entry_score(&cat->entries[i], &rq);
        size_t inset;
        size_t pos;

        if (score <= 0) continue;

        /* Insert into the top-`limit` list: score desc, name asc. */
        for (inset = 0; inset < n; inset++) {
            if (score > hits[inset].score) break;
            if (score == hits[inset].score && entry_name_before(cat, i, hits[inset].index)) break;
        }
        if (inset >= limit) continue;
        pos = n < limit ? n : limit - 1;
        for (size_t k = pos; k > inset; k--) hits[k] = hits[k - 1];
        hits[inset].index = i;
        hits[inset].score = score;
        if (n < limit) n++;
    }

    *out = hits;
    return n;
}

void sh_route_hits_free(sh_route_hit *hits) { free(hits); }

int sh_route_resolve(const sh_catalog *cat, const char *needle, int *match_kind,
                     size_t *out_candidates) {
    char n[ROUTE_QUERY_CAP];
    int found = -1;
    size_t count = 0;

    if (match_kind) *match_kind = 0;
    if (out_candidates) *out_candidates = 0;
    if (!cat || !needle) return -1;

    lower_copy(needle, n, sizeof(n));
    if (!n[0]) return -1;

    /* 1. exact name; duplicate names are ambiguous by design (the same
     *    skill name commonly ships from several roots with different
     *    content, so the caller must pick by id). */
    for (size_t i = 0; i < cat->count; i++) {
        char name[ROUTE_QUERY_CAP];
        lower_copy(cat->entries[i].name, name, sizeof(name));
        if (name[0] && strcmp(name, n) == 0) {
            if (found < 0) found = (int)i;
            count++;
        }
    }
    if (found >= 0) {
        if (out_candidates) *out_candidates = count;
        if (count == 1) {
            if (match_kind) *match_kind = 1;
            return found;
        }
        return -1;
    }

    /* 2. unique id prefix */
    for (size_t i = 0; i < cat->count; i++) {
        const char *id = cat->entries[i].id;
        if (id && strncmp(id, n, strlen(n)) == 0) {
            if (found < 0) found = (int)i;
            count++;
        }
    }
    if (found >= 0) {
        if (out_candidates) *out_candidates = count;
        if (count == 1) {
            if (match_kind) *match_kind = 2;
            return found;
        }
        return -1;
    }

    /* 3. unique name prefix */
    found = -1;
    count = 0;
    for (size_t i = 0; i < cat->count; i++) {
        char name[ROUTE_QUERY_CAP];
        lower_copy(cat->entries[i].name, name, sizeof(name));
        if (name[0] && strncmp(name, n, strlen(n)) == 0) {
            if (found < 0) found = (int)i;
            count++;
        }
    }
    if (count == 1) {
        if (match_kind) *match_kind = 3;
        if (out_candidates) *out_candidates = 1;
        return found;
    }
    if (count > 1) {
        if (out_candidates) *out_candidates = count;
        return -1;
    }

    /* 4. substring: the best-ranked entry must beat the runner-up. */
    {
        sh_route_hit *hits = NULL;
        size_t got = sh_route_search(cat, needle, 2, &hits);
        int result = -1;

        if (got >= 1 && (got == 1 || hits[0].score > hits[1].score)) {
            result = (int)hits[0].index;
            if (match_kind) *match_kind = 4;
            if (out_candidates) *out_candidates = 1;
        } else if (got >= 2) {
            if (out_candidates) *out_candidates = got;
        }
        sh_route_hits_free(hits);
        return result;
    }
}
