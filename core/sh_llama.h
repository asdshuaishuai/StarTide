/*
 * sh_llama.h - sentence embeddings from a GGUF model via llama.cpp.
 *
 * The multilingual embedding models worth running are decoder-based
 * (Qwen3-Embedding and its pruned variants such as F2LLM-v2), and llama.cpp
 * ships the whole stack in C: GGUF loading, a byte-level BPE tokeniser and
 * last-token pooling. That is why this backend exists - it needs no
 * tokeniser written here, and the pooling it needs (last token) is the one
 * llama.cpp exercises for every decoder embedding model.
 *
 * No Python at runtime; llama.cpp is linked as a C library.
 */
#ifndef SKILLCTL_LLAMA_H
#define SKILLCTL_LLAMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef SKILLCTL_NO_LLAMA

typedef struct sh_gguf sh_gguf;

static inline int sh_gguf_available(void) { return 0; }
static inline sh_gguf *sh_gguf_open(const char *model_path) {
    (void)model_path;
    return NULL;
}
static inline void sh_gguf_close(sh_gguf *g) { (void)g; }
static inline int sh_gguf_embed(sh_gguf *g, const char *text, size_t len, float **out_vec,
                                uint32_t *out_dim) {
    (void)g;
    (void)text;
    (void)len;
    (void)out_vec;
    (void)out_dim;
    return 0;
}
static inline int sh_gguf_pooling_type(sh_gguf *g) { (void)g; return 0; }
static inline int sh_gguf_dim(sh_gguf *g) { (void)g; return 0; }

#else

typedef struct sh_gguf sh_gguf;

int sh_gguf_available(void);

/* Load a GGUF model and open a 512-token embedding context. NULL on failure. */
sh_gguf *sh_gguf_open(const char *model_path);
void sh_gguf_close(sh_gguf *g);

/*
 * Embed one text. The tokeniser inside the model handles segmentation, and
 * the pooled (last-token for decoder encoders) vector comes back
 * L2-normalised. Allocates *out_vec; returns 1 on success.
 */
int sh_gguf_embed(sh_gguf *g, const char *text, size_t len, float **out_vec, uint32_t *out_dim);

/* Effective pooling type (0 none, 1 mean, 2 cls, 3 last) and model width. */
int sh_gguf_pooling_type(sh_gguf *g);
int sh_gguf_dim(sh_gguf *g);

#endif

#endif
