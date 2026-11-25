/*
 * cli_embedder.h - model discovery and the tokenise-then-encode step.
 *
 * One backend family (ONNX Runtime, in-process, no Python) with two
 * tokenisers, because the two model families want different ones:
 *
 *   BERT + vocab.txt      -> WordPiece, BGE-style query instruction
 *   XLM-R + tokenizer.json -> Unigram (SentencePiece), E5 query:/passage:
 *
 * Which one is used is decided by what the model directory contains, so
 * swapping models is a file operation. `embed` and `find` share this code
 * so a query and a document can never come from different models.
 */
#ifndef SKILLCTL_CLI_EMBEDDER_H
#define SKILLCTL_CLI_EMBEDDER_H

#include <stdint.h>

#include "sh_llama.h"
#include "sh_onnx.h"
#include "sh_unigram.h"
#include "sh_wordpiece.h"

/* BGE's query instruction, for the WordPiece backend. */
#define CLI_EMBED_BGE_QUERY_PREFIX "\xe4\xb8\xba\xe8\xbf\x99\xe4\xb8\xaa\xe5\x8f\xa5\xe5\xad\x90\xe7\x94\x9f\xe6\x88\x90\xe8\xa1\xa8\xe7\xa4\xba\xe4\xbb\xa5\xe7\x94\xa8\xe4\xba\x8e\xe6\xa3\x80\xe7\xb4\xa2\xe7\x9b\xb8\xe5\x85\xb3\xe6\x96\x87\xe7\xab\xa0\xef\xbc\x9a"

/* E5 convention: every text carries one of these prefixes. */
#define CLI_EMBED_E5_QUERY_PREFIX "query: "
#define CLI_EMBED_E5_PASSAGE_PREFIX "passage: "

typedef struct {
    sh_gguf *gguf; /* backend 3: llama.cpp GGUF */
    sh_bert *session;
    sh_wordpiece *wp;
    sh_unigram *uni;
    int backend; /* 0 none, 1 wordpiece/bge, 2 unigram/e5, 3 gguf */
    int dim;
} cli_embedder;

/* Resolve a model directory: SKILLCTL_ONNX_MODEL, else the multilingual
 * store default, else the Chinese BERT default. malloc'd. */
char *cli_embedder_model_dir(const char *store_home);

/* Path of the first *.gguf under SKILLCTL_GGUF_MODEL (a file or directory),
 * else <store>/models/f2llm-80m-gguf. malloc'd, NULL when absent. */
char *cli_embedder_gguf_path(const char *store_home);

/* Open whichever model is present. Returns 1 on success. */
int cli_embedder_open(cli_embedder *e, const char *store_home);
void cli_embedder_close(cli_embedder *e);

/* "onnx-wordpiece", "onnx-unigram" or "none". */
const char *cli_embedder_backend(const cli_embedder *e);

/* Embed one text. `is_query` adds the backend's query prefix. */
int cli_embedder_embed(cli_embedder *e, const char *text, size_t len, int is_query,
                       float **out_vec, uint32_t *out_dim);

#endif
