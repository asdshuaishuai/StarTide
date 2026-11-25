/*
 * sh_onnx.c - ONNX Runtime session + CLS pooling (see sh_onnx.h).
 */
#include "sh_onnx.h"

#ifndef SKILLCTL_NO_ONNX

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <onnxruntime/onnxruntime_c_api.h>

#define BERT_MAX_INPUTS 8

struct sh_bert {
    const OrtApi *api;
    OrtEnv *env;
    OrtSession *session;
    OrtMemoryInfo *mem;
    OrtAllocator *alloc;
    size_t input_count;
    char *input_names[BERT_MAX_INPUTS];
    char *output_name;
};

int sh_bert_available(void) { return 1; }

void sh_bert_close(sh_bert *b) {
    if (!b) return;
    if (b->api) {
        for (size_t i = 0; i < b->input_count; i++)
            if (b->input_names[i]) (void)b->api->AllocatorFree(b->alloc, b->input_names[i]);
        if (b->output_name) (void)b->api->AllocatorFree(b->alloc, b->output_name);
        if (b->mem) (void)b->api->ReleaseMemoryInfo(b->mem);
        if (b->session) (void)b->api->ReleaseSession(b->session);
        if (b->env) (void)b->api->ReleaseEnv(b->env);
    }
    free(b);
}

sh_bert *sh_bert_open(const char *model_path) {
    sh_bert *b;
    OrtSessionOptions *opts = NULL;
    const OrtApi *api;

    if (!model_path || !model_path[0]) return NULL;
    api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!api) return NULL;

    b = calloc(1, sizeof(sh_bert));
    if (!b) return NULL;
    b->api = api;

    if (api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "skillctl", &b->env) != NULL) {
        free(b);
        return NULL;
    }
    if (api->CreateSessionOptions(&opts) != NULL) {
        sh_bert_close(b);
        return NULL;
    }
    /* Both return an ORT status; neither has a failure mode worth aborting
     * over, and the cast keeps -Wall clean against ORT's [[nodiscard]]. */
    (void)api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    (void)api->SetIntraOpNumThreads(opts, 1);

    if (api->CreateSession(b->env, model_path, opts, &b->session) != NULL) {
        api->ReleaseSessionOptions(opts);
        sh_bert_close(b);
        return NULL;
    }
    api->ReleaseSessionOptions(opts);

    if (api->GetAllocatorWithDefaultOptions(&b->alloc) != NULL ||
        api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &b->mem) != NULL) {
        sh_bert_close(b);
        return NULL;
    }

    if (api->SessionGetInputCount(b->session, &b->input_count) != NULL ||
        b->input_count == 0 || b->input_count > BERT_MAX_INPUTS) {
        sh_bert_close(b);
        return NULL;
    }
    for (size_t i = 0; i < b->input_count; i++) {
        if (api->SessionGetInputName(b->session, i, b->alloc, &b->input_names[i]) != NULL) {
            sh_bert_close(b);
            return NULL;
        }
    }
    {
        size_t out_count = 0;
        if (api->SessionGetOutputCount(b->session, &out_count) != NULL || out_count == 0 ||
            api->SessionGetOutputName(b->session, 0, b->alloc, &b->output_name) != NULL) {
            sh_bert_close(b);
            return NULL;
        }
    }
    return b;
}

/* Build the int64 tensor for one named input. `zeros` is the shared
 * scratch buffer used for token_type_ids and any unknown input. */
static OrtValue *make_input(sh_bert *b, const char *name, const int64_t *ids,
                            const int64_t *mask, const int64_t *zeros, size_t n) {
    const int64_t *src = zeros;
    int64_t dims[2];
    OrtValue *value = NULL;

    if (strstr(name, "input_ids")) src = ids;
    else if (strstr(name, "attention_mask")) src = mask;

    dims[0] = 1;
    dims[1] = (int64_t)n;
    if (b->api->CreateTensorWithDataAsOrtValue(b->mem, (void *)src, n * sizeof(int64_t), dims,
                                              2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                              &value) != NULL)
        return NULL;
    return value;
}

int sh_bert_embed(sh_bert *b, const int64_t *ids, const int64_t *mask, size_t n,
                  float **out_vec, uint32_t *out_dim) {
    OrtValue *inputs[BERT_MAX_INPUTS];
    OrtValue *output = NULL;
    OrtTensorTypeAndShapeInfo *info = NULL;
    int64_t *zeros = NULL;
    int64_t *shape = NULL;
    const float *data = NULL;
    size_t rank = 0;
    size_t total = 0;
    float *vec = NULL;
    double norm = 0.0;
    uint32_t dim;
    int ok = 0;

    if (out_vec) *out_vec = NULL;
    if (out_dim) *out_dim = 0;
    if (!b || !ids || !mask || n == 0 || !out_vec || !out_dim) return 0;

    memset(inputs, 0, sizeof(inputs));
    zeros = calloc(n, sizeof(int64_t));
    if (!zeros) return 0;

    for (size_t i = 0; i < b->input_count; i++) {
        inputs[i] = make_input(b, b->input_names[i], ids, mask, zeros, n);
        if (!inputs[i]) goto cleanup;
    }

    if (b->api->Run(b->session, NULL, (const char *const *)b->input_names,
                    (const OrtValue *const *)inputs, b->input_count,
                    (const char *const *)&b->output_name, 1, &output) != NULL)
        goto cleanup;

    if (b->api->GetTensorTypeAndShape(output, &info) != NULL) goto cleanup;
    if (b->api->GetDimensionsCount(info, &rank) != NULL) goto cleanup;
    if (b->api->GetTensorShapeElementCount(info, &total) != NULL) goto cleanup;
    if (rank < 2 || rank > 3) goto cleanup;

    shape = calloc(rank, sizeof(int64_t));
    if (!shape) goto cleanup;
    if (b->api->GetDimensions(info, shape, rank) != NULL) goto cleanup;
    dim = (uint32_t)shape[rank - 1];
    if (dim == 0 || total < dim) goto cleanup;

    if (b->api->GetTensorMutableData(output, (void **)&data) != NULL || !data) goto cleanup;

    /* Rank 3 is [batch, seq, hidden]: the CLS token is the first row, which
     * is what BERT-derived sentence encoders are trained to pool. Rank 2 is
     * already [batch, hidden]. */
    vec = malloc((size_t)dim * sizeof(float));
    if (!vec) goto cleanup;
    memcpy(vec, data, (size_t)dim * sizeof(float));

    for (uint32_t i = 0; i < dim; i++) norm += (double)vec[i] * (double)vec[i];
    norm = sqrt(norm);
    if (norm > 0.0)
        for (uint32_t i = 0; i < dim; i++) vec[i] = (float)((double)vec[i] / norm);

    *out_vec = vec;
    *out_dim = dim;
    vec = NULL;
    ok = 1;

cleanup:
    free(vec);
    free(shape);
    free(zeros);
    if (info) b->api->ReleaseTensorTypeAndShapeInfo(info);
    if (output) b->api->ReleaseValue(output);
    for (size_t i = 0; i < b->input_count; i++)
        if (inputs[i]) b->api->ReleaseValue(inputs[i]);
    return ok;
}

#endif /* SKILLCTL_NO_ONNX */
