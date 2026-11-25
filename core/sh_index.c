/*
 * sh_index.c - BM25F index build, load and search (see sh_index.h).
 */
#include "sh_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sh_fs.h"
#include "sh_json.h"
#include "sh_text.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

const double sh_index_field_weight[SH_INDEX_FIELD_COUNT] = {4.0, 2.0, 1.5, 1.0, 0.5};
const double sh_index_field_b[SH_INDEX_FIELD_COUNT] = {0.3, 0.5, 0.6, 0.75, 0.3};

#define DUP_THRESHOLD 0.8
#define CROSS_SAMPLE 20 /* compare every 20th document across name groups */

/* ------------------------------------------------------------ numerics */

/*
 * log() and sqrt() would drag libm into the link line, which the project
 * deliberately does not carry.  Both are implemented here with plain
 * arithmetic; the accuracy is far beyond what a ranking score needs.
 */
static double sh_ln(double x) {
    double e = 0.0, z, z2, term, sum;
    int k;

    if (x <= 0.0) return 0.0;
    while (x > 2.0) {
        x *= 0.5;
        e += 1.0;
    }
    while (x < 1.0) {
        x *= 2.0;
        e -= 1.0;
    }
    /* ln(m) = 2 * atanh((m-1)/(m+1)), m in [1, 2] -> |z| <= 1/3. */
    z = (x - 1.0) / (x + 1.0);
    z2 = z * z;
    term = z;
    sum = 0.0;
    for (k = 1; k < 40; k += 2) {
        sum += term / (double)k;
        term *= z2;
        if (term < 1e-17) break;
    }
    return 2.0 * sum + e * 0.69314718055994530942;
}

static double sh_sqrt(double x) {
    double g;
    if (x <= 0.0) return 0.0;
    g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) {
        double next = 0.5 * (g + x / g);
        if (next == g) break;
        g = next;
    }
    return g;
}

/* ------------------------------------------------------------- buffers */

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} bb;

static int bb_reserve(bb *b, size_t add) {
    size_t need, cap;
    unsigned char *d;

    if (add > (size_t)-1 - b->len) return 0;
    need = b->len + add;
    if (need <= b->cap) return 1;
    cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    d = realloc(b->data, cap);
    if (!d) return 0;
    b->data = d;
    b->cap = cap;
    return 1;
}

static int bb_put(bb *b, const void *p, size_t n) {
    if (!bb_reserve(b, n)) return 0;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

static int bb_varint(bb *b, uint32_t v) {
    unsigned char tmp[5];
    size_t n = 0;
    do {
        unsigned char c = (unsigned char)(v & 0x7F);
        v >>= 7;
        if (v) c |= 0x80;
        tmp[n++] = c;
    } while (v);
    return bb_put(b, tmp, n);
}

static int rv_varint(const unsigned char *data, size_t len, size_t *pos, uint32_t *out) {
    uint32_t v = 0;
    int shift = 0;
    while (*pos < len && shift < 32) {
        unsigned char c = data[(*pos)++];
        v |= (uint32_t)(c & 0x7F) << shift;
        if (!(c & 0x80)) {
            *out = v;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static uint16_t get_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t term_hash(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 32;
    return (uint32_t)h;
}

/* --------------------------------------------------------------- paths */

char *sh_index_dir(const char *store_home) {
    if (!store_home) return NULL;
    return sh_path_join(store_home, "index");
}

static char *file_in(const char *dir, const char *name) {
    return sh_path_join(dir, name);
}

/*
 * Write `data` to <dir>/<name>. For the index the caller wants all files
 * staged before any is swapped in, so the write and the rename are two
 * steps and meta.json is committed last.
 */
static int stage_file(const char *dir, const char *name, const void *data, size_t len, char **tmp) {
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    char *path = malloc(dl + nl + 9);
    FILE *f;
    size_t wrote = 0;

    if (!path) return 0;
    memcpy(path, dir, dl);
    path[dl] = '/';
    path[dl + 1] = '.';
    memcpy(path + dl + 2, name, nl);
    memcpy(path + dl + 2 + nl, ".tmp", 5);

    f = fopen(path, "wb");
    if (!f) {
        free(path);
        return 0;
    }
    if (len) wrote = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || wrote != len) {
        remove(path);
        free(path);
        return 0;
    }
    *tmp = path;
    return 1;
}

static int commit_file(const char *dir, const char *name, const char *tmp) {
    char *final = file_in(dir, name);
    sh_fs_status st;
    if (!final) return 0;
    st = sh_rename(tmp, final);
    free(final);
    return st == SH_FS_OK;
}

/* --------------------------------------------------------------- meta */

typedef struct {
    uint32_t doc_count;
    uint32_t term_count;
    uint32_t catalog_entries;
    int64_t built_at;
    int64_t catalog_mtime;
    double avg[SH_INDEX_FIELD_COUNT];
} index_meta;

static int meta_write(const char *dir, const index_meta *m, char **tmp_out) {
    sh_jw w;
    int ok;

    sh_jw_init(&w);
    sh_jw_obj_begin(&w);
    sh_jw_key(&w, "format_version");
    sh_jw_str(&w, SH_INDEX_FORMAT_VERSION);
    sh_jw_key(&w, "doc_count");
    sh_jw_num(&w, (long long)m->doc_count);
    sh_jw_key(&w, "term_count");
    sh_jw_num(&w, (long long)m->term_count);
    sh_jw_key(&w, "catalog_entries");
    sh_jw_num(&w, (long long)m->catalog_entries);
    sh_jw_key(&w, "built_at");
    sh_jw_num(&w, (long long)m->built_at);
    sh_jw_key(&w, "catalog_mtime");
    sh_jw_num(&w, (long long)m->catalog_mtime);
    sh_jw_key(&w, "avg_len");
    sh_jw_arr_begin(&w);
    for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++) sh_jw_numf(&w, m->avg[f]);
    sh_jw_arr_end(&w);
    sh_jw_obj_end(&w);

    ok = stage_file(dir, "meta.json", sh_jw_text(&w), sh_jw_len(&w), tmp_out);
    sh_jw_free(&w);
    return ok;
}

static int meta_read(const char *dir, index_meta *out) {
    char *path = file_in(dir, "meta.json");
    char *data = NULL;
    size_t len = 0;
    sh_json *j;
    const char *version;
    int ok = 0;

    memset(out, 0, sizeof(*out));
    if (!path) return 0;
    if (sh_read_file(path, &data, &len) != SH_FS_OK) {
        free(path);
        return 0;
    }
    j = sh_json_parse(data, len);
    if (j && sh_json_type_of(j) == SH_JSON_OBJ) {
        version = sh_json_obj_str(j, "format_version");
        if (version && strcmp(version, SH_INDEX_FORMAT_VERSION) == 0) {
            const sh_json *avg = sh_json_obj_get(j, "avg_len");
            out->doc_count = (uint32_t)sh_json_obj_num(j, "doc_count", 0);
            out->term_count = (uint32_t)sh_json_obj_num(j, "term_count", 0);
            out->catalog_entries = (uint32_t)sh_json_obj_num(j, "catalog_entries", 0);
            out->built_at = (int64_t)sh_json_obj_num(j, "built_at", 0);
            out->catalog_mtime = (int64_t)sh_json_obj_num(j, "catalog_mtime", 0);
            for (size_t f = 0; f < SH_INDEX_FIELD_COUNT; f++)
                out->avg[f] = sh_json_as_num(sh_json_arr_get(avg, f));
            ok = 1;
        }
    }
    sh_json_free(j);
    free(data);
    free(path);
    return ok;
}

int sh_index_is_stale(const char *dir, const char *catalog_path, size_t catalog_entries) {
    index_meta meta;
    sh_fs_info info;

    if (!dir || !catalog_path) return 1;
    if (!meta_read(dir, &meta)) return 1;
    if (meta.catalog_entries != catalog_entries) return 1;
    if (sh_stat(catalog_path, &info) != SH_FS_OK) return 1;
    if (info.mtime != meta.catalog_mtime) return 1;
    return 0;
}

/* -------------------------------------------------------------- fields */

static size_t utf8_clip(const char *s, size_t len, size_t cap) {
    size_t i = 0;

    if (len <= cap) return len;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t adv;
        if (c < 0x80) adv = 1;
        else if ((c & 0xE0) == 0xC0) adv = 2;
        else if ((c & 0xF0) == 0xE0) adv = 3;
        else if ((c & 0xF8) == 0xF0) adv = 4;
        else adv = 1;
        if (i + adv > cap) break;
        i += adv;
    }
    return i;
}

/* Offset just past the YAML frontmatter, or 0 when there is none. */
static size_t frontmatter_end(const char *s, size_t len) {
    size_t p = 0;

    if (len < 4) return 0;
    if (!(s[0] == '-' && s[1] == '-' && s[2] == '-')) return 0;
    p = 3;
    if (p < len && s[p] == '\r') p++;
    if (p >= len || s[p] != '\n') return 0;
    p++;
    while (p < len) {
        size_t eol = p;
        size_t line_end;
        while (eol < len && s[eol] != '\n') eol++;
        line_end = eol;
        if (line_end > p && s[line_end - 1] == '\r') line_end--;
        if (line_end - p == 3 && s[p] == '-' && s[p + 1] == '-' && s[p + 2] == '-')
            return eol < len ? eol + 1 : len;
        p = eol + 1;
    }
    return 0;
}

static char *read_body(const char *store_home, const sh_catalog_entry *e, size_t *out_len) {
    char *data = NULL;
    size_t len = 0;

    *out_len = 0;
    if (!e) return NULL;

    if (e->in_store && store_home && e->id) {
        char *units = sh_path_join(store_home, "units");
        char *dir = units ? sh_path_join(units, e->id) : NULL;
        char *path = dir ? sh_path_join(dir, "SKILL.md") : NULL;
        free(units);
        free(dir);
        if (path) {
            if (sh_read_file(path, &data, &len) == SH_FS_OK) {
                free(path);
                *out_len = len;
                return data;
            }
            free(path);
        }
    }
    for (size_t i = 0; i < e->source_count; i++) {
        char *path;
        if (!e->sources[i] || !e->sources[i][0]) continue;
        path = sh_path_join(e->sources[i], "SKILL.md");
        if (!path) continue;
        if (sh_read_file(path, &data, &len) == SH_FS_OK) {
            free(path);
            *out_len = len;
            return data;
        }
        free(path);
    }
    return NULL;
}

int sh_index_fields_build(const char *store_home, const sh_catalog_entry *e, sh_index_fields *out) {
    char *raw = NULL;
    size_t raw_len = 0;
    size_t start, body_len;
    const char *first = "";

    memset(out, 0, sizeof(*out));
    if (!e) return 0;

    out->name = strdup(e->name ? e->name : "");
    out->desc = strdup(e->description ? e->description : "");
    if (e->source_count > 0 && e->sources[0]) first = e->sources[0];
    out->path = strdup(first);
    if (!out->name || !out->desc || !out->path) goto fail;

    raw = read_body(store_home, e, &raw_len);
    if (raw && raw_len > 0) {
        start = frontmatter_end(raw, raw_len);
        body_len = utf8_clip(raw + start, raw_len - start, SH_INDEX_BODY_CAP);
    } else {
        body_len = 0;
    }
    out->body = malloc(body_len + 1);
    if (!out->body) goto fail;
    if (body_len) memcpy(out->body, raw + start, body_len);
    out->body[body_len] = '\0';
    free(raw);
    raw = NULL;

    out->profile = sh_text_profile(out->body, body_len, NULL);
    if (!out->profile) {
        out->profile = strdup("");
        if (!out->profile) goto fail;
    }
    return 1;

fail:
    free(raw);
    sh_index_fields_free(out);
    return 0;
}

void sh_index_fields_free(sh_index_fields *f) {
    if (!f) return;
    free(f->name);
    free(f->desc);
    free(f->profile);
    free(f->body);
    free(f->path);
    memset(f, 0, sizeof(*f));
}

/* ------------------------------------------------------------ build */

static const char *sh_index_field_text(const sh_index_fields *f, int field) {
    switch (field) {
    case SH_INDEX_FIELD_NAME: return f->name;
    case SH_INDEX_FIELD_DESC: return f->desc;
    case SH_INDEX_FIELD_PROFILE: return f->profile;
    case SH_INDEX_FIELD_BODY: return f->body;
    default: return f->path;
    }
}

typedef struct {
    char **terms;
    uint32_t *dfs;
    size_t count;
    size_t cap;
    uint32_t *slots; /* term index + 1, 0 = empty */
    size_t slot_cap; /* power of two */
} term_table;

static int tt_rehash(term_table *tt, size_t new_cap) {
    uint32_t *slots = calloc(new_cap, sizeof(uint32_t));
    if (!slots) return 0;
    for (size_t i = 0; i < tt->count; i++) {
        uint32_t h = term_hash(tt->terms[i], strlen(tt->terms[i]));
        size_t s = (size_t)h & (new_cap - 1);
        while (slots[s]) s = (s + 1) & (new_cap - 1);
        slots[s] = (uint32_t)(i + 1);
    }
    free(tt->slots);
    tt->slots = slots;
    tt->slot_cap = new_cap;
    return 1;
}

static int tt_intern(term_table *tt, const char *term, size_t n, uint32_t *out) {
    size_t s;
    size_t h;

    if (tt->slot_cap == 0 && !tt_rehash(tt, 1024)) return 0;
    if ((tt->count + 1) * 10 >= tt->slot_cap * 7) {
        if (!tt_rehash(tt, tt->slot_cap * 2)) return 0;
    }
    h = (size_t)term_hash(term, n);
    s = h & (tt->slot_cap - 1);
    while (tt->slots[s]) {
        uint32_t idx = tt->slots[s] - 1;
        if (strlen(tt->terms[idx]) == n && memcmp(tt->terms[idx], term, n) == 0) {
            *out = idx;
            return 1;
        }
        s = (s + 1) & (tt->slot_cap - 1);
    }
    if (tt->count == tt->cap) {
        size_t cap = tt->cap ? tt->cap * 2 : 256;
        char **terms = realloc(tt->terms, cap * sizeof(char *));
        uint32_t *dfs;
        if (!terms) return 0;
        tt->terms = terms;
        dfs = realloc(tt->dfs, cap * sizeof(uint32_t));
        if (!dfs) return 0;
        tt->dfs = dfs;
        tt->cap = cap;
    }
    tt->terms[tt->count] = malloc(n + 1);
    if (!tt->terms[tt->count]) return 0;
    memcpy(tt->terms[tt->count], term, n);
    tt->terms[tt->count][n] = '\0';
    tt->dfs[tt->count] = 0;
    tt->slots[s] = (uint32_t)(tt->count + 1);
    *out = (uint32_t)tt->count;
    tt->count++;
    return 1;
}

static void tt_free(term_table *tt) {
    for (size_t i = 0; i < tt->count; i++) free(tt->terms[i]);
    free(tt->terms);
    free(tt->dfs);
    free(tt->slots);
    memset(tt, 0, sizeof(*tt));
}

/* Per-document term -> tf[5] map, keyed by global term index. */
typedef struct {
    uint32_t *keys;
    uint32_t *tf;
    uint32_t *used;
    size_t cap;
    size_t mask;
    size_t used_count;
    size_t used_cap;
} doc_map;

static size_t dm_slot(uint32_t key, size_t mask) {
    uint32_t h = key;
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return (size_t)h & mask;
}

static int dm_grow(doc_map *m, size_t cap) {
    uint32_t *keys = calloc(cap, sizeof(uint32_t));
    uint32_t *tf = calloc(cap * SH_INDEX_FIELD_COUNT, sizeof(uint32_t));
    if (!keys || !tf) {
        free(keys);
        free(tf);
        return 0;
    }
    for (size_t k = 0; k < m->used_count; k++) {
        size_t old = m->used[k];
        uint32_t key = m->keys[old];
        size_t s = dm_slot(key - 1, cap - 1);
        while (keys[s]) s = (s + 1) & (cap - 1);
        keys[s] = key;
        for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++)
            tf[s * SH_INDEX_FIELD_COUNT + f] = m->tf[old * SH_INDEX_FIELD_COUNT + f];
        m->used[k] = (uint32_t)s;
    }
    free(m->keys);
    free(m->tf);
    m->keys = keys;
    m->tf = tf;
    m->cap = cap;
    m->mask = cap - 1;
    return 1;
}

static int dm_add(doc_map *m, uint32_t key, int field) {
    size_t s;

    if (m->cap == 0 && !dm_grow(m, 1024)) return 0;
    if (m->used_count * 2 >= m->cap && !dm_grow(m, m->cap * 2)) return 0;
    s = dm_slot(key, m->mask);
    while (m->keys[s]) {
        if (m->keys[s] == key + 1) {
            m->tf[s * SH_INDEX_FIELD_COUNT + field]++;
            return 1;
        }
        s = (s + 1) & m->mask;
    }
    if (m->used_count == m->used_cap) {
        size_t cap = m->used_cap ? m->used_cap * 2 : 256;
        uint32_t *used = realloc(m->used, cap * sizeof(uint32_t));
        if (!used) return 0;
        m->used = used;
        m->used_cap = cap;
    }
    m->keys[s] = key + 1;
    m->tf[s * SH_INDEX_FIELD_COUNT + field] = 1;
    m->used[m->used_count++] = (uint32_t)s;
    return 1;
}

static void dm_reset(doc_map *m) {
    for (size_t k = 0; k < m->used_count; k++) m->keys[m->used[k]] = 0;
    m->used_count = 0;
}

static void dm_free(doc_map *m) {
    free(m->keys);
    free(m->tf);
    free(m->used);
    memset(m, 0, sizeof(*m));
}

typedef struct {
    uint32_t term;
    uint32_t doc;
    uint32_t tf[SH_INDEX_FIELD_COUNT];
} posting_entry;

typedef struct {
    posting_entry *items;
    size_t count;
    size_t cap;
} posting_vec;

static int pv_push(posting_vec *p, const posting_entry *e) {
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 4096;
        posting_entry *items = realloc(p->items, cap * sizeof(posting_entry));
        if (!items) return 0;
        p->items = items;
        p->cap = cap;
    }
    p->items[p->count++] = *e;
    return 1;
}

static int cmp_posting(const void *a, const void *b) {
    const posting_entry *x = a;
    const posting_entry *y = b;
    if (x->term != y->term) return x->term < y->term ? -1 : 1;
    if (x->doc != y->doc) return x->doc < y->doc ? -1 : 1;
    return 0;
}

typedef struct {
    uint32_t idx;
    const char *term;
} term_order;

static int cmp_term_order(const void *a, const void *b) {
    const term_order *x = a;
    const term_order *y = b;
    return strcmp(x->term, y->term);
}

/* Union-find over catalog entries; the better entry becomes the root. */
static size_t uf_find(size_t *parent, size_t i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

static int entry_rank(const sh_catalog *cat, size_t i) {
    const sh_catalog_entry *e = &cat->entries[i];
    int rank = e->in_store ? 1000 : 0;
    rank += e->source_count > 50 ? 500 : (int)e->source_count * 10;
    return rank;
}

static void uf_union(size_t *parent, const sh_catalog *cat, size_t a, size_t b) {
    size_t ra = uf_find(parent, a);
    size_t rb = uf_find(parent, b);
    if (ra == rb) return;
    if (entry_rank(cat, ra) > entry_rank(cat, rb) || (entry_rank(cat, ra) == entry_rank(cat, rb) && ra < rb))
        parent[rb] = ra;
    else
        parent[ra] = rb;
}

typedef struct {
    size_t idx;
    const char *norm;
} name_key;

static int cmp_name_key(const void *a, const void *b) {
    const name_key *x = a;
    const name_key *y = b;
    return strcmp(x->norm, y->norm);
}

int sh_index_build(const char *dir, const char *store_home, const char *catalog_path,
                   const sh_catalog *cat, sh_index_build_stats *stats) {
    sh_index_fields *fields = NULL;
    char **norms = NULL;
    sh_text_shingles *shingles = NULL;
    size_t *parent = NULL;
    size_t *counts = NULL;
    name_key *keys = NULL;
    sh_index_doc *docs = NULL;
    size_t *rep_idx = NULL;
    size_t doc_count = 0, merged = 0;
    term_table tt;
    posting_vec pv;
    doc_map dm;
    bb postings;
    bb terms_out;
    bb docs_out;
    char *tmp_terms = NULL, *tmp_postings = NULL, *tmp_docs = NULL, *tmp_meta = NULL;
    index_meta meta;
    sh_fs_info info;
    clock_t t0 = clock();
    int rc = 0;

    memset(&tt, 0, sizeof(tt));
    memset(&pv, 0, sizeof(pv));
    memset(&dm, 0, sizeof(dm));
    memset(&postings, 0, sizeof(postings));
    memset(&terms_out, 0, sizeof(terms_out));
    memset(&docs_out, 0, sizeof(docs_out));
    memset(&meta, 0, sizeof(meta));

    if (!dir || !cat || cat->count == 0) return 0;
    if (sh_mkdir_p(dir) != SH_FS_OK) return 0;

    /* ---- read the five fields of every catalog entry ---- */
    fields = calloc(cat->count, sizeof(sh_index_fields));
    norms = calloc(cat->count, sizeof(char *));
    shingles = calloc(cat->count, sizeof(sh_text_shingles));
    parent = calloc(cat->count, sizeof(size_t));
    counts = calloc(cat->count, sizeof(size_t));
    keys = calloc(cat->count, sizeof(name_key));
    if (!fields || !norms || !shingles || !parent || !counts || !keys) goto done;

    for (size_t i = 0; i < cat->count; i++) {
        if (!sh_index_fields_build(store_home, &cat->entries[i], &fields[i])) goto done;
        norms[i] = sh_text_normalize_name(cat->entries[i].name);
        if (!norms[i]) goto done;
        {
            const char *text = fields[i].body[0] ? fields[i].body : fields[i].desc;
            if (!sh_text_shingles_build(text, strlen(text), &shingles[i])) goto done;
        }
        parent[i] = i;
        keys[i].idx = i;
        keys[i].norm = norms[i];
    }

    /* ---- duplicate grouping ---- */
    qsort(keys, cat->count, sizeof(name_key), cmp_name_key);
    for (size_t g = 0; g < cat->count;) {
        size_t end = g + 1;
        while (end < cat->count && strcmp(keys[end].norm, keys[g].norm) == 0) end++;
        for (size_t a = g; a < end; a++) {
            for (size_t b = a + 1; b < end; b++) {
                if (uf_find(parent, keys[a].idx) == uf_find(parent, keys[b].idx)) continue;
                if (sh_text_shingles_near_duplicate(&shingles[keys[a].idx], &shingles[keys[b].idx],
                                                    DUP_THRESHOLD))
                    uf_union(parent, cat, keys[a].idx, keys[b].idx);
            }
        }
        g = end;
    }

    /* Cross-group near duplicates: 5% of the documents are probed against
     * every other document.  The full quadratic pass is not worth its
     * cost at this corpus size; sampling is the deliberate compromise. */
    for (size_t i = 0; i < cat->count; i += CROSS_SAMPLE) {
        for (size_t j = 0; j < cat->count; j++) {
            if (i == j) continue;
            if (strcmp(norms[i], norms[j]) == 0) continue; /* already grouped */
            if (uf_find(parent, i) == uf_find(parent, j)) continue;
            if (sh_text_shingles_near_duplicate(&shingles[i], &shingles[j], DUP_THRESHOLD))
                uf_union(parent, cat, i, j);
        }
    }

    for (size_t i = 0; i < cat->count; i++) counts[uf_find(parent, i)]++;
    for (size_t i = 0; i < cat->count; i++)
        if (parent[i] == i) doc_count++;
    if (doc_count == 0) goto done;

    docs = calloc(doc_count, sizeof(sh_index_doc));
    rep_idx = calloc(doc_count, sizeof(size_t));
    if (!docs || !rep_idx) goto done;
    {
        size_t d = 0;
        for (size_t i = 0; i < cat->count; i++) {
            if (parent[i] != i) continue;
            docs[d].id = strdup(cat->entries[i].id ? cat->entries[i].id : "");
            docs[d].name = strdup(fields[i].name);
            docs[d].variants = counts[i];
            if (!docs[d].id || !docs[d].name) goto done;
            rep_idx[d] = i;
            merged += counts[i] - 1;
            d++;
        }
        doc_count = d;
    }

    /* ---- tokenise every field of every document ---- */
    for (size_t d = 0; d < doc_count; d++) {
        dm_reset(&dm);
        for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++) {
            sh_text_tokens tokens;
            const char *text = sh_index_field_text(&fields[rep_idx[d]], f);
            memset(&tokens, 0, sizeof(tokens));
            if (!sh_text_tokenize(text, strlen(text), &tokens)) {
                sh_text_tokens_free(&tokens);
                goto done;
            }
            docs[d].len[f] = (uint32_t)tokens.count;
            for (size_t t = 0; t < tokens.count; t++) {
                uint32_t ti = 0;
                if (!tt_intern(&tt, tokens.items[t], strlen(tokens.items[t]), &ti)) {
                    sh_text_tokens_free(&tokens);
                    goto done;
                }
                if (!dm_add(&dm, ti, f)) {
                    sh_text_tokens_free(&tokens);
                    goto done;
                }
            }
            sh_text_tokens_free(&tokens);
        }
        for (size_t k = 0; k < dm.used_count; k++) {
            size_t s = dm.used[k];
            posting_entry pe;
            pe.term = dm.keys[s] - 1;
            pe.doc = (uint32_t)d;
            for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++)
                pe.tf[f] = dm.tf[s * SH_INDEX_FIELD_COUNT + f];
            if (!pv_push(&pv, &pe)) goto done;
        }
    }
    if (pv.count > 0) {
        qsort(pv.items, pv.count, sizeof(posting_entry), cmp_posting);
        for (size_t i = 1; i < pv.count; i++) {
            if (pv.items[i].term == pv.items[i - 1].term &&
                pv.items[i].doc == pv.items[i - 1].doc)
                goto done; /* impossible; never emit duplicates */
        }
    }

    /* ---- average field lengths ---- */
    for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++) {
        double sum = 0.0;
        for (size_t d = 0; d < doc_count; d++) sum += (double)docs[d].len[f];
        meta.avg[f] = doc_count ? sum / (double)doc_count : 0.0;
    }

    /* ---- serialise terms (strcmp order) and postings ---- */
    {
        size_t *group_start = calloc(tt.count ? tt.count : 1, sizeof(size_t));
        uint32_t *group_len = calloc(tt.count ? tt.count : 1, sizeof(uint32_t));
        term_order *order = calloc(tt.count ? tt.count : 1, sizeof(term_order));
        if (!group_start || !group_len || !order) {
            free(group_start);
            free(group_len);
            free(order);
            goto done;
        }
        {
            size_t p = 0;
            for (size_t t = 0; t < tt.count; t++) {
                group_start[t] = p;
                while (p < pv.count && pv.items[p].term == t) {
                    group_len[t]++;
                    p++;
                }
            }
        }
        for (size_t t = 0; t < tt.count; t++) {
            order[t].idx = (uint32_t)t;
            order[t].term = tt.terms[t];
        }
        qsort(order, tt.count, sizeof(term_order), cmp_term_order);

        for (size_t oi = 0; oi < tt.count; oi++) {
            uint32_t t = order[oi].idx;
            uint32_t off = (uint32_t)postings.len;
            for (size_t k = 0; k < group_len[t]; k++) {
                const posting_entry *pe = &pv.items[group_start[t] + k];
                if (!bb_varint(&postings, pe->doc)) {
                    free(group_start);
                    free(group_len);
                    free(order);
                    goto done;
                }
                for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++) {
                    if (!bb_varint(&postings, pe->tf[f])) {
                        free(group_start);
                        free(group_len);
                        free(order);
                        goto done;
                    }
                }
            }
            {
                size_t tl = strlen(tt.terms[t]);
                unsigned char hdr[2];
                unsigned char tail[12];
                if (tl > 0xFFFF) tl = 0xFFFF;
                put_u16(hdr, (uint16_t)tl);
                put_u32(tail, group_len[t]);
                put_u32(tail + 4, off);
                put_u32(tail + 8, (uint32_t)(postings.len - off));
                if (!bb_put(&terms_out, hdr, 2) || !bb_put(&terms_out, tt.terms[t], tl) ||
                    !bb_put(&terms_out, tail, 12)) {
                    free(group_start);
                    free(group_len);
                    free(order);
                    goto done;
                }
            }
        }
        free(group_start);
        free(group_len);
        free(order);
    }

    /* ---- docs.txt ---- */
    for (size_t d = 0; d < doc_count; d++) {
        char line[512];
        int n;
        n = snprintf(line, sizeof(line), "%zu\t%s\t%s\t%zu\t%u\t%u\t%u\t%u\t%u\n", d,
                     docs[d].id, docs[d].name, docs[d].variants, docs[d].len[0], docs[d].len[1],
                     docs[d].len[2], docs[d].len[3], docs[d].len[4]);
        if (n <= 0 || (size_t)n >= sizeof(line)) goto done;
        if (!bb_put(&docs_out, line, (size_t)n)) goto done;
    }

    /* ---- stamp and commit ---- */
    meta.doc_count = (uint32_t)doc_count;
    meta.term_count = (uint32_t)tt.count;
    meta.catalog_entries = (uint32_t)cat->count;
    meta.built_at = sh_now();
    memset(&info, 0, sizeof(info));
    meta.catalog_mtime = sh_stat(catalog_path, &info) == SH_FS_OK ? info.mtime : 0;

    if (!stage_file(dir, "terms.bin", terms_out.data, terms_out.len, &tmp_terms) ||
        !stage_file(dir, "postings.bin", postings.data, postings.len, &tmp_postings) ||
        !stage_file(dir, "docs.txt", docs_out.data, docs_out.len, &tmp_docs) ||
        !meta_write(dir, &meta, &tmp_meta))
        goto done;

    /* meta last: readers treat a matching meta as "the index is complete". */
    if (!commit_file(dir, "terms.bin", tmp_terms)) goto done;
    free(tmp_terms);
    tmp_terms = NULL;
    if (!commit_file(dir, "postings.bin", tmp_postings)) goto done;
    free(tmp_postings);
    tmp_postings = NULL;
    if (!commit_file(dir, "docs.txt", tmp_docs)) goto done;
    free(tmp_docs);
    tmp_docs = NULL;
    if (!commit_file(dir, "meta.json", tmp_meta)) goto done;
    free(tmp_meta);
    tmp_meta = NULL;

    if (stats) {
        stats->docs = doc_count;
        stats->terms = tt.count;
        stats->postings = pv.count;
        stats->merged = merged;
        stats->seconds = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    }
    rc = 1;

done:
    if (!rc) {
        if (tmp_terms) remove(tmp_terms);
        if (tmp_postings) remove(tmp_postings);
        if (tmp_docs) remove(tmp_docs);
        if (tmp_meta) remove(tmp_meta);
    }
    free(tmp_terms);
    free(tmp_postings);
    free(tmp_docs);
    free(tmp_meta);
    if (docs) {
        for (size_t d = 0; d < doc_count; d++) {
            free(docs[d].id);
            free(docs[d].name);
        }
    }
    free(docs);
    free(rep_idx);
    free(keys);
    free(counts);
    free(parent);
    if (shingles) {
        for (size_t i = 0; i < cat->count; i++) sh_text_shingles_free(&shingles[i]);
    }
    free(shingles);
    if (norms) {
        for (size_t i = 0; i < cat->count; i++) free(norms[i]);
    }
    free(norms);
    if (fields) {
        for (size_t i = 0; i < cat->count; i++) sh_index_fields_free(&fields[i]);
    }
    free(fields);
    tt_free(&tt);
    dm_free(&dm);
    free(pv.items);
    free(postings.data);
    free(terms_out.data);
    free(docs_out.data);
    return rc;
}

/* --------------------------------------------------------------- load */

static int split_tab(char *line, char **out, size_t max, size_t *count) {
    size_t n = 0;
    char *p = line;

    while (n < max) {
        out[n++] = p;
        p = strchr(p, '\t');
        if (!p) break;
        *p = '\0';
        p++;
    }
    *count = n;
    return 1;
}

int sh_index_load(const char *dir, sh_index *out) {
    index_meta meta;
    char *postings_path = NULL, *terms_path = NULL, *docs_path = NULL;
    char *pdata = NULL, *tdata = NULL, *ddata = NULL;
    size_t plen = 0, tlen = 0, dlen = 0;
    int rc = 0;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!dir) return 0;
    if (!meta_read(dir, &meta)) return 0;
    if (meta.doc_count == 0) return 0;

    out->dir = strdup(dir);
    out->doc_count = meta.doc_count;
    out->term_count = meta.term_count;
    out->catalog_entries = meta.catalog_entries;
    out->built_at = meta.built_at;
    out->catalog_mtime = meta.catalog_mtime;
    for (size_t f = 0; f < SH_INDEX_FIELD_COUNT; f++) out->avg_len[f] = meta.avg[f];
    if (!out->dir) goto done;

    postings_path = file_in(dir, "postings.bin");
    terms_path = file_in(dir, "terms.bin");
    docs_path = file_in(dir, "docs.txt");
    if (!postings_path || !terms_path || !docs_path) goto done;
    if (sh_read_file(postings_path, &pdata, &plen) != SH_FS_OK) goto done;
    out->postings = (unsigned char *)pdata;
    pdata = NULL;
    out->postings_size = plen;

    out->terms = calloc(meta.term_count ? meta.term_count : 1, sizeof(sh_index_term));
    if (!out->terms) goto done;
    if (sh_read_file(terms_path, &tdata, &tlen) != SH_FS_OK) goto done;
    {
        size_t pos = 0;
        for (uint32_t k = 0; k < meta.term_count; k++) {
            uint16_t tl;
            sh_index_term *term = &out->terms[k];
            if (pos + 2 > tlen) goto done;
            tl = get_u16((const unsigned char *)tdata + pos);
            pos += 2;
            if (tl == 0 || pos + (size_t)tl + 12 > tlen) goto done;
            term->term = malloc((size_t)tl + 1);
            if (!term->term) goto done;
            memcpy(term->term, tdata + pos, tl);
            term->term[tl] = '\0';
            pos += tl;
            term->df = get_u32((const unsigned char *)tdata + pos);
            term->postings_off = get_u32((const unsigned char *)tdata + pos + 4);
            term->postings_len = get_u32((const unsigned char *)tdata + pos + 8);
            pos += 12;
            if ((size_t)term->postings_off + (size_t)term->postings_len > out->postings_size)
                goto done;
            if (k > 0 && strcmp(out->terms[k - 1].term, term->term) >= 0) goto done;
        }
    }
    free(tdata);
    tdata = NULL;

    out->docs = calloc(meta.doc_count, sizeof(sh_index_doc));
    if (!out->docs) goto done;
    if (sh_read_file(docs_path, &ddata, &dlen) != SH_FS_OK) goto done;
    {
        size_t pos = 0;
        size_t d = 0;
        while (pos < dlen && d < meta.doc_count) {
            size_t eol = pos;
            char *fields[10];
            size_t nf = 0;
            while (eol < dlen && ddata[eol] != '\n') eol++;
            if (eol > pos) {
                ddata[eol] = '\0';
                split_tab(ddata + pos, fields, 10, &nf);
                if (nf < 3) goto done;
                out->docs[d].id = strdup(fields[1]);
                out->docs[d].name = strdup(fields[2]);
                if (!out->docs[d].id || !out->docs[d].name) goto done;
                out->docs[d].variants = nf >= 4 ? (size_t)strtoull(fields[3], NULL, 10) : 1;
                if (out->docs[d].variants == 0) out->docs[d].variants = 1;
                for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++)
                    out->docs[d].len[f] = nf >= 5 + (size_t)f
                                              ? (uint32_t)strtoul(fields[4 + (size_t)f], NULL, 10)
                                              : 0;
                d++;
            }
            pos = eol + 1;
        }
        if (d != meta.doc_count) goto done;
    }
    out->loaded = 1;
    rc = 1;

done:
    free(postings_path);
    free(terms_path);
    free(docs_path);
    free(pdata);
    free(tdata);
    free(ddata);
    if (!rc) sh_index_free(out);
    return rc;
}

void sh_index_free(sh_index *idx) {
    if (!idx) return;
    if (idx->terms) {
        for (uint32_t i = 0; i < idx->term_count; i++) free(idx->terms[i].term);
    }
    free(idx->terms);
    if (idx->docs) {
        for (uint32_t i = 0; i < idx->doc_count; i++) {
            free(idx->docs[i].id);
            free(idx->docs[i].name);
        }
    }
    free(idx->docs);
    free(idx->postings);
    free(idx->dir);
    memset(idx, 0, sizeof(*idx));
}

/* ------------------------------------------------------------- scoring */

double sh_index_idf(uint32_t n, uint32_t df) {
    if (n == 0) return 0.0;
    return sh_ln(1.0 + ((double)n - (double)df + 0.5) / ((double)df + 0.5));
}

double sh_index_bm25f_term(uint32_t n, uint32_t df, const uint32_t tf[SH_INDEX_FIELD_COUNT],
                           const uint32_t len[SH_INDEX_FIELD_COUNT],
                           const double avg[SH_INDEX_FIELD_COUNT]) {
    double tfn = 0.0;

    for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++) {
        double norm;
        if (tf[f] == 0) continue;
        if (avg[f] > 0.0)
            norm = 1.0 - sh_index_field_b[f] + sh_index_field_b[f] * (double)len[f] / avg[f];
        else
            norm = 1.0;
        tfn += sh_index_field_weight[f] * (double)tf[f] / norm;
    }
    if (tfn <= 0.0) return 0.0;
    return sh_index_idf(n, df) * tfn / (SH_INDEX_K1 + tfn);
}

static int cmp_term_key(const void *key, const void *elem) {
    const char *k = key;
    const sh_index_term *t = elem;
    return strcmp(k, t->term);
}

static int cmp_scored(const void *a, const void *b) {
    const sh_index_scored *x = a;
    const sh_index_scored *y = b;
    if (x->score > y->score) return -1;
    if (x->score < y->score) return 1;
    if (x->doc < y->doc) return -1;
    if (x->doc > y->doc) return 1;
    return 0;
}

size_t sh_index_search(const sh_index *idx, const char *const *terms, size_t term_count,
                       size_t limit, sh_index_scored **out) {
    double *scores = NULL;
    sh_index_scored *all = NULL;
    sh_index_scored *result = NULL;
    size_t n = 0;
    size_t got = 0;

    if (out) *out = NULL;
    if (!idx || !idx->loaded || !terms || term_count == 0 || limit == 0) return 0;

    scores = calloc(idx->doc_count, sizeof(double));
    if (!scores) return 0;

    for (size_t q = 0; q < term_count; q++) {
        const sh_index_term *term;
        size_t pos, end;
        if (!terms[q] || !terms[q][0]) continue;
        term = bsearch(terms[q], idx->terms, idx->term_count, sizeof(sh_index_term), cmp_term_key);
        if (!term) continue;
        pos = term->postings_off;
        end = pos + term->postings_len;
        if (end > idx->postings_size) continue;
        while (pos < end) {
            uint32_t doc = 0;
            uint32_t tf[SH_INDEX_FIELD_COUNT] = {0, 0, 0, 0, 0};
            int bad = 0;
            if (!rv_varint(idx->postings, end, &pos, &doc)) break;
            for (int f = 0; f < SH_INDEX_FIELD_COUNT; f++) {
                if (!rv_varint(idx->postings, end, &pos, &tf[f])) {
                    bad = 1;
                    break;
                }
            }
            if (bad || doc >= idx->doc_count) break;
            scores[doc] += sh_index_bm25f_term(idx->doc_count, term->df, tf, idx->docs[doc].len,
                                               idx->avg_len);
        }
    }

    all = malloc(idx->doc_count * sizeof(sh_index_scored));
    if (!all) {
        free(scores);
        return 0;
    }
    for (size_t d = 0; d < idx->doc_count; d++) {
        if (scores[d] <= 0.0) continue;
        all[got].doc = d;
        all[got].score = scores[d];
        got++;
    }
    free(scores);

    if (got > 0) qsort(all, got, sizeof(sh_index_scored), cmp_scored);
    n = got < limit ? got : limit;
    if (n > 0) {
        result = malloc(n * sizeof(sh_index_scored));
        if (!result) {
            free(all);
            return 0;
        }
        memcpy(result, all, n * sizeof(sh_index_scored));
    }
    free(all);
    *out = result;
    return n;
}

/* ------------------------------------------------------------- vectors */

#define SHVEC_MAGIC "SHVEC1"
#define SHVEC_HEADER 14 /* 6 magic + u32 count + u32 dim */

int sh_vectors_load(const char *path, sh_vectors *out) {
    char *data = NULL;
    size_t len = 0;
    uint32_t count, dim;
    size_t need;
    int rc = 0;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!path) return 0;
    if (sh_read_file(path, &data, &len) != SH_FS_OK) return 0;
    if (len < SHVEC_HEADER || memcmp(data, SHVEC_MAGIC, 6) != 0) goto done;
    count = get_u32((const unsigned char *)data + 6);
    dim = get_u32((const unsigned char *)data + 10);
    if (count == 0 || dim == 0 || dim > 8192 || count > 1000000) goto done;
    need = SHVEC_HEADER + (size_t)count * (64 + (size_t)dim * 4);
    if (len < need) goto done;

    out->ids = malloc((size_t)count * 64);
    out->vec = malloc((size_t)count * (size_t)dim * sizeof(float));
    if (!out->ids || !out->vec) goto done;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *rec =
            (const unsigned char *)data + SHVEC_HEADER + i * (64 + (size_t)dim * 4);
        memcpy(out->ids + i * 64, rec, 64);
        for (uint32_t d = 0; d < dim; d++) {
            uint32_t bits = get_u32(rec + 64 + (size_t)d * 4);
            float value;
            memcpy(&value, &bits, sizeof(value));
            out->vec[i * dim + d] = value;
        }
    }
    out->count = count;
    out->dim = dim;
    rc = 1;

done:
    free(data);
    if (!rc) sh_vectors_free(out);
    return rc;
}

void sh_vectors_free(sh_vectors *v) {
    if (!v) return;
    free(v->ids);
    free(v->vec);
    memset(v, 0, sizeof(*v));
}

int sh_vectors_write(const char *path, const sh_vectors *v) {
    FILE *f;
    char *tmp;
    size_t n;
    int rc = 0;

    if (!path || !v || v->count == 0 || v->dim == 0) return 0;
    n = strlen(path);
    tmp = malloc(n + 5);
    if (!tmp) return 0;
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".tmp", 5);

    f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return 0;
    }
    {
        unsigned char hdr[SHVEC_HEADER];
        memcpy(hdr, SHVEC_MAGIC, 6);
        put_u32(hdr + 6, v->count);
        put_u32(hdr + 10, v->dim);
        if (fwrite(hdr, 1, SHVEC_HEADER, f) != SHVEC_HEADER) goto close_fail;
    }
    for (uint32_t i = 0; i < v->count; i++) {
        if (fwrite(v->ids + (size_t)i * 64, 1, 64, f) != 64) goto close_fail;
        for (uint32_t d = 0; d < v->dim; d++) {
            uint32_t bits;
            memcpy(&bits, &v->vec[(size_t)i * v->dim + d], sizeof(bits));
            {
                unsigned char buf[4];
                put_u32(buf, bits);
                if (fwrite(buf, 1, 4, f) != 4) goto close_fail;
            }
        }
    }
    if (fclose(f) != 0) {
        f = NULL;
        goto close_fail;
    }
    f = NULL;
    if (sh_rename(tmp, path) != SH_FS_OK) goto close_fail;
    rc = 1;
    goto out;

close_fail:
    if (f) fclose(f);
    remove(tmp);
out:
    free(tmp);
    return rc;
}

int sh_vectors_dot(const sh_vectors *v, size_t i, const float *query, double *out) {
    double sum = 0.0;
    const float *row;

    if (!v || !query || !out || i >= v->count) return 0;
    row = v->vec + i * v->dim;
    for (uint32_t d = 0; d < v->dim; d++) sum += (double)row[d] * (double)query[d];
    *out = sum;
    return 1;
}

int sh_index_normalize_vector(float *vec, uint32_t dim) {
    double norm = 0.0;

    if (!vec || dim == 0) return 0;
    for (uint32_t d = 0; d < dim; d++) norm += (double)vec[d] * (double)vec[d];
    if (norm <= 0.0) return 0;
    norm = sh_sqrt(norm);
    for (uint32_t d = 0; d < dim; d++) vec[d] = (float)((double)vec[d] / norm);
    return 1;
}

/* -------------------------------------------------- embedder plumbing */

static unsigned long g_run_seq = 0;

int sh_index_run_command(const char *work_dir, const char *command, const char *input,
                         size_t input_len, char **out_text, size_t *out_len) {
    char *in_path = NULL, *out_path = NULL, *cmdline = NULL;
    size_t wl, seq;
    int rc = 0;

    if (out_text) *out_text = NULL;
    if (out_len) *out_len = 0;
    if (!work_dir || !command || !command[0]) return 0;
    if (sh_mkdir_p(work_dir) != SH_FS_OK) return 0;

    seq = ++g_run_seq;
    wl = strlen(work_dir);
    in_path = malloc(wl + 48);
    out_path = malloc(wl + 48);
    if (!in_path || !out_path) goto done;
    snprintf(in_path, wl + 48, "%s/.shrun_%d_%lu.in", work_dir, sh_process_id(), seq);
    snprintf(out_path, wl + 48, "%s/.shrun_%d_%lu.out", work_dir, sh_process_id(), seq);

    if (sh_write_file(in_path, input ? input : "", input_len) != SH_FS_OK) goto done;

    cmdline = malloc(strlen(command) + strlen(in_path) + strlen(out_path) + 16);
    if (!cmdline) goto done;
    snprintf(cmdline, strlen(command) + strlen(in_path) + strlen(out_path) + 16,
             "%s < \"%s\" > \"%s\"", command, in_path, out_path);

    {
        int status = system(cmdline);
#ifdef _WIN32
        if (status != 0) goto done;
#else
        if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) goto done;
#endif
    }

    if (sh_read_file(out_path, out_text, out_len) != SH_FS_OK) goto done;
    rc = 1;

done:
    if (in_path) remove(in_path);
    if (out_path) remove(out_path);
    free(in_path);
    free(out_path);
    free(cmdline);
    return rc;
}

/* Parse an array of numbers out of one JSON document; returns 1 on
 * success.  A line-per-array response is handled by trying the whole
 * output first and then each line. */
static int json_to_vector(const sh_json *j, float **out_vec, uint32_t *out_dim) {
    const sh_json *arr = j;
    size_t n;
    float *vec;

    if (!j) return 0;
    if (sh_json_type_of(j) == SH_JSON_ARR && sh_json_arr_len(j) > 0 &&
        sh_json_type_of(sh_json_arr_get(j, 0)) == SH_JSON_ARR)
        arr = sh_json_arr_get(j, 0);
    if (sh_json_type_of(arr) != SH_JSON_ARR) return 0;
    n = sh_json_arr_len(arr);
    if (n == 0 || n > 8192) return 0;
    vec = malloc(n * sizeof(float));
    if (!vec) return 0;
    for (size_t i = 0; i < n; i++) {
        const sh_json *el = sh_json_arr_get(arr, i);
        if (!el || sh_json_type_of(el) != SH_JSON_NUM) {
            free(vec);
            return 0;
        }
        vec[i] = (float)sh_json_as_num(el);
    }
    *out_vec = vec;
    *out_dim = (uint32_t)n;
    return 1;
}

static int parse_vector_text(const char *text, size_t len, float **out_vec, uint32_t *out_dim) {
    sh_json *j;
    size_t start = 0, end;

    *out_vec = NULL;
    *out_dim = 0;
    if (!text || len == 0) return 0;

    while (start < len && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n' ||
                           text[start] == '\r'))
        start++;
    if (start >= len) return 0;

    /* Whole-text parse (handles both [..] and [[..]]). */
    j = sh_json_parse(text + start, len - start);
    if (j && json_to_vector(j, out_vec, out_dim)) {
        sh_json_free(j);
        return 1;
    }
    sh_json_free(j);

    /* Line-per-array fallback: take the first line that parses as numbers. */
    end = start;
    while (end < len) {
        size_t eol = end;
        while (eol < len && text[eol] != '\n') eol++;
        if (eol > end) {
            j = sh_json_parse(text + end, eol - end);
            if (j && json_to_vector(j, out_vec, out_dim)) {
                sh_json_free(j);
                return 1;
            }
            sh_json_free(j);
        }
        end = eol + 1;
    }
    return 0;
}

int sh_index_embed_text(const char *embedder, const char *work_dir, const char *text,
                        float **out_vec, uint32_t *out_dim) {
    char *output = NULL;
    size_t len = 0;
    int rc;

    if (out_vec) *out_vec = NULL;
    if (out_dim) *out_dim = 0;
    if (!embedder || !embedder[0] || !text) return 0;
    if (!sh_index_run_command(work_dir, embedder, text, strlen(text), &output, &len)) return 0;
    rc = parse_vector_text(output, len, out_vec, out_dim);
    free(output);
    return rc;
}
