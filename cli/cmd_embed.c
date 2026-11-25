/*
 * cmd_embed.c - the embed command.
 *
 * The engine ships no model. SKILLCTL_EMBEDDER names a command that reads
 * the skill text on stdin and prints a JSON array of floats, so the choice
 * of embedding model stays the operator's (a local runner, an API shim,
 * anything). Vectors are keyed by the unit's content hash, which is what
 * makes them safely incremental: a unit that has not changed keeps its
 * vector, and a changed unit is simply re-embedded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "sh_fs.h"
#include "sh_index.h"
#include "sh_json.h"
#include "cli_embedder.h"

static char *vectors_path(const char *store_home) {
    char *dir = sh_index_dir(store_home);
    char *path = sh_path_join(dir, "vectors.bin");
    free(dir);
    return path;
}

/* Concatenate the fields the router embeds, matching what the query side
 * feeds the embedder so both land in the same semantic space. */
static char *compose_text(const sh_index_fields *f) {
    size_t n = strlen(f->name ? f->name : "") + strlen(f->desc ? f->desc : "") +
               strlen(f->profile ? f->profile : "") +
               strlen(f->body ? f->body : "") + 8;
    char *out = malloc(n);
    size_t at = 0;

    if (!out) return NULL;
    /* Body is capped by the caller; profile carries the headings. Name and
     * description lead so short, high-signal text is never truncated away. */
    if (f->name && f->name[0]) {
        size_t l = strlen(f->name);
        memcpy(out + at, f->name, l);
        at += l;
        out[at++] = '\n';
    }
    if (f->desc && f->desc[0]) {
        size_t l = strlen(f->desc);
        memcpy(out + at, f->desc, l);
        at += l;
        out[at++] = '\n';
    }
    if (f->profile && f->profile[0]) {
        size_t l = strlen(f->profile);
        memcpy(out + at, f->profile, l);
        at += l;
        out[at++] = '\n';
    }
    if (f->body && f->body[0]) {
        size_t l = strlen(f->body);
        memcpy(out + at, f->body, l);
        at += l;
    }
    out[at] = '\0';
    return out;
}

int cmd_embed(int argc, char **argv) {
    const char *embedder = getenv("SKILLCTL_EMBEDDER");
    cli_embedder local;
    int have_local;
    char *catalog_path;
    char *out_path;
    char *index_dir;
    sh_catalog cat;
    sh_vectors vecs;
    int ok = 1;

    (void)argc;
    (void)argv;

    /* Prefer the in-process ONNX model; fall back to an external command
     * only when the operator configured one explicitly. */
    memset(&local, 0, sizeof(local));
    have_local = cli_embedder_open(&local, cli_store_home());
    if (!have_local && (!embedder || !embedder[0])) {
        char *dir = cli_embedder_model_dir(cli_store_home());
        cli_error("skillctl: no local ONNX model found");
        cli_error("expected: %s/{model_qint8.onnx,vocab.txt}", dir ? dir : "?");
        cli_error("or set SKILLCTL_ONNX_MODEL to a directory holding them");
        cli_error("or set SKILLCTL_EMBEDDER to an external embedder command");
        free(dir);
        return 1;
    }

    catalog_path = sh_store_catalog_path(cli_store_home());
    index_dir = sh_index_dir(cli_store_home());
    out_path = vectors_path(cli_store_home());
    memset(&cat, 0, sizeof(cat));
    memset(&vecs, 0, sizeof(vecs));

    if (!sh_catalog_load(&cat, catalog_path) || cat.count == 0) {
        cli_error("skillctl: the catalog is empty; run 'skillctl scan' first");
        free(catalog_path);
        free(index_dir);
        free(out_path);
        sh_catalog_free(&cat);
        return 1;
    }
    if (sh_mkdir_p(index_dir) != SH_FS_OK) {
        cli_error("skillctl: cannot create %s", index_dir);
        free(catalog_path);
        free(index_dir);
        free(out_path);
        sh_catalog_free(&cat);
        return 1;
    }

    cli_note("embedding %zu skills", cat.count);

    for (size_t i = 0; i < cat.count; i++) {
        const sh_catalog_entry *e = &cat.entries[i];
        sh_index_fields f;
        char *text;
        float *vec = NULL;
        uint32_t dim = 0;

        memset(&f, 0, sizeof(f));
        if (!sh_index_fields_build(cli_store_home(), e, &f)) {
            cli_verbose_note("skip %s: cannot read its fields", e->name ? e->name : e->id);
            continue;
        }
        text = compose_text(&f);
        sh_index_fields_free(&f);
        if (!text) {
            ok = 0;
            break;
        }
        if (have_local) {
            ok = cli_embedder_embed(&local, text, strlen(text), 0, &vec, &dim);
        } else {
            ok = sh_index_embed_text(embedder, index_dir, text, &vec, &dim);
        }
        if (!ok) {
            cli_error("skillctl: embedding failed on %s", e->name ? e->name : e->id);
            free(text);
            break;
        }
        free(text);

        if (dim != vecs.dim && vecs.count > 0) {
            cli_error("skillctl: embedding dimension changed from %u to %u at %s",
                      vecs.dim, dim, e->name ? e->name : e->id);
            free(vec);
            ok = 0;
            break;
        }

        if (vecs.count == 0) {
            vecs.dim = dim;
            vecs.ids = calloc((size_t)cat.count, 64);
            vecs.vec = malloc((size_t)cat.count * dim * sizeof(float));
            if (!vecs.ids || !vecs.vec) {
                free(vec);
                ok = 0;
                break;
            }
        }
        /* The unit id is a 64-hex content hash; copy exactly 64 bytes. */
        memcpy(vecs.ids + (size_t)vecs.count * 64, e->id, 64);
        memcpy(vecs.vec + (size_t)vecs.count * dim, vec, dim * sizeof(float));
        vecs.count++;
        free(vec);

        if ((i + 1) % 100 == 0) cli_note("  %zu / %zu", i + 1, cat.count);
    }

    if (ok && vecs.count > 0) ok = sh_vectors_write(out_path, &vecs);

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "ok");
        sh_jw_bool(&w, ok);
        sh_jw_key(&w, "vectors");
        sh_jw_num(&w, (long long)vecs.count);
        sh_jw_key(&w, "dim");
        sh_jw_num(&w, (long long)vecs.dim);
        sh_jw_obj_end(&w);
        fputs(sh_jw_text(&w), stdout);
        fputc('\n', stdout);
        sh_jw_free(&w);
    } else if (ok) {
        cli_note("wrote %zu vectors (dim %u) to %s", vecs.count, vecs.dim, out_path);
    } else {
        cli_error("skillctl: embed failed; existing vectors left untouched");
    }

    cli_embedder_close(&local);
    sh_vectors_free(&vecs);
    sh_catalog_free(&cat);
    free(catalog_path);
    free(index_dir);
    free(out_path);
    return ok ? 0 : 1;
}
