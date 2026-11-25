/*
 * sh_onnx.h - BERT sentence embeddings through ONNX Runtime.
 *
 * The vector path runs in-process: no Python, no sidecar service, no shell
 * out to an embedder. ONNX Runtime is a C library, the model is a single
 * .onnx file, and the tokeniser is sh_wordpiece.c, so `skillctl embed` and
 * `skillctl find` are one binary plus one model file.
 *
 * This wrapper does the minimum a BERT encoder needs: create a session on
 * the CPU, feed input_ids/attention_mask/token_type_ids (whichever the
 * export declares), and pool the [CLS] token into an L2-normalised vector.
 * If the ONNX Runtime library is absent at link time the whole feature is
 * compiled out; if it is present but the model is missing, the caller
 * degrades to the lexical lists.
 */
#ifndef SKILLCTL_ONNX_H
#define SKILLCTL_ONNX_H

#include <stddef.h>
#include <stdint.h>

#ifdef SKILLCTL_NO_ONNX

typedef struct sh_bert sh_bert;

static inline sh_bert *sh_bert_open(const char *model_path) {
    (void)model_path;
    return NULL;
}
static inline void sh_bert_close(sh_bert *b) { (void)b; }
static inline int sh_bert_embed_pool(sh_bert *b, const int64_t *ids, const int64_t *mask,
                                     size_t n, int mean_pool, float **out_vec,
                                     uint32_t *out_dim) {
    (void)b;
    (void)ids;
    (void)mask;
    (void)n;
    (void)mean_pool;
    (void)out_vec;
    (void)out_dim;
    return 0;
}
static inline int sh_bert_embed(sh_bert *b, const int64_t *ids, const int64_t *mask, size_t n,
                                float **out_vec, uint32_t *out_dim) {
    (void)b;
    (void)ids;
    (void)mask;
    (void)n;
    (void)out_vec;
    (void)out_dim;
    return 0;
}
static inline int sh_bert_available(void) { return 0; }

#else

typedef struct sh_bert sh_bert;

/* 1 when this build was compiled with ONNX Runtime support. */
int sh_bert_available(void);

/* Open a session. Returns NULL when the file is missing or the runtime
 * refuses it. */
sh_bert *sh_bert_open(const char *model_path);
void sh_bert_close(sh_bert *b);

/*
 * Run one sequence and return its pooled CLS embedding, L2-normalised.
 * Allocates *out_vec (free with free); sets *out_dim. Returns 1 on success.
 */
int sh_bert_embed(sh_bert *b, const int64_t *ids, const int64_t *mask, size_t n,
                  float **out_vec, uint32_t *out_dim);

/*
 * Pooling choice matters: BGE-family encoders are trained on the [CLS] token,
 * E5-family on the mean of the token states. Using the wrong one degrades
 * similarity without failing, so the caller names it explicitly.
 */
int sh_bert_embed_pool(sh_bert *b, const int64_t *ids, const int64_t *mask, size_t n,
                       int mean_pool, float **out_vec, uint32_t *out_dim);

#endif

#endif
