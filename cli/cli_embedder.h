/*
 * cli_embedder.h - model discovery and the tokenise-then-encode step.
 *
 * One place decides where the model lives and how text becomes a vector,
 * so `embed` and `find` cannot drift apart: a query and a document have to
 * be produced by the same model or the cosine is meaningless.
 */
#ifndef SKILLCTL_CLI_EMBEDDER_H
#define SKILLCTL_CLI_EMBEDDER_H

#include <stdint.h>

#include "sh_onnx.h"
#include "sh_wordpiece.h"

/* BGE's recommended query instruction; documents are encoded bare. */
#define CLI_EMBED_QUERY_PREFIX "\xe4\xb8\xba\xe8\xbf\x99\xe4\xb8\xaa\xe5\x8f\xa5\xe5\xad\x90\xe7\x94\x9f\xe6\x88\x90\xe8\xa1\xa8\xe7\xa4\xba\xe4\xbb\xa5\xe7\x94\xa8\xe4\xba\x8e\xe6\xa3\x80\xe7\xb4\xa2\xe7\x9b\xb8\xe5\x85\xb3\xe6\x96\x87\xe7\xab\xa0\xef\xbc\x9a"

typedef struct {
    sh_bert *session;
    sh_wordpiece *wp;
    int ready;
} cli_embedder;

/* Resolve the model directory: SKILLCTL_ONNX_MODEL, else
 * <store>/models/bge-small-zh-v1.5. malloc'd; caller frees. */
char *cli_embedder_model_dir(const char *store_home);

/*
 * Load the model named by SKILLCTL_ONNX_MODEL (or the store default).
 * Returns 1 when a session and vocabulary are ready to encode.
 */
int cli_embedder_open(cli_embedder *e, const char *store_home);
void cli_embedder_close(cli_embedder *e);

/* Embed one text. `is_query` prepends the BGE query instruction. */
int cli_embedder_embed(cli_embedder *e, const char *text, size_t len, int is_query,
                       float **out_vec, uint32_t *out_dim);

#endif
