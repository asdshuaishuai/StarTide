/*
 * sh_wordpiece.c - BERT WordPiece tokenisation (see sh_wordpiece.h).
 */
#include "sh_wordpiece.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WP_MAX_TOKEN_BYTES 64
#define WP_MAX_PIECE_BYTES 128

struct sh_wordpiece {
    char **tokens;      /* owned; index == id */
    size_t count;
    uint32_t *slots;    /* open-addressed table: id + 1, 0 = empty */
    size_t slot_count;  /* power of two */
    long cls_id;
    long sep_id;
    long pad_id;
    long unk_id;
    int lower_case;
};

/* ------------------------------------------------------------ hashing */

static uint64_t wp_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;

    for (; *s; s++) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static void wp_insert(sh_wordpiece *wp, size_t id) {
    size_t mask = wp->slot_count - 1;
    size_t pos = (size_t)(wp_hash(wp->tokens[id]) & (uint64_t)mask);

    while (wp->slots[pos] != 0) pos = (pos + 1) & mask;
    wp->slots[pos] = (uint32_t)(id + 1);
}

long sh_wp_token_id(sh_wordpiece *wp, const char *token) {
    size_t mask;
    size_t pos;

    if (!wp || !token || !token[0]) return -1;
    mask = wp->slot_count - 1;
    pos = (size_t)(wp_hash(token) & (uint64_t)mask);
    for (size_t probe = 0; probe < wp->slot_count; probe++) {
        uint32_t slot = wp->slots[pos];
        if (slot == 0) return -1;
        if (strcmp(wp->tokens[slot - 1], token) == 0) return (long)(slot - 1);
        pos = (pos + 1) & mask;
    }
    return -1;
}

size_t sh_wp_vocab_size(const sh_wordpiece *wp) { return wp ? wp->count : 0; }

/* -------------------------------------------------------------- load */

void sh_wp_free(sh_wordpiece *wp) {
    if (!wp) return;
    for (size_t i = 0; i < wp->count; i++) free(wp->tokens[i]);
    free(wp->tokens);
    free(wp->slots);
    free(wp);
}

sh_wordpiece *sh_wp_load(const char *vocab_path, int lower_case) {
    FILE *f;
    sh_wordpiece *wp;
    char line[1024];
    size_t cap = 4096;
    size_t slots = 4096;

    if (!vocab_path) return NULL;
    f = fopen(vocab_path, "rb");
    if (!f) return NULL;

    wp = calloc(1, sizeof(sh_wordpiece));
    if (!wp) {
        fclose(f);
        return NULL;
    }
    wp->tokens = calloc(cap, sizeof(char *));
    if (!wp->tokens) {
        fclose(f);
        free(wp);
        return NULL;
    }
    wp->lower_case = lower_case;

    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        char *copy;

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue; /* a blank line would silently shift every id */
        if (wp->count == cap) {
            char **bigger = realloc(wp->tokens, cap * 2 * sizeof(char *));
            if (!bigger) goto fail;
            wp->tokens = bigger;
            cap *= 2;
        }
        copy = malloc(n + 1);
        if (!copy) goto fail;
        memcpy(copy, line, n + 1);
        wp->tokens[wp->count++] = copy;
    }
    fclose(f);
    f = NULL;

    while (slots < wp->count * 2) slots <<= 1;
    wp->slot_count = slots;
    wp->slots = calloc(slots, sizeof(uint32_t));
    if (!wp->slots) goto fail;
    for (size_t i = 0; i < wp->count; i++) wp_insert(wp, i);

    wp->cls_id = sh_wp_token_id(wp, "[CLS]");
    wp->sep_id = sh_wp_token_id(wp, "[SEP]");
    wp->pad_id = sh_wp_token_id(wp, "[PAD]");
    wp->unk_id = sh_wp_token_id(wp, "[UNK]");
    if (wp->cls_id < 0 || wp->sep_id < 0 || wp->unk_id < 0) goto fail;
    if (wp->pad_id < 0) wp->pad_id = 0;

    return wp;

fail:
    if (f) fclose(f);
    sh_wp_free(wp);
    return NULL;
}

/* -------------------------------------------------------- tokenising */

/* A CJK ideograph (or full-width punctuation) is emitted as its own token.
 * The lead byte ranges cover the BMP blocks BERT isolates. */
static int is_isolated_byte(unsigned char c) { return (c & 0xE0) == 0xE0; }

static int is_ascii_punct(unsigned char c) {
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) ||
           (c >= 123 && c <= 126);
}

/* Length of a UTF-8 sequence starting at s. */
static size_t utf8_span(const char *s) {
    unsigned char c = (unsigned char)s[0];

    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/*
 * Attempt to consume one word piece from `word` at `start`, writing it to
 * `out`. With `continuation` the candidate carries the "##" prefix. Returns
 * the consumed length, or 0 when no piece starting here exists.
 */
static size_t wp_match(sh_wordpiece *wp, const char *word, size_t len, size_t start,
                       int continuation, char *out, size_t out_cap) {
    size_t max_end = len;
    size_t limit = start + WP_MAX_PIECE_BYTES;

    if (max_end > limit) max_end = limit;
    for (size_t end = max_end; end > start; end--) {
        size_t n = end - start;
        size_t prefix = continuation ? 2 : 0;
        size_t span;

        /* Never cut a UTF-8 sequence in half. */
        span = utf8_span(word + start);
        if (span > 1 && (n % span) != 0) continue;
        if (prefix + n >= out_cap) continue;
        if (continuation) {
            out[0] = '#';
            out[1] = '#';
        }
        memcpy(out + prefix, word + start, n);
        out[prefix + n] = '\0';
        if (sh_wp_token_id(wp, out) >= 0) return n;
    }
    return 0;
}

static void wp_emit(sh_wordpiece *wp, const char *word, size_t len, int64_t *ids,
                    int64_t *mask, size_t *n, size_t max_tokens) {
    size_t start = 0;
    char piece[WP_MAX_PIECE_BYTES + 4];
    int continuation = 0;

    /* BERT semantics: a word whose first piece cannot be matched becomes a
     * single [UNK], rather than being skipped or split arbitrarily. */
    if (len > WP_MAX_PIECE_BYTES) {
        if (*n < max_tokens) {
            ids[*n] = wp->unk_id;
            mask[*n] = 1;
            (*n)++;
        }
        return;
    }

    while (start < len && *n < max_tokens) {
        size_t took = wp_match(wp, word, len, start, continuation, piece, sizeof(piece));

        if (took == 0) {
            if (!continuation) {
                ids[*n] = wp->unk_id;
                mask[*n] = 1;
                (*n)++;
            }
            return;
        }
        ids[*n] = sh_wp_token_id(wp, piece);
        mask[*n] = 1;
        (*n)++;
        start += took;
        continuation = 1;
    }
}

size_t sh_wp_encode(sh_wordpiece *wp, const char *text, size_t len, int64_t *ids,
                    int64_t *mask, size_t max_tokens) {
    char word[WP_MAX_PIECE_BYTES + 1];
    size_t word_len = 0;
    size_t n = 0;

    if (!wp || !ids || !mask || max_tokens < 3) return 0;

    ids[n] = wp->cls_id;
    mask[n] = 1;
    n++;

    for (size_t i = 0; i < len && text[i]; ) {
        unsigned char c = (unsigned char)text[i];

        if (c < 0x80) {
            if (isspace(c)) {
                if (word_len) wp_emit(wp, word, word_len, ids, mask, &n, max_tokens - 1);
                word_len = 0;
                i++;
                continue;
            }
            if (is_ascii_punct(c)) {
                if (word_len) wp_emit(wp, word, word_len, ids, mask, &n, max_tokens - 1);
                word_len = 0;
                word[0] = (char)c;
                wp_emit(wp, word, 1, ids, mask, &n, max_tokens - 1);
                i++;
                continue;
            }
            if (word_len + 1 < sizeof(word)) {
                word[word_len++] = wp->lower_case ? (char)tolower(c) : (char)c;
            }
            i++;
            continue;
        }

        if (is_isolated_byte(c)) {
            size_t span = utf8_span(text + i);
            if (word_len) wp_emit(wp, word, word_len, ids, mask, &n, max_tokens - 1);
            word_len = 0;
            if (i + span <= len) wp_emit(wp, text + i, span, ids, mask, &n, max_tokens - 1);
            i += span;
            continue;
        }

        /* Other non-ASCII (accented Latin, …) stays inside the word. */
        {
            size_t span = utf8_span(text + i);
            if (word_len + span < sizeof(word)) {
                memcpy(word + word_len, text + i, span);
                word_len += span;
            }
            i += span;
        }
    }
    if (word_len) wp_emit(wp, word, word_len, ids, mask, &n, max_tokens - 1);

    ids[n] = wp->sep_id;
    mask[n] = 1;
    n++;
    return n;
}
