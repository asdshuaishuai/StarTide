/*
 * sh_hash.h - SHA-256 and content-addressed tree hashing.
 */
#ifndef SH_HASH_H
#define SH_HASH_H

#include <stddef.h>
#include <stdint.h>

#include "sh_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char buf[64];
    size_t buflen;
} sh_sha256_ctx;

void sh_sha256_init(sh_sha256_ctx *ctx);
void sh_sha256_update(sh_sha256_ctx *ctx, const void *data, size_t len);
void sh_sha256_final(sh_sha256_ctx *ctx, unsigned char out[32]);

/* Lowercase hex digest of a buffer.  out must hold 65 bytes. */
void sh_sha256_hex(const void *data, size_t len, char out[65]);

/* Lowercase hex digest of a file's contents.  out must hold 65 bytes. */
sh_fs_status sh_sha256_file(const char *path, char out[65]);

/*
 * Hash of a directory tree, used as the content identity of a skill.
 *
 * The digest covers every regular file's relative path and contents, walked
 * in sorted path order.  Dot-files, .git and the usual build noise are
 * skipped so that third-party tools writing lock files into a skill do not
 * change its identity.  Symlinks are hashed by their target string.
 *
 * out must hold 65 bytes.
 */
sh_fs_status sh_sha256_tree(const char *dir, char out[65], uint64_t *out_size, size_t *out_files);

/* Short form of a hex digest: the first n characters. */
void sh_hash_short(const char *hex, size_t n, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* SH_HASH_H */
