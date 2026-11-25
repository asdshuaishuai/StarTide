/*
 * sh_text.h - tokenisation, deterministic enrichment and near-duplicate
 * detection for the skill index.
 *
 * The tokeniser is the front end of both the BM25F index (sh_index.c) and
 * the query path (sh_route.c).  It is deliberately the same code on both
 * sides: a query has to produce the same terms the documents were indexed
 * with, or no amount of scoring rescues the ranking.
 *
 * Three rules matter for this corpus:
 *   - ASCII runs are cut on characters that never belong to the same
 *     identifier: whitespace, punctuation, and the code separators
 *     '_' '-' '.' '/' ':'.  Skill names like `godot_export` therefore
 *     index as both `godot` and `export`, and the same query wording
 *     matches whichever spelling the document used.
 *   - CJK has no spaces, so a run of ideographs emits overlapping
 *     bigrams (and a unigram when the run is a single character).
 *   - A small bilingual stopword list removes the words that carry no
 *     discrimination ("the", "of", "的", "了").
 *
 * Everything here is deterministic and model-free.  Enrich-Retrieve-Rank
 * (2608.22695) found LLM enrichment neutral-to-negative once metadata is
 * adequate, so the profile is extracted from headings, trigger lines,
 * fence tags and reference mentions with plain string rules.
 */
#ifndef SKILLCTL_TEXT_H
#define SKILLCTL_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------- tokeniser */

#define SH_TEXT_MAX_TOKEN 64

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} sh_text_tokens;

/*
 * Tokenise `text` and append the tokens to `out` (zero-initialise it
 * first; free with sh_text_tokens_free).  Returns 1 on success, 0 on
 * allocation failure.  An empty text yields zero tokens.
 */
int sh_text_tokenize(const char *text, size_t len, sh_text_tokens *out);

/* Append one raw token (no filtering).  Used by callers that build terms
 * from an already tokenised source. Returns 1 on success. */
int sh_text_tokens_push(sh_text_tokens *t, const char *s, size_t n);

void sh_text_tokens_free(sh_text_tokens *t);

/* True when the token is a stopword (English list, or a CJK character
 * from the small Chinese list).  Exposed for tests and for callers that
 * tokenise by hand. */
int sh_text_is_stopword(const char *s, size_t n);

/*
 * Normalise a skill name for duplicate grouping: lowercased, trimmed,
 * with runs of '_', '-', '.', ':' and whitespace folded to a single '-'.
 * Returns a malloc'd string (caller frees), or NULL on allocation
 * failure.
 */
char *sh_text_normalize_name(const char *name);

/* ----------------------------------------------------------- profile */

/*
 * Deterministic enrichment, the "profile" field of the index.  Extracts,
 * in this order:
 *   - every H2/H3 heading (`## ...`, `### ...`),
 *   - every line that carries a trigger phrase ("when to use",
 *     "use when", "trigger", "use this", "适用", "何时使用"),
 *   - the language tag of every fenced code block (```python),
 *   - every file name mentioned below a `references/` (or `reference/`)
 *     directory.
 * Returns a malloc'd plain-text blob (words separated by spaces) and
 * sets *out_len, or NULL when nothing was found.  The caller frees it.
 */
char *sh_text_profile(const char *body, size_t len, size_t *out_len);

/* -------------------------------------------------------- shingles */

/*
 * 5-token shingle sets, hashed with FNV-1a into 64-bit values and kept
 * sorted so two sets compare with a linear merge.  The near-duplicate
 * test is used twice: to merge identical skill variants at index time
 * (the "false negative filtering" step that SkillRouter credits with
 * +4.0pp) and to collapse duplicate hits at query time.
 */
typedef struct {
    uint64_t *hashes;
    size_t count;
} sh_text_shingles;

int sh_text_shingles_build(const char *text, size_t len, sh_text_shingles *out);
void sh_text_shingles_free(sh_text_shingles *s);

double sh_text_shingles_jaccard(const sh_text_shingles *a, const sh_text_shingles *b);

/*
 * Thresholded Jaccard with early exits: the size bound rejects pairs
 * whose shingle counts make the ratio unreachable, and the merge stops
 * as soon as the growing intersection provably clears the threshold.
 * Both sets empty counts as identical.
 */
int sh_text_shingles_near_duplicate(const sh_text_shingles *a, const sh_text_shingles *b,
                                    double threshold);

/* Convenience wrappers over the two texts.  Return 0 when either build
 * fails, which lets the indexer treat an allocation failure as "not a
 * duplicate" rather than aborting the whole build. */
double sh_text_jaccard(const char *a, size_t alen, const char *b, size_t blen);
int sh_text_near_duplicate(const char *a, size_t alen, const char *b, size_t blen,
                           double threshold);

#ifdef __cplusplus
}
#endif

#endif /* SKILLCTL_TEXT_H */
