/*
 * sh_unigram.h - Unigram (SentencePiece) tokenisation from tokenizer.json.
 *
 * The multilingual encoders worth using (multilingual-e5, XLM-R, LaBSE) do
 * not ship a vocab.txt: their tokeniser is SentencePiece, which is a
 * Unigram model - a vocabulary of pieces with log-probabilities, segmented
 * by Viterbi rather than by greedy longest match. A WordPiece reader cannot
 * load those, and shelling out to Python is off the table, so this reads
 * tokenizer.json directly and segments in C.
 *
 * Scope: the Metaspace normalisation XLM-R uses (spaces become U+2581,
 * prepended to the text) and Viterbi segmentation with an unknown penalty.
 * Unicode NFKC normalisation is not applied; for Latin and CJK text the
 * pieces are the same, which is the range this router works in.
 */
#ifndef SKILLCTL_UNIGRAM_H
#define SKILLCTL_UNIGRAM_H

#include <stddef.h>
#include <stdint.h>

#define SH_UNI_MAX_TOKENS 512

typedef struct sh_unigram sh_unigram;

/* Load tokenizer.json. Returns NULL when the file is unreadable or has no
 * Unigram vocabulary. */
sh_unigram *sh_uni_load(const char *tokenizer_json_path);
void sh_uni_free(sh_unigram *u);

size_t sh_uni_vocab_size(const sh_unigram *u);
long sh_uni_unk_id(sh_unigram *u);
long sh_uni_id_of(sh_unigram *u, const char *piece);

/*
 * Encode as <s> pieces </s>, writing ids and the attention mask.
 * Returns the token count (at least 2), or 0 on failure.
 */
size_t sh_uni_encode(sh_unigram *u, const char *text, size_t len, int64_t *ids, int64_t *mask,
                     size_t max_tokens);

#endif
