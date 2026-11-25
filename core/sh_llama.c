/*
 * sh_llama.c - GGUF embeddings through llama.cpp (see sh_llama.h).
 */
#include "sh_llama.h"

#ifndef SKILLCTL_NO_LLAMA

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <llama.h>

#define GGUF_MAX_TOKENS 512
#define GGUF_TOKEN_CAP 2048

struct sh_gguf {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    int32_t n_embd;
};

/* llama.cpp narrates backend loading on every open; a CLI that prints one
 * line per query should not repeat that. Errors still surface. */
static void gguf_log(enum ggml_log_level level, const char *text, void *user_data) {
    (void)user_data;
    if (level == GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

int sh_gguf_available(void) { return 1; }

int sh_gguf_pooling_type(sh_gguf *g) { return g && g->ctx ? (int)llama_pooling_type(g->ctx) : 0; }
int sh_gguf_dim(sh_gguf *g) { return g ? g->n_embd : 0; }

void sh_gguf_close(sh_gguf *g) {
    if (!g) return;
    if (g->ctx) llama_free(g->ctx);
    if (g->model) llama_model_free(g->model);
    llama_backend_free();
    free(g);
}

sh_gguf *sh_gguf_open(const char *model_path) {
    sh_gguf *g;
    struct llama_model_params mp;
    struct llama_context_params cp;

    if (!model_path || !model_path[0]) return NULL;

    g = calloc(1, sizeof(sh_gguf));
    if (!g) return NULL;

    llama_log_set(gguf_log, NULL);
    llama_backend_init();

    mp = llama_model_default_params();
    /* CPU only: a 80M-parameter model does not need Metal, and skipping the
     * GPU backend keeps startup predictable for a one-shot CLI. */
    mp.n_gpu_layers = 0;
    g->model = llama_model_load_from_file(model_path, mp);
    if (!g->model) goto fail;

    cp = llama_context_default_params();
    cp.n_ctx = GGUF_MAX_TOKENS;
    cp.n_batch = GGUF_MAX_TOKENS;
    cp.n_ubatch = GGUF_MAX_TOKENS;
    cp.embeddings = true;
    /* Decoder embedding models (Qwen3-Embedding, F2LLM-v2) pool the last
     * token and keep causal attention - do not switch to NON_CAUSAL here,
     * that is BERT's mode and it changes the encoder's meaning. */
    cp.pooling_type = LLAMA_POOLING_TYPE_LAST;
    g->ctx = llama_init_from_model(g->model, cp);
    if (!g->ctx) goto fail;

    g->vocab = llama_model_get_vocab(g->model);
    g->n_embd = llama_model_n_embd(g->model);
    if (!g->vocab || g->n_embd <= 0) goto fail;
    return g;

fail:
    sh_gguf_close(g);
    return NULL;
}

/*
 * Tokenise, truncating on a token boundary, keeping the HEAD.
 *
 * Last-token pooling pools the final token of the kept prefix, and with
 * causal attention that token has already attended to everything before
 * it - so the prefix is summarised. Keeping the tail instead would throw
 * away the name and description that lead the text and summarise only its
 * ending.
 */
static int32_t gguf_tokenize(sh_gguf *g, const char *text, size_t len, llama_token *out) {
    llama_token *scratch = malloc(GGUF_TOKEN_CAP * sizeof(llama_token));
    int32_t n;
    int32_t result;

    if (!scratch) return -1;
    n = llama_tokenize(g->vocab, text, (int32_t)len, scratch, GGUF_TOKEN_CAP, true, false);
    if (n < 0) {
        free(scratch);
        return -1;
    }
    if (n <= GGUF_MAX_TOKENS) {
        memcpy(out, scratch, (size_t)n * sizeof(llama_token));
        result = n;
    } else {
        memcpy(out, scratch, (size_t)GGUF_MAX_TOKENS * sizeof(llama_token));
        result = GGUF_MAX_TOKENS;
    }
    free(scratch);
    return result;
}

int sh_gguf_embed(sh_gguf *g, const char *text, size_t len, float **out_vec, uint32_t *out_dim) {
    llama_token tokens[GGUF_MAX_TOKENS];
    struct llama_batch batch;
    const float *emb;
    float *vec;
    double norm = 0.0;
    int32_t n;
    int32_t dim;

    if (out_vec) *out_vec = NULL;
    if (out_dim) *out_dim = 0;
    if (!g || !text || !out_vec || !out_dim) return 0;

    n = gguf_tokenize(g, text, len, tokens);
    if (n <= 0) return 0;

    /* Reset the cache so every call starts at position 0: llama.cpp tracks
     * positions automatically across decodes and would otherwise append. */
    llama_memory_clear(llama_get_memory(g->ctx), true);

    /*
     * An explicit batch rather than llama_batch_get_one: that helper marks
     * only the last token for output, which is right for logits and wrong
     * for pooled embeddings - the pool needs every token's state.
     */
    memset(&batch, 0, sizeof(batch));
    batch = llama_batch_init(n, 0, 1);
    if (!batch.token || !batch.pos || !batch.n_seq_id || !batch.seq_id || !batch.logits) {
        llama_batch_free(batch);
        return 0;
    }
    batch.n_tokens = n;
    for (int32_t i = 0; i < n; i++) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = 1;
    }
    if (llama_decode(g->ctx, batch) != 0) {
        llama_batch_free(batch);
        return 0;
    }
    llama_batch_free(batch);

    emb = llama_get_embeddings_seq(g->ctx, 0);
    if (!emb) emb = llama_get_embeddings(g->ctx);
    if (!emb) return 0;

    dim = g->n_embd;
    vec = malloc((size_t)dim * sizeof(float));
    if (!vec) return 0;
    memcpy(vec, emb, (size_t)dim * sizeof(float));

    for (int32_t i = 0; i < dim; i++) norm += (double)vec[i] * (double)vec[i];
    norm = sqrt(norm);
    if (norm > 0.0)
        for (int32_t i = 0; i < dim; i++) vec[i] = (float)((double)vec[i] / norm);

    *out_vec = vec;
    *out_dim = (uint32_t)dim;
    return 1;
}

#endif /* SKILLCTL_NO_LLAMA */
