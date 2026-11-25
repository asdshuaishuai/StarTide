/*
 * sh_wordpiece.h - a BERT WordPiece tokeniser in plain C.
 *
 * The embedding model runs through ONNX Runtime, so tokenisation cannot be
 * delegated to a Python library: the same process has to turn text into
 * input_ids before the session can run. BERT-family models ship a
 * `vocab.txt` (one token per line, line number = id), which is exactly what
 * a greedy longest-match WordPiece tokeniser needs, so this stays small and
 * dependency-free.
 *
 * Scope: the BERT basic tokeniser (whitespace, ASCII punctuation, isolated
 * CJK characters) followed by word-piece splitting with "##" continuation
 * prefixes. That is what bge-*, gte-*, and every other BERT-derived
 * embedding model expect.
 */
#ifndef SKILLCTL_WORDPIECE_H
#define SKILLCTL_WORDPIECE_H

#include <stddef.h>
#include <stdint.h>

#define SH_WP_MAX_TOKENS 512

typedef struct sh_wordpiece sh_wordpiece;

/*
 * Load vocab.txt. `lower_case` folds ASCII before matching, which is what
 * the uncased BERT checkpoints (bge-small-zh, all-MiniLM, …) were trained
 * with. Returns NULL when the file is unreadable or lacks [CLS]/[SEP].
 */
sh_wordpiece *sh_wp_load(const char *vocab_path, int lower_case);
void sh_wp_free(sh_wordpiece *wp);

/* Token count of the vocabulary (21128 for a Chinese BERT). */
size_t sh_wp_vocab_size(const sh_wordpiece *wp);

/* Id of a literal token, or -1. Exposed for tests and diagnostics. */
long sh_wp_token_id(sh_wordpiece *wp, const char *token);

/*
 * Encode `text` as [CLS] pieces [SEP], writing ids and the attention mask
 * (1 for real tokens, 0 for padding). Returns the number of tokens written
 * (at least 2) and 0 on failure. Long input is truncated to max_tokens.
 */
size_t sh_wp_encode(sh_wordpiece *wp, const char *text, size_t len, int64_t *ids,
                    int64_t *mask, size_t max_tokens);

#endif
