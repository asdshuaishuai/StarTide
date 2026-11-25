/*
 * sh_text.c - tokenisation, deterministic enrichment and near-duplicate
 * detection (see sh_text.h).
 */
#include "sh_text.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ stopwords */

/* 61 core English function words.  Everything longer than six characters
 * is known not to be on the list, which keeps the linear scan cheap. */
static const char *const EN_STOP[] = {
    "the",  "and",  "or",   "of",    "to",     "in",    "on",      "at",      "by",
    "for",  "with", "from", "is",    "are",    "was",   "were",    "be",      "been",
    "being", "have", "has",  "had",   "do",     "does",  "did",     "will",    "would",
    "can",  "could", "should", "may", "might",  "must",  "this",    "that",    "these",
    "those", "it",  "its",  "as",    "if",     "then",  "than",    "so",      "such",
    "not",  "no",   "but",  "also",  "into",   "over",  "under",   "about",   "after",
    "before", "when", "where", "which", "who", "how",   "why"
};

#define EN_STOP_COUNT (sizeof(EN_STOP) / sizeof(EN_STOP[0]))

/* The handful of Chinese particles that appear everywhere and discriminate
 * nothing. */
static const uint32_t CJK_STOP[] = {
    0x7684, /* 的 */
    0x4E86, /* 了 */
    0x662F, /* 是 */
    0x548C, /* 和 */
    0x5728, /* 在 */
    0x6211, /* 我 */
    0x4E0E, /* 与 */
    0x53CA  /* 及 */
};

#define CJK_STOP_COUNT (sizeof(CJK_STOP) / sizeof(CJK_STOP[0]))

static int cjk_stop_cp(uint32_t cp) {
    for (size_t i = 0; i < CJK_STOP_COUNT; i++)
        if (CJK_STOP[i] == cp) return 1;
    return 0;
}

int sh_text_is_stopword(const char *s, size_t n) {
    if (!s || n == 0) return 0;
    if (n == 1) {
        unsigned char c = (unsigned char)s[0];
        /* Single ASCII characters are dropped by the caller anyway; report
         * them as stopwords so the predicate agrees with the tokeniser. */
        return c < 0x80;
    }
    if (n > 6) return 0;
    for (size_t i = 0; i < EN_STOP_COUNT; i++) {
        if (strlen(EN_STOP[i]) == n && memcmp(EN_STOP[i], s, n) == 0) return 1;
    }
    /* A single CJK character (3 bytes in UTF-8 for the BMP range). */
    if (n >= 3) {
        uint32_t cp = 0;
        size_t adv = 0;
        const unsigned char *p = (const unsigned char *)s;
        if (n >= 3 && (p[0] & 0xF0) == 0xE0) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                 (uint32_t)(p[2] & 0x3F);
            adv = 3;
        }
        if (adv == n && cjk_stop_cp(cp)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------- tokeniser */

static int is_ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Characters that end a code identifier: they never carry meaning inside
 * one, and splitting on them lets `godot_export` and `godot export`
 * index the same way. */
static int is_ident_sep(unsigned char c) {
    return c == '_' || c == '-' || c == '.' || c == '/' || c == ':';
}

static uint32_t lower_ascii(uint32_t c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int is_cjk_cp(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) ||   /* kana */
           (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK ext A */
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK unified */
           (cp >= 0xAC00 && cp <= 0xD7AF) ||   /* hangul syllables */
           (cp >= 0xF900 && cp <= 0xFAFF);     /* CJK compatibility */
}

/* Decode one UTF-8 code point.  Invalid input yields cp 0 and advance 1. */
static size_t utf8_next(const char *s, size_t len, size_t i, uint32_t *cp) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char c = p[i];
    size_t n;
    uint32_t v;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        v = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        v = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        v = c & 0x07;
    } else {
        *cp = 0;
        return 1;
    }
    if (i + n > len) {
        *cp = 0;
        return 1;
    }
    for (size_t k = 1; k < n; k++) {
        unsigned char cc = p[i + k];
        if ((cc & 0xC0) != 0x80) {
            *cp = 0;
            return 1;
        }
        v = (v << 6) | (uint32_t)(cc & 0x3F);
    }
    *cp = v;
    return n;
}

int sh_text_tokens_push(sh_text_tokens *t, const char *s, size_t n) {
    char *copy;

    if (!t || n == 0) return 1;
    if (t->count == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 16;
        char **items = realloc(t->items, cap * sizeof(char *));
        if (!items) return 0;
        t->items = items;
        t->cap = cap;
    }
    copy = malloc(n + 1);
    if (!copy) return 0;
    memcpy(copy, s, n);
    copy[n] = '\0';
    t->items[t->count++] = copy;
    return 1;
}

void sh_text_tokens_free(sh_text_tokens *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) free(t->items[i]);
    free(t->items);
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
}

/* Emit an ASCII token: length-1 strings and stopwords are dropped. */
static int emit_ascii(sh_text_tokens *out, const char *s, size_t n) {
    if (n < 2) return 1;
    if (sh_text_is_stopword(s, n)) return 1;
    return sh_text_tokens_push(out, s, n);
}

int sh_text_tokenize(const char *text, size_t len, sh_text_tokens *out) {
    char tok[SH_TEXT_MAX_TOKEN];
    size_t tok_len = 0;
    char cjk_buf[4];
    size_t cjk_len = 0;
    uint32_t cjk_cp = 0;
    size_t cjk_run = 0;
    size_t i = 0;
    int rc = 1;

    if (!out) return 0;
    if (!text) len = 0;

    for (i = 0; i < len && rc; ) {
        unsigned char c = (unsigned char)text[i];

        if (c < 0x80) {
            /* End any CJK run that was in progress. */
            if (cjk_run > 0) {
                if (cjk_run == 1 && !cjk_stop_cp(cjk_cp))
                    rc = sh_text_tokens_push(out, cjk_buf, cjk_len);
                cjk_run = 0;
            }
            if (is_ascii_alnum(c)) {
                if (tok_len + 1 < sizeof(tok)) tok[tok_len++] = (char)lower_ascii(c);
                i++;
                continue;
            }
            if (tok_len > 0) {
                rc = emit_ascii(out, tok, tok_len);
                tok_len = 0;
            }
            /* Whitespace, punctuation and code separators all end a token;
             * the separators simply let the next run start a new one. */
            i++;
            continue;
        }

        {
            uint32_t cp = 0;
            size_t adv = utf8_next(text, len, i, &cp);

            if (adv > 1 && is_cjk_cp(cp)) {
                if (tok_len > 0) {
                    rc = emit_ascii(out, tok, tok_len);
                    tok_len = 0;
                }
                if (cjk_run > 0) {
                    /* Overlapping bigram: previous character + this one. */
                    if (!(cjk_stop_cp(cjk_cp) && cjk_stop_cp(cp))) {
                        char big[8];
                        memcpy(big, cjk_buf, cjk_len);
                        memcpy(big + cjk_len, text + i, adv);
                        rc = rc && sh_text_tokens_push(out, big, cjk_len + adv);
                    }
                }
                cjk_cp = cp;
                cjk_len = adv < sizeof(cjk_buf) ? adv : sizeof(cjk_buf);
                memcpy(cjk_buf, text + i, cjk_len);
                cjk_run++;
                i += adv;
                continue;
            }

            /* Any other multi-byte character separates runs. */
            if (cjk_run > 0) {
                if (cjk_run == 1 && !cjk_stop_cp(cjk_cp))
                    rc = rc && sh_text_tokens_push(out, cjk_buf, cjk_len);
                cjk_run = 0;
            }
            if (tok_len > 0) {
                rc = rc && emit_ascii(out, tok, tok_len);
                tok_len = 0;
            }
            i += adv;
        }
    }

    if (rc && cjk_run > 0) {
        if (cjk_run == 1 && !cjk_stop_cp(cjk_cp))
            rc = sh_text_tokens_push(out, cjk_buf, cjk_len);
    }
    if (rc && tok_len > 0) rc = emit_ascii(out, tok, tok_len);
    return rc;
}

/* ------------------------------------------------------------- names */

char *sh_text_normalize_name(const char *name) {
    size_t n, o = 0;
    char *out;
    int pending_sep = 0;
    int seen = 0;

    if (!name) return NULL;
    n = strlen(name);
    out = malloc(n + 1);
    if (!out) return NULL;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || is_ident_sep(c)) {
            if (seen) pending_sep = 1;
            continue;
        }
        if (pending_sep) {
            out[o++] = '-';
            pending_sep = 0;
        }
        out[o++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        seen = 1;
    }
    out[o] = '\0';
    return out;
}

/* ------------------------------------------------------------ profile */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int full;
} pbuf;

#define PROFILE_CAP (8 * 1024)

static int pbuf_add(pbuf *b, const char *s, size_t n) {
    size_t need;

    if (!b || b->full || n == 0) return 1;
    need = b->len + n + 2;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < need) cap *= 2;
        if (cap > PROFILE_CAP) cap = PROFILE_CAP;
        {
            char *data = realloc(b->data, cap);
            if (!data) return 0;
            b->data = data;
            b->cap = cap;
        }
    }
    if (b->len + n + 1 >= b->cap) {
        n = b->cap > b->len + 2 ? b->cap - b->len - 2 : 0;
        b->full = 1;
    }
    if (n > 0) {
        if (b->len > 0) b->data[b->len++] = ' ';
        memcpy(b->data + b->len, s, n);
        b->len += n;
    }
    b->data[b->len] = '\0';
    return 1;
}

static int contains_ci(const char *line, size_t n, const char *needle) {
    size_t nn = strlen(needle);
    if (nn == 0 || nn > n) return 0;
    for (size_t i = 0; i + nn <= n; i++) {
        size_t k = 0;
        while (k < nn && lower_ascii((unsigned char)line[i + k]) ==
                             lower_ascii((unsigned char)needle[k]))
            k++;
        if (k == nn) return 1;
    }
    return 0;
}

/* Copy the base name of the first `references/<path>` mention on the line. */
static size_t ref_basename(const char *line, size_t n, char *out, size_t cap) {
    static const char *const prefixes[] = {"references/", "reference/"};
    size_t best = 0;
    int found = 0;

    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        size_t plen = strlen(prefixes[p]);
        for (size_t i = 0; i + plen < n; i++) {
            if (memcmp(line + i, prefixes[p], plen) != 0) continue;
            {
                size_t start = i + plen;
                size_t end = start;
                size_t base;
                while (end < n) {
                    unsigned char c = (unsigned char)line[end];
                    if (c <= ' ' || c == ')' || c == '(' || c == '"' || c == '\'' ||
                        c == '`' || c == '>' || c == '<' || c == ',' || c == ';')
                        break;
                    end++;
                }
                if (end == start) continue;
                base = start;
                for (size_t k = start; k < end; k++)
                    if (line[k] == '/' || line[k] == '\\') base = k + 1;
                if (base >= end) continue;
                if (end - base >= cap) end = base + cap - 1;
                if (!found || end - base > best) {
                    memcpy(out, line + base, end - base);
                    out[end - base] = '\0';
                    best = end - base;
                    found = 1;
                }
            }
            break; /* first mention per prefix is enough */
        }
    }
    return found ? best : 0;
}

static const char *skip_spaces(const char *s, size_t n, size_t *at) {
    size_t i = *at;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    *at = i;
    return s + i;
}

char *sh_text_profile(const char *body, size_t len, size_t *out_len) {
    pbuf b;
    int in_fence = 0;
    size_t pos = 0;

    if (out_len) *out_len = 0;
    if (!body || len == 0) return NULL;

    memset(&b, 0, sizeof(b));

    while (pos < len) {
        size_t eol = pos;
        size_t end;
        size_t at = 0;

        while (eol < len && body[eol] != '\n') eol++;
        end = eol;
        if (end > pos && body[end - 1] == '\r') end--;

        /* Fenced code block: capture the language tag, toggle the state. */
        {
            const char *line = body + pos;
            size_t n = end - pos;
            size_t marker = 0;
            skip_spaces(line, n, &at);
            while (at < n && (line[at] == '`' || line[at] == '~')) {
                marker++;
                at++;
            }
            if (marker >= 3) {
                if (!in_fence) {
                    size_t tag_at = at;
                    size_t tag_end;
                    skip_spaces(line, n, &tag_at);
                    tag_end = n;
                    while (tag_end > tag_at && (line[tag_end - 1] == ' ' || line[tag_end - 1] == '\t'))
                        tag_end--;
                    if (tag_end > tag_at) {
                        /* Only the first word of the tag matters. */
                        size_t word_end = tag_at;
                        while (word_end < tag_end && line[word_end] != ' ' && line[word_end] != '\t' &&
                               line[word_end] != '{' && line[word_end] != ',')
                            word_end++;
                        if (word_end > tag_at) pbuf_add(&b, line + tag_at, word_end - tag_at);
                    }
                    in_fence = 1;
                } else {
                    in_fence = 0;
                }
                pos = eol + 1;
                continue;
            }
        }

        if (!in_fence) {
            const char *line = body + pos;
            size_t n = end - pos;

            /* H2 / H3 headings. */
            at = 0;
            skip_spaces(line, n, &at);
            if (at < n && line[at] == '#') {
                size_t hashes = 0;
                size_t k = at;
                while (k < n && line[k] == '#') {
                    hashes++;
                    k++;
                }
                if ((hashes == 2 || hashes == 3) && (k == n || line[k] == ' ' || line[k] == '\t')) {
                    size_t t = k;
                    size_t t_end = n;
                    skip_spaces(line, n, &t);
                    while (t_end > t && (line[t_end - 1] == ' ' || line[t_end - 1] == '\t')) t_end--;
                    if (t_end > t) pbuf_add(&b, line + t, t_end - t);
                }
            }

            /* Trigger lines. */
            if (contains_ci(line, n, "when to use") || contains_ci(line, n, "use when") ||
                contains_ci(line, n, "trigger") || contains_ci(line, n, "use this") ||
                contains_ci(line, n, "适用") || contains_ci(line, n, "何时使用")) {
                size_t t = 0;
                size_t t_end = n;
                skip_spaces(line, n, &t);
                while (t_end > t && (line[t_end - 1] == ' ' || line[t_end - 1] == '\t')) t_end--;
                if (t_end > t) pbuf_add(&b, line + t, t_end - t);
            }

            /* Referenced files. */
            {
                char name[256];
                size_t nlen = ref_basename(line, n, name, sizeof(name));
                if (nlen > 0) pbuf_add(&b, name, nlen);
            }
        }

        pos = eol + 1;
    }

    if (b.len == 0) {
        free(b.data);
        return NULL;
    }
    if (out_len) *out_len = b.len;
    return b.data;
}

/* ----------------------------------------------------------- shingles */

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t fnv_bytes(uint64_t h, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= FNV_PRIME;
    }
    return h;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

int sh_text_shingles_build(const char *text, size_t len, sh_text_shingles *out) {
    sh_text_tokens t;
    size_t n;
    uint64_t *hashes;

    if (!out) return 0;
    out->hashes = NULL;
    out->count = 0;
    memset(&t, 0, sizeof(t));

    if (!sh_text_tokenize(text, len, &t)) {
        sh_text_tokens_free(&t);
        return 0;
    }
    if (t.count < 5) {
        sh_text_tokens_free(&t);
        return 1;
    }

    n = t.count - 4;
    hashes = malloc(n * sizeof(uint64_t));
    if (!hashes) {
        sh_text_tokens_free(&t);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        uint64_t h = FNV_OFFSET;
        for (size_t k = 0; k < 5; k++) {
            const char *tok = t.items[i + k];
            h = fnv_bytes(h, tok, strlen(tok));
            h ^= 0x1f;
            h *= FNV_PRIME;
        }
        hashes[i] = h;
    }

    qsort(hashes, n, sizeof(uint64_t), cmp_u64);
    {
        size_t unique = 0;
        for (size_t i = 0; i < n; i++)
            if (unique == 0 || hashes[i] != hashes[unique - 1]) hashes[unique++] = hashes[i];
        out->count = unique;
    }
    out->hashes = hashes;
    sh_text_tokens_free(&t);
    return 1;
}

void sh_text_shingles_free(sh_text_shingles *s) {
    if (!s) return;
    free(s->hashes);
    s->hashes = NULL;
    s->count = 0;
}

/*
 * Merge-based Jaccard.  With threshold >= 0 the function may stop early:
 * the size bound rejects impossible pairs in O(1), and a ratio already
 * past the threshold can never drop back below it (the intersection only
 * grows and the union only shrinks).
 */
static double jaccard_core(const sh_text_shingles *a, const sh_text_shingles *b, double threshold) {
    size_t na, nb, i = 0, j = 0;
    size_t inter = 0;

    if (!a || !b) return 0.0;
    na = a->count;
    nb = b->count;
    if (na == 0 || nb == 0) return (na == 0 && nb == 0) ? 1.0 : 0.0;

    if (threshold >= 0.0) {
        double lo = na < nb ? (double)na : (double)nb;
        double hi = na < nb ? (double)nb : (double)na;
        if (lo / hi < threshold) return 0.0;
    }

    while (i < na && j < nb) {
        uint64_t x = a->hashes[i];
        uint64_t y = b->hashes[j];
        if (x == y) {
            inter++;
            i++;
            j++;
        } else if (x < y) {
            i++;
        } else {
            j++;
        }
        if (threshold >= 0.0) {
            double ratio = (double)inter / (double)(na + nb - inter);
            if (ratio >= threshold) return 1.0;
        }
    }
    if (inter == 0) return 0.0;
    return (double)inter / (double)(na + nb - inter);
}

double sh_text_shingles_jaccard(const sh_text_shingles *a, const sh_text_shingles *b) {
    return jaccard_core(a, b, -1.0);
}

int sh_text_shingles_near_duplicate(const sh_text_shingles *a, const sh_text_shingles *b,
                                    double threshold) {
    size_t na = a ? a->count : 0;
    size_t nb = b ? b->count : 0;
    if (na == 0 || nb == 0) return na == 0 && nb == 0;
    return jaccard_core(a, b, threshold) >= threshold;
}

double sh_text_jaccard(const char *a, size_t alen, const char *b, size_t blen) {
    sh_text_shingles sa, sb;
    double value;

    if (!sh_text_shingles_build(a, alen, &sa)) return 0.0;
    if (!sh_text_shingles_build(b, blen, &sb)) {
        sh_text_shingles_free(&sa);
        return 0.0;
    }
    value = jaccard_core(&sa, &sb, -1.0);
    sh_text_shingles_free(&sa);
    sh_text_shingles_free(&sb);
    return value;
}

int sh_text_near_duplicate(const char *a, size_t alen, const char *b, size_t blen,
                           double threshold) {
    sh_text_shingles sa, sb;
    int value;

    if (!sh_text_shingles_build(a, alen, &sa)) return 0;
    if (!sh_text_shingles_build(b, blen, &sb)) {
        sh_text_shingles_free(&sa);
        return 0;
    }
    value = sh_text_shingles_near_duplicate(&sa, &sb, threshold);
    sh_text_shingles_free(&sa);
    sh_text_shingles_free(&sb);
    return value;
}
