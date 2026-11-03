/*
 * sh_fs.h - portable filesystem primitives.
 *
 * Everything inside skillctl works with UTF-8 byte strings and '/' as the
 * separator.  On Windows the backend converts to UTF-16 at the syscall
 * boundary and uses '\\' there.
 */
#ifndef SH_FS_H
#define SH_FS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SH_FS_OK = 0,
    SH_FS_NOT_FOUND,
    SH_FS_EXISTS,
    SH_FS_PERMISSION,
    SH_FS_IO_ERROR,
    SH_FS_NOT_SUPPORTED
} sh_fs_status;

typedef enum {
    SH_FS_KIND_OTHER = 0,
    SH_FS_KIND_FILE,
    SH_FS_KIND_DIR,
    SH_FS_KIND_LINK
} sh_fs_kind;

typedef struct {
    sh_fs_kind kind;
    int is_dir;
    int is_file;
    int is_link;
    uint64_t size;
    int64_t mtime; /* seconds since the Unix epoch */
} sh_fs_info;

typedef struct {
    char *name;
    char *path;
    sh_fs_kind kind;
} sh_dirent;

typedef struct sh_dir sh_dir;

/* ---------------------------------------------------------------- paths */

int sh_path_is_abs(const char *p);

/* Join two path fragments.  The result is malloc'd; the caller frees it. */
char *sh_path_join(const char *a, const char *b);

/* Parent directory of p (malloc'd).  Returns "." when p has no parent. */
char *sh_path_parent(const char *p);

/* Last component of p.  Points into p; never NULL. */
const char *sh_path_base(const char *p);

/* Expand a leading "~" (and "~/" only) using home.  malloc'd. */
char *sh_path_expand(const char *p, const char *home);

/* Collapse duplicate separators, drop "." components, fold "..".  malloc'd. */
char *sh_path_normalize(const char *p);

/* Resolve symlinks and make the path absolute.  NULL when it does not exist. */
char *sh_path_real(const char *p);

/*
 * Relative path from base to target, both absolute.  Falls back to the
 * absolute target when the two live on different roots.  malloc'd.
 */
char *sh_path_relative(const char *base, const char *target);

/* True when path equals prefix or is below it (prefix boundary aware). */
int sh_path_has_prefix(const char *path, const char *prefix);

/* ------------------------------------------------------------------- fs */

sh_fs_status sh_lstat(const char *path, sh_fs_info *out); /* no symlink following */
sh_fs_status sh_stat(const char *path, sh_fs_info *out);  /* follows symlinks */

sh_fs_status sh_mkdir_p(const char *path);
sh_fs_status sh_rename(const char *from, const char *to);
sh_fs_status sh_remove_tree(const char *path);
sh_fs_status sh_copy_tree(const char *src, const char *dst);

sh_fs_status sh_read_file(const char *path, char **out, size_t *out_len);
sh_fs_status sh_write_file(const char *path, const void *data, size_t len);

/*
 * Create a link at linkpath pointing at target.  target_is_dir selects the
 * directory flavour of link.  On Windows this may fall back to a junction.
 */
sh_fs_status sh_symlink(const char *target, const char *linkpath, int target_is_dir);

/* Resolve a link.  malloc'd, or NULL when path is not a link. */
char *sh_readlink(const char *path);

int sh_exists(const char *path);
int sh_is_link(const char *path);

/* Total size in bytes of everything below path.  Links are not followed. */
uint64_t sh_dir_size(const char *path);

/* ------------------------------------------------------- dir iteration */

sh_dir *sh_dir_open(const char *path);

/* 1 = entry produced, 0 = end of directory, -1 = error. */
int sh_dir_next(sh_dir *d, sh_dirent *out);

void sh_dir_close(sh_dir *d);

/* ---------------------------------------------------------------- misc */

const char *sh_fs_strerror(sh_fs_status st);
const char *sh_fs_backend(void);

/* Monotonic-ish wall clock seconds since the epoch. */
int64_t sh_now(void);

/* Process id, used to make temporary names unique. */
int sh_process_id(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_FS_H */
