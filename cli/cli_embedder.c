/*
 * cli_embedder.c - model discovery and tokenise-then-encode.
 */
#include "cli_embedder.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"

#define UNIGRAM_MODEL_DIR "multilingual-e5-small-onnx"
#define WORDPIECE_MODEL_DIR "bge-small-zh-v1.5"
#define GGUF_MODEL_DIR "f2llm-80m-gguf"

/* First of the candidate ONNX files that exists. */
static char *resolve_onnx_file(const char *dir) {
    static const char *NAMES[] = {"model_int8.onnx",      "model_qint8.onnx",
                                  "model_quantized.onnx", "model_int8_full.onnx",
                                  "model.onnx",           NULL};

    for (const char **n = NAMES; *n; n++) {
        char *path = sh_path_join(dir, *n);
        if (sh_exists(path)) return path;
        free(path);
    }
    return NULL;
}

/* First *.gguf in a directory. */
static char *first_gguf_in(const char *dir) {
    DIR *d;
    struct dirent *ent;
    char *found = NULL;

    d = opendir(dir);
    if (!d) return NULL;
    while (!found && (ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n > 5 && strcmp(ent->d_name + n - 5, ".gguf") == 0)
            found = sh_path_join(dir, ent->d_name);
    }
    closedir(d);
    return found;
}

char *cli_embedder_gguf_path(const char *store_home) {
    const char *env = getenv("SKILLCTL_GGUF_MODEL");
    char *models;
    char *dir;
    char *found;

    if (env && env[0]) {
        if (strstr(env, ".gguf")) {
            FILE *f = fopen(env, "rb");
            if (f) {
                fclose(f);
                return strdup(env);
            }
            return NULL;
        }
        return first_gguf_in(env);
    }
    models = sh_path_join(store_home, "models");
    dir = sh_path_join(models, GGUF_MODEL_DIR);
    free(models);
    found = first_gguf_in(dir);
    free(dir);
    return found;
}

char *cli_embedder_model_dir(const char *store_home) {
    const char *env = getenv("SKILLCTL_ONNX_MODEL");
    char *models;
    char *multi;
    char *zh;

    if (env && env[0]) return strdup(env);

    models = sh_path_join(store_home, "models");
    multi = sh_path_join(models, UNIGRAM_MODEL_DIR);
    zh = sh_path_join(models, WORDPIECE_MODEL_DIR);
    free(models);
    /* Prefer the multilingual directory when it actually holds a model. */
    if (sh_exists(multi)) {
        char *probe = resolve_onnx_file(multi);
        if (probe) {
            free(probe);
            free(zh);
            return multi;
        }
        free(probe);
    }
    free(multi);
    return zh;
}

void cli_embedder_close(cli_embedder *e) {
    if (!e) return;
    if (e->gguf) sh_gguf_close(e->gguf);
    if (e->session) sh_bert_close(e->session);
    if (e->wp) sh_wp_free(e->wp);
    if (e->uni) sh_uni_free(e->uni);
    memset(e, 0, sizeof(*e));
}

const char *cli_embedder_backend(const cli_embedder *e) {
    if (!e) return "none";
    if (e->backend == 3) return "gguf";
    if (e->backend == 2) return "onnx-unigram";
    if (e->backend == 1) return "onnx-wordpiece";
    return "none";
}

int cli_embedder_open(cli_embedder *e, const char *store_home) {
    char *dir;
    char *model;
    char *tokenizer_json;
    char *vocab;
    int ok = 0;

    if (!e) return 0;
    memset(e, 0, sizeof(*e));

    /* GGUF first: it is the multilingual decoder model, and llama.cpp
     * brings the tokeniser it needs. */
    {
        char *gguf = cli_embedder_gguf_path(store_home);
        if (gguf) {
            e->gguf = sh_gguf_open(gguf);
            free(gguf);
            if (e->gguf) {
                e->backend = 3;
                return 1;
            }
        }
    }

    if (!sh_bert_available()) return 0;

    dir = cli_embedder_model_dir(store_home);
    if (!dir) return 0;
    model = resolve_onnx_file(dir);
    if (!model) {
        free(dir);
        return 0;
    }
    tokenizer_json = sh_path_join(dir, "tokenizer.json");
    vocab = sh_path_join(dir, "vocab.txt");

    if (sh_exists(tokenizer_json)) {
        e->uni = sh_uni_load(tokenizer_json);
        if (e->uni) e->session = sh_bert_open(model);
        if (e->session) {
            e->backend = 2;
            ok = 1;
        }
    } else if (sh_exists(vocab)) {
        e->wp = sh_wp_load(vocab, 1);
        if (e->wp) e->session = sh_bert_open(model);
        if (e->session) {
            e->backend = 1;
            ok = 1;
        }
    }

    if (!ok) cli_embedder_close(e);
    free(model);
    free(tokenizer_json);
    free(vocab);
    free(dir);
    return ok;
}

/* Prefix one text with a convention string. */
static char *prefixed(const char *prefix, const char *text, size_t len, size_t *out_len) {
    size_t plen = strlen(prefix);
    char *joined = malloc(plen + len + 1);

    if (!joined) return NULL;
    memcpy(joined, prefix, plen);
    memcpy(joined + plen, text, len);
    joined[plen + len] = '\0';
    *out_len = plen + len;
    return joined;
}

int cli_embedder_embed(cli_embedder *e, const char *text, size_t len, int is_query,
                       float **out_vec, uint32_t *out_dim) {
    int64_t ids[SH_UNI_MAX_TOKENS];
    int64_t mask[SH_UNI_MAX_TOKENS];
    char *joined = NULL;
    size_t joined_len = 0;
    size_t n;

    *out_vec = NULL;
    *out_dim = 0;
    if (!e || e->backend == 0 || !text) return 0;

    if (e->backend == 3) {
        /* F2LLM/Qwen3 encoders are symmetric: no query instruction. */
        return sh_gguf_embed(e->gguf, text, len, out_vec, out_dim);
    }

    if (e->backend == 2) {
        joined = prefixed(is_query ? CLI_EMBED_E5_QUERY_PREFIX : CLI_EMBED_E5_PASSAGE_PREFIX,
                          text, len, &joined_len);
        if (!joined) return 0;
        n = sh_uni_encode(e->uni, joined, joined_len, ids, mask, SH_UNI_MAX_TOKENS);
        free(joined);
        if (n == 0) return 0;
        /* E5 is trained with mean pooling. */
        return sh_bert_embed_pool(e->session, ids, mask, n, 1, out_vec, out_dim);
    }

    if (is_query) {
        joined = prefixed(CLI_EMBED_BGE_QUERY_PREFIX, text, len, &joined_len);
        if (!joined) return 0;
        text = joined;
        len = joined_len;
    }
    n = sh_wp_encode(e->wp, text, len, ids, mask, SH_WP_MAX_TOKENS);
    free(joined);
    if (n == 0) return 0;
    return sh_bert_embed(e->session, ids, mask, n, out_vec, out_dim);
}
