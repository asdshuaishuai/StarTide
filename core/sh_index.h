/*
 * sh_index.h - the on-disk skill index: BM25F postings, deterministic
 * document enrichment and (optional) dense vectors.
 *
 * Why an index at all? SkillRouter (2603.22455) measures plain BM25F over
 * metadata at Hit@1 0.31-0.35 while the same catalog with skill bodies
 * reaches far higher: dropping the body costs 29-44pp. So the index keeps
 * five fields - name, description, profile (deterministically extracted
 * from the body), body and path - and scores them with BM25F. ToolRet
 * (ACL 2025) adds the other half of the recipe: lexical and dense
 * retrieval are fused with RRF (k=60) because query/tool wording overlaps
 * so little (ROUGE-L 0.06). Both halves live here; sh_route.c fuses them.
 *
 * On-disk layout (all little-endian, all written to a temp file and
 * renamed into place, meta.json last so it acts as the commit stamp):
 *
 *   index/meta.json     format_version, doc_count, term_count,
 *                       catalog_entries, catalog_mtime, built_at, avg_len
 *   index/terms.bin     term_len(u16) term df(u32) off(u32) len(u32),
 *                       ordered by term bytes (binary searchable)
 *   index/postings.bin  per term: doc_idx varint + five field tf varints
 *   index/docs.txt      idx TAB unit_id TAB name TAB variants TAB len[0..4]
 *                       (the first three columns are the debug identity;
 *                       the trailing columns carry the bookkeeping BM25F
 *                       needs and are optional on read)
 *   index/vectors.bin   "SHVEC1" u32 count u32 dim, then count records of
 *                       64-byte id + dim f32 (L2 normalised)
 *
 * A full rebuild is the only rebuild: an incremental delta segment over
 * ~800 documents does not pay for its complexity, and a rebuild is a
 * second of CPU.  --if-stale skips the work when the catalog's entry
 * count and mtime are unchanged.
 */
#ifndef SKILLCTL_INDEX_H
#define SKILLCTL_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "sh_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH_INDEX_FORMAT_VERSION "1"
#define SH_INDEX_BODY_CAP (16 * 1024)
#define SH_INDEX_K1 1.2
#define SH_INDEX_RRF_K 60

typedef enum {
    SH_INDEX_FIELD_NAME = 0,
    SH_INDEX_FIELD_DESC = 1,
    SH_INDEX_FIELD_PROFILE = 2,
    SH_INDEX_FIELD_BODY = 3,
    SH_INDEX_FIELD_PATH = 4,
    SH_INDEX_FIELD_COUNT = 5
} sh_index_field;

/* Field weights and length-normalisation constants (BM25F, Robertson &
 * Zaragoza 2004).  Names are the strongest evidence, paths the weakest. */
extern const double sh_index_field_weight[SH_INDEX_FIELD_COUNT];
extern const double sh_index_field_b[SH_INDEX_FIELD_COUNT];

/* One term of the dictionary. */
typedef struct {
    char *term;
    uint32_t df;
    uint32_t postings_off;
    uint32_t postings_len;
} sh_index_term;

/* One indexed document: a group of near-identical skill units. */
typedef struct {
    char *id;   /* representative unit id */
    char *name; /* representative name */
    size_t variants;
    uint32_t len[SH_INDEX_FIELD_COUNT];
} sh_index_doc;

typedef struct {
    int loaded;
    char *dir;
    uint32_t doc_count;
    uint32_t term_count;
    uint32_t catalog_entries;
    int64_t built_at;
    int64_t catalog_mtime;
    double avg_len[SH_INDEX_FIELD_COUNT];
    sh_index_term *terms; /* sorted by term bytes */
    unsigned char *postings;
    size_t postings_size;
    sh_index_doc *docs;
} sh_index;

/* index/ under the store home.  malloc'd; caller frees. */
char *sh_index_dir(const char *store_home);

/* Load every file.  Returns 1 on success; on failure the struct is reset
 * and 0 is returned (a missing index is not an error to report). */
int sh_index_load(const char *dir, sh_index *out);
void sh_index_free(sh_index *idx);

typedef struct {
    size_t docs;
    size_t terms;
    size_t postings;
    size_t merged; /* catalog entries folded into another document */
    double seconds;
} sh_index_build_stats;

/*
 * Read SKILL.md bodies, dedupe near-identical variants, tokenise the five
 * fields and rewrite the whole index.  `store_home` locates the managed
 * copies; entries that are not in the store fall back to their source
 * path.  `catalog_path` is only used to stamp the build time and mtime
 * that --if-stale compares against.  Returns 1 on success.
 */
int sh_index_build(const char *dir, const char *store_home, const char *catalog_path,
                   const sh_catalog *cat, sh_index_build_stats *stats);

/*
 * 1 when the catalog's entry count or mtime differs from what the index
 * was built from (or when the index is missing or unreadable).
 */
int sh_index_is_stale(const char *dir, const char *catalog_path, size_t catalog_entries);

/* ------------------------------------------------------------ search */

typedef struct {
    size_t doc;
    double score;
} sh_index_scored;

/* IDF = ln(1 + (N - df + 0.5) / (df + 0.5)). */
double sh_index_idf(uint32_t n, uint32_t df);

/* BM25F contribution of one term:
 *   tf~ = sum_f w_f * tf_f / (1 - b_f + b_f * len_f / avg_f)
 *   score = IDF * tf~ / (k1 + tf~)
 * Exposed so the unit tests can pin a hand-computed value. */
double sh_index_bm25f_term(uint32_t n, uint32_t df, const uint32_t tf[SH_INDEX_FIELD_COUNT],
                           const uint32_t len[SH_INDEX_FIELD_COUNT],
                           const double avg[SH_INDEX_FIELD_COUNT]);

/* Score every document against the (already deduped) query terms and
 * return the best `limit`, highest first, ties by document index.
 * Allocates *out; the caller frees it.  0 hits for an empty query. */
size_t sh_index_search(const sh_index *idx, const char *const *terms, size_t term_count,
                       size_t limit, sh_index_scored **out);

/* ----------------------------------------------------------- vectors */

typedef struct {
    uint32_t count;
    uint32_t dim;
    char *ids;  /* count * 64 bytes */
    float *vec; /* count * dim */
} sh_vectors;

int sh_vectors_load(const char *path, sh_vectors *out);
void sh_vectors_free(sh_vectors *v);

/* Atomic (temp + rename) write of the whole file. */
int sh_vectors_write(const char *path, const sh_vectors *v);

/* Dot product of the query against record `i`. Returns 1 on success. */
int sh_vectors_dot(const sh_vectors *v, size_t i, const float *query, double *out);

/* L2-normalise in place; returns 1 when the vector had a non-zero norm. */
int sh_index_normalize_vector(float *vec, uint32_t dim);

/* -------------------------------------------------- embedder plumbing */

/*
 * Build the "name\ndescription\nprofile" text that both the embed command
 * and the query-time embedder consume.
 */
typedef struct {
    char *name;
    char *desc;
    char *profile;
    char *body;
    char *path;
} sh_index_fields;

int sh_index_fields_build(const char *store_home, const sh_catalog_entry *e, sh_index_fields *out);
void sh_index_fields_free(sh_index_fields *f);

/*
 * Run an external command with stdin from a temp file and stdout to a
 * temp file.  The command is the user's own (SKILLCTL_EMBEDDER /
 * SKILLCTL_QUERY_EMBEDDER); it is interpreted by the platform shell so
 * arguments can be quoted as usual.  Returns 1 when the command exited 0
 * and its output was read.
 */
int sh_index_run_command(const char *work_dir, const char *command, const char *input,
                         size_t input_len, char **out_text, size_t *out_len);

/*
 * Feed `text` to the embedder command and parse its stdout as a JSON
 * array of numbers (or the first array of a line-per-array response).
 * Allocates *out_vec; the caller frees it.  Returns 1 on success.
 */
int sh_index_embed_text(const char *embedder, const char *work_dir, const char *text,
                        float **out_vec, uint32_t *out_dim);

#ifdef __cplusplus
}
#endif

#endif /* SKILLCTL_INDEX_H */
