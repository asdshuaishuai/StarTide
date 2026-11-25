/*
 * cli_embedder.c - model discovery and tokenise-then-encode.
 */
#include "cli_embedder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"

#define DEFAULT_MODEL_DIR "bge-small-zh-v1.5"

char *cli_embedder_model_dir(const char *store_home) {
    const char *env = getenv("SKILLCTL_ONNX_MODEL");

    if (env && env[0]) return strdup(env);
    {
        char *models = sh_path_join(store_home, "models");
        char *dir = sh_path_join(models, DEFAULT_MODEL_DIR);
        free(models);
        return dir;
    }
}

/* First of the candidate model files that exists. */
static char *resolve_model_file(const char *dir) {
    static const char *NAMES[] = {"model_qint8.onnx", "model_int8.onnx", "model.onnx", NULL};

    for (const char **n = NAMES; *n; n++) {
        char *path = sh_path_join(dir, *n);
        if (sh_exists(path)) return path;
        free(path);
    }
    return NULL;
}

void cli_embedder_close(cli_embedder *e) {
    if (!e) return;
    if (e->session) sh_bert_close(e->session);
    if (e->wp) sh_wp_free(e->wp);
    e->session = NULL;
    e->wp = NULL;
    e->ready = 0;
}

int cli_embedder_open(cli_embedder *e, const char *store_home) {
    char *dir;
    char *model;
    char *vocab;
    int ok = 0;

    if (!e) return 0;
    memset(e, 0, sizeof(*e));
    if (!sh_bert_available()) return 0;

    dir = cli_embedder_model_dir(store_home);
    if (!dir) return 0;
    model = resolve_model_file(dir);
    vocab = sh_path_join(dir, "vocab.txt");
    if (model && sh_exists(vocab)) {
        e->wp = sh_wp_load(vocab, 1);
        if (e->wp) e->session = sh_bert_open(model);
        if (e->session) {
            e->ready = 1;
            ok = 1;
        }
    }
    if (!ok) cli_embedder_close(e);
    free(model);
    free(vocab);
    free(dir);
    return ok;
}

int cli_embedder_embed(cli_embedder *e, const char *text, size_t len, int is_query,
                       float **out_vec, uint32_t *out_dim) {
    int64_t ids[SH_WP_MAX_TOKENS];
    int64_t mask[SH_WP_MAX_TOKENS];
    size_t n;
    char *joined = NULL;

    *out_vec = NULL;
    *out_dim = 0;
    if (!e || !e->ready || !text) return 0;

    if (is_query) {
        size_t plen = strlen(CLI_EMBED_QUERY_PREFIX);
        joined = malloc(plen + len + 1);
        if (!joined) return 0;
        memcpy(joined, CLI_EMBED_QUERY_PREFIX, plen);
        memcpy(joined + plen, text, len);
        joined[plen + len] = '\0';
        text = joined;
        len += plen;
    }

    n = sh_wp_encode(e->wp, text, len, ids, mask, SH_WP_MAX_TOKENS);
    free(joined);
    if (n == 0) return 0;
    return sh_bert_embed(e->session, ids, mask, n, out_vec, out_dim);
}
