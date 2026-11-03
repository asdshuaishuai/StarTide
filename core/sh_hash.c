/*
 * sh_hash.c - SHA-256 (FIPS 180-4) plus content-addressed tree hashing.
 *
 * Self-contained so that skillctl keeps its promise of zero external
 * dependencies and stays trivially portable.
 */
#include "sh_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- sha256 */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(sh_sha256_ctx *ctx, const unsigned char data[64]) {
    uint32_t m[64];
    for (int i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(m[i - 15], 7) ^ ROTR(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = ROTR(m[i - 2], 17) ^ ROTR(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + m[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sh_sha256_init(sh_sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void sh_sha256_update(sh_sha256_ctx *ctx, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        ctx->buf[ctx->buflen++] = p[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buf);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

void sh_sha256_final(sh_sha256_ctx *ctx, unsigned char out[32]) {
    size_t i = ctx->buflen;
    if (ctx->buflen < 56) {
        ctx->buf[i++] = 0x80;
        while (i < 56) ctx->buf[i++] = 0x00;
    } else {
        ctx->buf[i++] = 0x80;
        while (i < 64) ctx->buf[i++] = 0x00;
        sha256_transform(ctx, ctx->buf);
        memset(ctx->buf, 0, 56);
    }

    ctx->bitlen += ctx->buflen * 8;
    for (int b = 0; b < 8; b++) ctx->buf[63 - b] = (unsigned char)(ctx->bitlen >> (b * 8));
    sha256_transform(ctx, ctx->buf);

    for (int b = 0; b < 8; b++)
        for (int n = 0; n < 4; n++)
            out[b * 4 + n] = (unsigned char)(ctx->state[b] >> ((3 - n) * 8));
}

static void hexify(const unsigned char digest[32], char out[65]) {
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

void sh_sha256_hex(const void *data, size_t len, char out[65]) {
    sh_sha256_ctx ctx;
    unsigned char digest[32];
    sh_sha256_init(&ctx);
    sh_sha256_update(&ctx, data, len);
    sh_sha256_final(&ctx, digest);
    hexify(digest, out);
}

sh_fs_status sh_sha256_file(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return SH_FS_NOT_FOUND;

    sh_sha256_ctx ctx;
    unsigned char digest[32];
    sh_sha256_init(&ctx);

    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sh_sha256_update(&ctx, buf, n);
    fclose(f);

    sh_sha256_final(&ctx, digest);
    hexify(digest, out);
    return SH_FS_OK;
}

/* ------------------------------------------------------------ tree hash */

typedef struct {
    char **paths;
    size_t count;
    size_t cap;
} path_list;

static int pl_push(path_list *l, const char *p) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        char **np = realloc(l->paths, cap * sizeof(char *));
        if (!np) return 0;
        l->paths = np;
        l->cap = cap;
    }
    l->paths[l->count++] = strdup(p);
    return 1;
}

static int pl_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Directories that never carry meaningful skill content. */
static int is_noise_dir(const char *name) {
    static const char *names[] = {"node_modules", "__pycache__", ".git", ".svn", ".hg",
                                  "target",      "dist",         "build", "out",     "vendor",
                                  ".venv",       "venv",         ".idea", ".vscode", NULL};
    for (int i = 0; names[i]; i++)
        if (strcmp(names[i], name) == 0) return 1;
    return 0;
}

/* Collect relative paths of every regular file below dir. */
static int collect_files(const char *dir, const char *prefix, path_list *out,
                         uint64_t *size, size_t *count) {
    sh_dir *d = sh_dir_open(dir);
    if (!d) return 0;

    sh_dirent ent;
    while (sh_dir_next(d, &ent) == 1) {
        if (ent.name[0] == '.') { /* dot noise: skipped from the identity */
            free(ent.name);
            free(ent.path);
            continue;
        }
        if (ent.kind == SH_FS_KIND_DIR && is_noise_dir(ent.name)) {
            free(ent.name);
            free(ent.path);
            continue;
        }

        char *rel = prefix && *prefix ? sh_path_join(prefix, ent.name) : strdup(ent.name);

        if (ent.kind == SH_FS_KIND_LINK) {
            pl_push(out, rel);
        } else if (ent.kind == SH_FS_KIND_DIR) {
            collect_files(ent.path, rel, out, size, count);
        } else if (ent.kind == SH_FS_KIND_FILE) {
            sh_fs_info info;
            if (sh_lstat(ent.path, &info) == SH_FS_OK) *size += info.size;
            (*count)++;
            pl_push(out, rel);
        }
        free(rel);
        free(ent.name);
        free(ent.path);
    }
    sh_dir_close(d);
    return 1;
}

sh_fs_status sh_sha256_tree(const char *dir, char out[65], uint64_t *out_size,
                            size_t *out_files) {
    path_list list = {0};
    uint64_t size = 0;
    size_t count = 0;

    if (!collect_files(dir, "", &list, &size, &count)) return SH_FS_IO_ERROR;
    if (list.count > 1) qsort(list.paths, list.count, sizeof(char *), pl_cmp);

    sh_sha256_ctx ctx;
    sh_sha256_init(&ctx);

    /* The directory name itself is not part of the identity. */
    for (size_t i = 0; i < list.count; i++) {
        const char *rel = list.paths[i];
        sh_sha256_update(&ctx, rel, strlen(rel));
        sh_sha256_update(&ctx, "\0", 1);

        char *full = sh_path_join(dir, rel);
        sh_fs_info info;
        if (full && sh_lstat(full, &info) == SH_FS_OK && info.is_link) {
            char *target = sh_readlink(full);
            if (target) {
                sh_sha256_update(&ctx, target, strlen(target));
                free(target);
            }
        } else if (full) {
            FILE *f = fopen(full, "rb");
            if (f) {
                unsigned char buf[65536];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sh_sha256_update(&ctx, buf, n);
                fclose(f);
            }
        }
        sh_sha256_update(&ctx, "\0", 1);
        free(full);
    }

    unsigned char digest[32];
    sh_sha256_final(&ctx, digest);
    hexify(digest, out);

    for (size_t i = 0; i < list.count; i++) free(list.paths[i]);
    free(list.paths);

    if (out_size) *out_size = size;
    if (out_files) *out_files = count;
    return SH_FS_OK;
}

void sh_hash_short(const char *hex, size_t n, char *out, size_t out_len) {
    if (out_len == 0) return;
    size_t len = strlen(hex);
    if (n > len) n = len;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, hex, n);
    out[n] = '\0';
}
