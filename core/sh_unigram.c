/*
 * sh_unigram.c - Unigram segmentation over tokenizer.json (see sh_unigram.h).
 */
#include "sh_unigram.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"

#define UNI_MAX_PIECE_BYTES 48
#define UNI_UNK_PENALTY (-100.0)

struct sh_unigram {
    char **pieces;   /* owned; index == id */
    float *scores;
    size_t count;
    uint32_t *slots; /* open addressing: id + 1, 0 = empty */
    size_t slot_count;
    size_t max_piece_bytes;
    long unk_id;
    long bos_id;
    long eos_id;
    long pad_id;
};

/* ------------------------------------------------------------ helpers */

static uint64_t uni_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;

    for (; *s; s++) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static long uni_lookup(const sh_unigram *u, const char *piece) {
    size_t mask;
    size_t pos;

    if (!u || !piece || !piece[0]) return -1;
    mask = u->slot_count - 1;
    pos = (size_t)(uni_hash(piece) & (uint64_t)mask);
    for (size_t probe = 0; probe < u->slot_count; probe++) {
        uint32_t slot = u->slots[pos];
        if (slot == 0) return -1;
        if (strcmp(u->pieces[slot - 1], piece) == 0) return (long)(slot - 1);
        pos = (pos + 1) & mask;
    }
    return -1;
}

void sh_uni_free(sh_unigram *u) {
    if (!u) return;
    for (size_t i = 0; i < u->count; i++) free(u->pieces[i]);
    free(u->pieces);
    free(u->scores);
    free(u->slots);
    free(u);
}

size_t sh_uni_vocab_size(const sh_unigram *u) { return u ? u->count : 0; }
long sh_uni_unk_id(sh_unigram *u) { return u ? u->unk_id : -1; }
long sh_uni_id_of(sh_unigram *u, const char *piece) { return uni_lookup(u, piece); }

/* ------------------------------------------------------- json scanning */

/* Parse one JSON string starting at *pp (which must point at '"'). Writes
 * the decoded bytes into out (NUL-terminated) and advances *pp past the
 * closing quote. Returns 1 on success. */
static int json_string(const char **pp, const char *end, char *out, size_t cap) {
    const char *p = *pp;
    size_t n = 0;

    if (p >= end || *p != '"') return 0;
    p++;
    while (p < end && *p != '"') {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            if (p >= end) return 0;
            switch (*p) {
                case '"': if (n + 1 < cap) out[n++] = '"'; p++; break;
                case '\\': if (n + 1 < cap) out[n++] = '\\'; p++; break;
                case '/': if (n + 1 < cap) out[n++] = '/'; p++; break;
                case 'b': if (n + 1 < cap) out[n++] = '\b'; p++; break;
                case 'f': if (n + 1 < cap) out[n++] = '\f'; p++; break;
                case 'n': if (n + 1 < cap) out[n++] = '\n'; p++; break;
                case 'r': if (n + 1 < cap) out[n++] = '\r'; p++; break;
                case 't': if (n + 1 < cap) out[n++] = '\t'; p++; break;
                case 'u': {
                    /* \uXXXX: encode as UTF-8. Surrogate pairs are rare in
                     * tokenizer vocabularies; unpaired halves are kept as-is. */
                    unsigned cp = 0;
                    p++;
                    for (int k = 0; k < 4 && p < end; k++, p++) {
                        cp <<= 4;
                        if (*p >= '0' && *p <= '9') cp |= (unsigned)(*p - '0');
                        else if (*p >= 'a' && *p <= 'f') cp |= (unsigned)(*p - 'a' + 10);
                        else if (*p >= 'A' && *p <= 'F') cp |= (unsigned)(*p - 'A' + 10);
                    }
                    if (cp < 0x80) {
                        if (n + 1 < cap) out[n++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (n + 2 < cap) {
                            out[n++] = (char)(0xC0 | (cp >> 6));
                            out[n++] = (char)(0x80 | (cp & 0x3F));
                        }
                    } else {
                        if (n + 3 < cap) {
                            out[n++] = (char)(0xE0 | (cp >> 12));
                            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[n++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    continue;
                }
                default: p++; break;
            }
            continue;
        }
        if (n + 1 < cap) out[n++] = (char)c;
        p++;
    }
    if (p >= end) return 0;
    out[n] = '\0';
    *pp = p + 1;
    return 1;
}

/* Skip whitespace. */
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

/* Find `"key"` at or after p, returning a pointer just past the key. */
static const char *find_key(const char *p, const char *end, const char *key) {
    size_t klen = strlen(key);

    while (p < end) {
        const char *hit = NULL;
        for (const char *q = p; q + klen < end; q++) {
            if (memcmp(q, key, klen) == 0) {
                hit = q;
                break;
            }
        }
        if (!hit) return NULL;
        return hit + klen;
    }
    return NULL;
}

/* --------------------------------------------------------------- load */

sh_unigram *sh_uni_load(const char *path) {
    char *raw = NULL;
    size_t raw_len = 0;
    sh_unigram *u = NULL;
    const char *p;
    const char *end;
    size_t cap = 0;

    if (!path) return NULL;
    if (sh_read_file(path, &raw, &raw_len) != 0) return NULL;
    end = raw + raw_len;

    u = calloc(1, sizeof(sh_unigram));
    if (!u) {
        free(raw);
        return NULL;
    }
    u->unk_id = 3;
    u->bos_id = 0;
    u->eos_id = 2;
    u->pad_id = 1;

    /* The vocabulary lives under "model": {"vocab": [["piece", score], ...] }. */
    p = find_key(raw, end, "\"vocab\"");
    if (!p) goto fail;
    p = skip_ws(p, end);
    if (p >= end || *p != ':') goto fail;
    p = skip_ws(p + 1, end);
    if (p >= end || *p != '[') goto fail;
    p++;

    for (;;) {
        char piece[UNI_MAX_PIECE_BYTES + 8];
        double score = 0.0;

        p = skip_ws(p, end);
        if (p >= end) goto fail;
        if (*p == ']') {
            p++;
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '[') goto fail;
        p = skip_ws(p + 1, end);
        if (!json_string(&p, end, piece, sizeof(piece))) goto fail;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            char num[64];
            size_t n = 0;
            p = skip_ws(p + 1, end);
            while (p < end && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' ||
                               *p == '.' || *p == 'e' || *p == 'E') &&
                   n + 1 < sizeof(num))
                num[n++] = *p++;
            num[n] = '\0';
            score = strtod(num, NULL);
        }
        /* Close the pair's own ']'. Without this the loop below mistakes it
         * for the end of the vocabulary and stops after one entry. */
        p = skip_ws(p, end);
        if (p < end && *p == ']') p++;

        if (u->count == cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            char **pieces = realloc(u->pieces, ncap * sizeof(char *));
            float *scores;
            if (!pieces) goto fail;
            u->pieces = pieces;
            scores = realloc(u->scores, ncap * sizeof(float));
            if (!scores) goto fail;
            u->scores = scores;
            cap = ncap;
        }
        {
            size_t plen = strlen(piece);
            char *copy = malloc(plen + 1);
            if (!copy) goto fail;
            memcpy(copy, piece, plen + 1);
            u->pieces[u->count] = copy;
            u->scores[u->count] = (float)score;
            if (plen > u->max_piece_bytes) u->max_piece_bytes = plen;
            u->count++;
        }
    }

    if (u->count == 0) goto fail;
    if (u->max_piece_bytes > UNI_MAX_PIECE_BYTES) u->max_piece_bytes = UNI_MAX_PIECE_BYTES;

    {
        size_t slots = 4096;
        while (slots < u->count * 2) slots <<= 1;
        u->slot_count = slots;
        u->slots = calloc(slots, sizeof(uint32_t));
        if (!u->slots) goto fail;
        for (size_t i = 0; i < u->count; i++) {
            size_t mask = slots - 1;
            size_t pos = (size_t)(uni_hash(u->pieces[i]) & (uint64_t)mask);
            while (u->slots[pos] != 0) pos = (pos + 1) & mask;
            u->slots[pos] = (uint32_t)(i + 1);
        }
    }

    /* Special-token ids: prefer what added_tokens declares. */
    {
        const char *at = find_key(raw, end, "\"added_tokens\"");
        if (at) {
            const char *q = at;
            while (q && q < end) {
                char content[64];
                char idbuf[32];
                size_t n = 0;
                const char *next = find_key(q, end, "\"content\"");
                const char *idp = find_key(q, end, "\"id\"");
                if (!next || !idp) break;
                next = skip_ws(next, end);
                if (next < end && *next == ':') next = skip_ws(next + 1, end);
                if (!json_string(&next, end, content, sizeof(content))) break;
                idp = skip_ws(idp, end);
                if (idp < end && *idp == ':') idp = skip_ws(idp + 1, end);
                while (idp < end && isdigit((unsigned char)*idp) && n + 1 < sizeof(idbuf))
                    idbuf[n++] = *idp++;
                idbuf[n] = '\0';
                if (n > 0) {
                    long id = strtol(idbuf, NULL, 10);
                    if (strcmp(content, "<s>") == 0) u->bos_id = id;
                    else if (strcmp(content, "</s>") == 0) u->eos_id = id;
                    else if (strcmp(content, "<pad>") == 0) u->pad_id = id;
                    else if (strcmp(content, "<unk>") == 0) u->unk_id = id;
                }
                q = next;
            }
        }
    }

    free(raw);
    return u;

fail:
    free(raw);
    sh_uni_free(u);
    return NULL;
}

/* ---------------------------------------------------------- segmenting */

/* Replace spaces with U+2581 and prepend one, the Metaspace convention.
 * Returns a malloc'd string. */
static char *metaspace(const char *text, size_t len) {
    static const char MARK[] = "\xe2\x96\x81";
    /* Every byte can become a 3-byte marker (a run of spaces expands 3x),
     * so the buffer is sized for the worst case. */
    char *out = malloc(3 * len + 8);
    size_t at = 0;

    if (!out) return NULL;
    memcpy(out, MARK, 3);
    at = 3;
    for (size_t i = 0; i < len && text[i]; i++) {
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') {
            memcpy(out + at, MARK, 3);
            at += 3;
        } else {
            out[at++] = text[i];
        }
    }
    out[at] = '\0';
    return out;
}

/* Byte length of the UTF-8 character starting at s. */
static size_t u8_span(const char *s) {
    unsigned char c = (unsigned char)s[0];

    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t sh_uni_encode(sh_unigram *u, const char *text, size_t len, int64_t *ids, int64_t *mask,
                     size_t max_tokens) {
    char *norm;
    size_t nlen;
    float *best;
    int32_t *prev;
    long *best_id;
    size_t n = 0;

    if (!u || !ids || !mask || max_tokens < 3) return 0;

    norm = metaspace(text, len);
    if (!norm) return 0;
    nlen = strlen(norm);

    best = malloc((nlen + 1) * sizeof(float));
    prev = malloc((nlen + 1) * sizeof(int32_t));
    best_id = malloc((nlen + 1) * sizeof(long));
    if (!best || !prev || !best_id) {
        free(norm);
        free(best);
        free(prev);
        free(best_id);
        return 0;
    }
    for (size_t i = 0; i <= nlen; i++) {
        best[i] = -1e30f;
        prev[i] = -1;
        best_id[i] = -1;
    }
    best[0] = 0.0f;

    for (size_t i = 0; i < nlen; i++) {
        size_t limit;
        if (best[i] <= -1e29f) continue;
        limit = u->max_piece_bytes;
        if (i + limit > nlen) limit = nlen - i;
        for (size_t l = 1; l <= limit; l++) {
            char piece[UNI_MAX_PIECE_BYTES + 8];
            long id;
            float score;

            if (l >= sizeof(piece)) break;
            memcpy(piece, norm + i, l);
            piece[l] = '\0';
            /* Only allow boundaries on a character edge. */
            if (i + l < nlen && u8_span(norm + i + l) > 1) {
                size_t span = u8_span(norm + i + l);
                if (span > 1 && l > 1) {
                    /* A cut in the middle of a sequence is not a boundary. */
                    const char *q = norm + i;
                    size_t pos = 0;
                    while (pos < l) {
                        size_t sp = u8_span(q + pos);
                        if (pos + sp > l) break;
                        pos += sp;
                    }
                    if (pos != l) continue;
                }
            }
            id = uni_lookup(u, piece);
            if (id < 0) continue;
            score = best[i] + u->scores[id];
            if (score > best[i + l]) {
                best[i + l] = score;
                prev[i + l] = (int32_t)i;
                best_id[i + l] = id;
            }
        }
        /* Unknown fallback: consume one character. */
        {
            size_t span = u8_span(norm + i);
            float score = best[i] + UNI_UNK_PENALTY;
            if (i + span <= nlen && score > best[i + span]) {
                best[i + span] = score;
                prev[i + span] = (int32_t)i;
                best_id[i + span] = u->unk_id;
            }
        }
    }

    /* Walk back, then emit forward. */
    ids[n] = u->bos_id;
    mask[n] = 1;
    n++;
    {
        size_t *stack = malloc((nlen + 1) * sizeof(size_t));
        size_t depth = 0;
        size_t at = nlen;

        if (!stack) {
            free(norm);
            free(best);
            free(prev);
            free(best_id);
            return 0;
        }
        while (at > 0 && prev[at] >= 0) {
            stack[depth++] = (size_t)best_id[at];
            at = (size_t)prev[at];
        }
        while (depth > 0 && n < max_tokens - 1) {
            long id = (long)stack[--depth];
            ids[n] = id < 0 ? u->unk_id : id;
            mask[n] = 1;
            n++;
        }
        free(stack);
    }
    ids[n] = u->eos_id;
    mask[n] = 1;
    n++;

    free(norm);
    free(best);
    free(prev);
    free(best_id);
    return n;
}
