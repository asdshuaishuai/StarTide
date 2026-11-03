/*
 * sh_fs_posix.c - POSIX implementation of the sh_fs backend.
 *
 * Linux, macOS and other Unices.  Symlinks are first-class: the scanner
 * must be able to tell a real directory from a link without resolving it.
 */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "sh_fs.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct sh_dir {
    DIR *dir;
    char *base;
};

const char *sh_fs_backend(void) { return "posix"; }

const char *sh_fs_strerror(sh_fs_status st) {
    switch (st) {
        case SH_FS_OK: return "ok";
        case SH_FS_NOT_FOUND: return "not found";
        case SH_FS_EXISTS: return "already exists";
        case SH_FS_PERMISSION: return "permission denied";
        case SH_FS_IO_ERROR: return "i/o error";
        case SH_FS_NOT_SUPPORTED: return "not supported";
    }
    return "unknown error";
}

int64_t sh_now(void) { return (int64_t)time(NULL); }

int sh_process_id(void) { return (int)getpid(); }

/* ---------------------------------------------------------------- paths */

int sh_path_is_abs(const char *p) {
    if (!p || !*p) return 0;
    return p[0] == '/';
}

char *sh_path_join(const char *a, const char *b) {
    if (!a || !*a) return b ? strdup(b) : NULL;
    if (!b || !*b) return strdup(a);
    if (sh_path_is_abs(b)) return strdup(b);

    size_t la = strlen(a);
    size_t lb = strlen(b);
    while (la > 1 && a[la - 1] == '/') la--;
    while (lb > 0 && b[0] == '/') { b++; lb--; }

    char *out = malloc(la + lb + 2);
    if (!out) return NULL;
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb);
    out[la + lb + 1] = '\0';
    return out;
}

char *sh_path_parent(const char *p) {
    if (!p || !*p) return strdup(".");
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    size_t cut = n;
    while (cut > 0 && p[cut - 1] != '/') cut--;
    if (cut == 0) return strdup(".");
    while (cut > 1 && p[cut - 1] == '/') cut--;
    if (cut == 0) return strdup("/");
    char *out = malloc(cut + 1);
    memcpy(out, p, cut);
    out[cut] = '\0';
    return out;
}

const char *sh_path_base(const char *p) {
    if (!p || !*p) return "";
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    size_t start = n;
    while (start > 0 && p[start - 1] != '/') start--;
    return p + start;
}

char *sh_path_expand(const char *p, const char *home) {
    if (!p) return NULL;
    if (p[0] != '~') return strdup(p);
    if (p[1] == '\0') return home ? strdup(home) : strdup(p);
    if (p[1] != '/') return strdup(p);
    if (!home) return strdup(p);
    return sh_path_join(home, p + 2);
}

char *sh_path_normalize(const char *p) {
    if (!p) return NULL;

    int absolute = sh_path_is_abs(p);
    size_t len = strlen(p);
    char **parts = calloc(len + 1, sizeof(char *));
    if (!parts) return NULL;

    size_t count = 0;
    const char *cur = p;
    while (*cur) {
        while (*cur == '/') cur++;
        if (!*cur) break;
        const char *start = cur;
        while (*cur && *cur != '/') cur++;
        size_t seg = (size_t)(cur - start);
        if (seg == 1 && start[0] == '.') continue;
        if (seg == 2 && start[0] == '.' && start[1] == '.') {
            if (count > 0 && strcmp(parts[count - 1], "..") != 0) {
                free(parts[--count]);
            } else if (!absolute) {
                parts[count++] = strdup("..");
            }
            continue;
        }
        parts[count++] = strndup(start, seg);
    }

    size_t cap = len + 2;
    char *out = malloc(cap);
    size_t used = 0;
    if (absolute) out[used++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t seg = strlen(parts[i]);
        if (i > 0 && used + 1 < cap) out[used++] = '/';
        if (used + seg + 1 > cap) { cap = used + seg + 2; out = realloc(out, cap); }
        memcpy(out + used, parts[i], seg);
        used += seg;
        free(parts[i]);
    }
    free(parts);
    if (used == 0) {
        free(out);
        return strdup(absolute ? "/" : ".");
    }
    out[used] = '\0';
    return out;
}

char *sh_path_real(const char *p) {
    if (!p || !*p) return NULL;
    char buf[PATH_MAX];
    if (!realpath(p, buf)) return NULL;
    return strdup(buf);
}

/* Split a normalized path into its segments. */
static char **split_segments(const char *path, size_t *out_count) {
    size_t cap = 8, count = 0;
    char **segs = calloc(cap, sizeof(char *));
    if (!segs) { *out_count = 0; return NULL; }

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        if (count == cap) {
            cap *= 2;
            char **n = realloc(segs, cap * sizeof(char *));
            if (!n) break;
            segs = n;
        }
        segs[count++] = strndup(start, (size_t)(p - start));
    }
    *out_count = count;
    return segs;
}

char *sh_path_relative(const char *base, const char *target) {
    if (!base || !target) return target ? strdup(target) : NULL;

    char *b = sh_path_normalize(base);
    char *t = sh_path_normalize(target);
    if (!b || !t) { free(b); free(t); return target ? strdup(target) : NULL; }

    size_t nb = 0, nt = 0;
    char **bs = split_segments(b, &nb);
    char **ts = split_segments(t, &nt);
    if (!bs || !ts) { free(b); free(t); return strdup(target); }

    size_t common = 0;
    while (common < nb && common < nt && strcmp(bs[common], ts[common]) == 0) common++;

    size_t up = nb - common;

    /* ".." for each level we must climb, then the remaining target segments. */
    size_t cap = up * 3 + nt * 32 + 4;
    char *out = malloc(cap);
    size_t used = 0;
    for (size_t i = 0; i < up; i++) {
        memcpy(out + used, "..", 2);
        used += 2;
        if (i + 1 < up || common < nt) out[used++] = '/';
    }
    for (size_t i = common; i < nt; i++) {
        size_t sl = strlen(ts[i]);
        if (used + sl + 2 > cap) {
            while (used + sl + 2 > cap) cap *= 2;
            out = realloc(out, cap);
        }
        memcpy(out + used, ts[i], sl);
        used += sl;
        if (i + 1 < nt) out[used++] = '/';
    }
    out[used] = '\0';
    if (used == 0) { free(out); out = strdup("."); }

    for (size_t i = 0; i < nb; i++) free(bs[i]);
    for (size_t i = 0; i < nt; i++) free(ts[i]);
    free(bs);
    free(ts);
    free(b);
    free(t);
    return out;
}

int sh_path_has_prefix(const char *path, const char *prefix) {
    if (!path || !prefix) return 0;
    size_t pl = strlen(prefix);
    if (pl == 0) return 1;
    while (pl > 1 && prefix[pl - 1] == '/') pl--;
    if (strncmp(path, prefix, pl) != 0) return 0;
    if (path[pl] == '\0') return 1;
    if (prefix[pl - 1] == '/') return 1;
    return path[pl] == '/';
}

/* ------------------------------------------------------------------- fs */

static sh_fs_status errno_to_status(int err) {
    switch (err) {
        case 0: return SH_FS_OK;
        case ENOENT: return SH_FS_NOT_FOUND;
        case EEXIST: return SH_FS_EXISTS;
        case EACCES: case EPERM: return SH_FS_PERMISSION;
        default: return SH_FS_IO_ERROR;
    }
}

static void fill_info(const struct stat *st, sh_fs_info *out) {
    memset(out, 0, sizeof(*out));
    out->size = (uint64_t)st->st_size;
    out->mtime = (int64_t)st->st_mtime;
    if (S_ISLNK(st->st_mode)) {
        out->kind = SH_FS_KIND_LINK;
        out->is_link = 1;
    } else if (S_ISDIR(st->st_mode)) {
        out->kind = SH_FS_KIND_DIR;
        out->is_dir = 1;
    } else if (S_ISREG(st->st_mode)) {
        out->kind = SH_FS_KIND_FILE;
        out->is_file = 1;
    } else {
        out->kind = SH_FS_KIND_OTHER;
    }
}

sh_fs_status sh_lstat(const char *path, sh_fs_info *out) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno_to_status(errno);
    fill_info(&st, out);
    return SH_FS_OK;
}

sh_fs_status sh_stat(const char *path, sh_fs_info *out) {
    struct stat st;
    if (stat(path, &st) != 0) return errno_to_status(errno);
    fill_info(&st, out);
    return SH_FS_OK;
}

sh_fs_status sh_mkdir_p(const char *path) {
    if (!path || !*path) return SH_FS_IO_ERROR;
    char *copy = strdup(path);
    if (!copy) return SH_FS_IO_ERROR;

    size_t n = strlen(copy);
    while (n > 1 && copy[n - 1] == '/') copy[--n] = '\0';

    for (char *p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            sh_fs_status st = errno_to_status(errno);
            free(copy);
            return st;
        }
        *p = '/';
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        sh_fs_status st = errno_to_status(errno);
        free(copy);
        return st;
    }
    free(copy);
    return SH_FS_OK;
}

sh_fs_status sh_rename(const char *from, const char *to) {
    if (rename(from, to) != 0) return errno_to_status(errno);
    return SH_FS_OK;
}

int sh_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int sh_is_link(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

char *sh_readlink(const char *path) {
    char buf[PATH_MAX];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    return strdup(buf);
}

sh_fs_status sh_symlink(const char *target, const char *linkpath, int target_is_dir) {
    (void)target_is_dir;
    if (symlink(target, linkpath) != 0) return errno_to_status(errno);
    return SH_FS_OK;
}

sh_fs_status sh_read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return errno_to_status(errno);

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return SH_FS_IO_ERROR; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return SH_FS_IO_ERROR; }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return SH_FS_IO_ERROR; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    if (out) *out = buf; else free(buf);
    if (out_len) *out_len = got;
    return SH_FS_OK;
}

sh_fs_status sh_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return errno_to_status(errno);
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return SH_FS_IO_ERROR;
    }
    if (fclose(f) != 0) return SH_FS_IO_ERROR;
    return SH_FS_OK;
}

sh_fs_status sh_remove_tree(const char *path) {
    sh_fs_info info;
    if (sh_lstat(path, &info) != SH_FS_OK) return SH_FS_NOT_FOUND;

    if (info.is_link || !info.is_dir) {
        if (unlink(path) != 0) return errno_to_status(errno);
        return SH_FS_OK;
    }

    sh_dir *d = sh_dir_open(path);
    if (d) {
        sh_dirent ent;
        while (sh_dir_next(d, &ent) == 1) {
            sh_remove_tree(ent.path);
            free(ent.name);
            free(ent.path);
        }
        sh_dir_close(d);
    }
    if (rmdir(path) != 0) return errno_to_status(errno);
    return SH_FS_OK;
}

sh_fs_status sh_copy_tree(const char *src, const char *dst) {
    sh_fs_status st = sh_mkdir_p(dst);
    if (st != SH_FS_OK) return st;

    sh_dir *d = sh_dir_open(src);
    if (!d) return SH_FS_IO_ERROR;

    sh_fs_status result = SH_FS_OK;
    sh_dirent ent;
    while (sh_dir_next(d, &ent) == 1) {
        char *target = sh_path_join(dst, ent.name);
        if (!target) { result = SH_FS_IO_ERROR; free(ent.name); free(ent.path); break; }
        if (ent.kind == SH_FS_KIND_LINK) {
            char *link = sh_readlink(ent.path);
            if (!link) {
                /* An unreadable link would silently diverge the copy from
                 * the source identity; fail instead. */
                result = SH_FS_IO_ERROR;
                free(target);
                free(ent.name);
                free(ent.path);
                break;
            }
            result = sh_symlink(link, target, 0);
            free(link);
            if (result != SH_FS_OK) { free(target); free(ent.name); free(ent.path); break; }
        } else if (ent.kind == SH_FS_KIND_DIR) {
            result = sh_copy_tree(ent.path, target);
            if (result != SH_FS_OK) { free(target); free(ent.name); free(ent.path); break; }
        } else {
            size_t len = 0;
            char *data = NULL;
            sh_fs_status st = sh_read_file(ent.path, &data, &len);
            if (st == SH_FS_OK) st = sh_write_file(target, data, len);
            free(data);
            if (st != SH_FS_OK) {
                result = st;
                free(target);
                free(ent.name);
                free(ent.path);
                break;
            }
        }
        free(target);
        free(ent.name);
        free(ent.path);
    }
    sh_dir_close(d);
    return result;
}

uint64_t sh_dir_size(const char *path) {
    sh_fs_info info;
    if (sh_lstat(path, &info) != SH_FS_OK) return 0;
    if (info.is_link) return 0;
    if (!info.is_dir) return info.size;

    uint64_t total = 0;
    sh_dir *d = sh_dir_open(path);
    if (!d) return 0;
    sh_dirent ent;
    while (sh_dir_next(d, &ent) == 1) {
        total += sh_dir_size(ent.path);
        free(ent.name);
        free(ent.path);
    }
    sh_dir_close(d);
    return total;
}

/* ------------------------------------------------------- dir iteration */

sh_dir *sh_dir_open(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return NULL;
    sh_dir *d = calloc(1, sizeof(sh_dir));
    if (!d) { closedir(dir); return NULL; }
    d->dir = dir;
    d->base = strdup(path);
    return d;
}

int sh_dir_next(sh_dir *d, sh_dirent *out) {
    if (!d || !d->dir) return -1;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(d->dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char *path = sh_path_join(d->base, ent->d_name);
        if (!path) return -1;

        sh_fs_kind kind = SH_FS_KIND_OTHER;
#ifdef DT_DIR
        switch (ent->d_type) {
            case DT_DIR: kind = SH_FS_KIND_DIR; break;
            case DT_REG: kind = SH_FS_KIND_FILE; break;
            case DT_LNK: kind = SH_FS_KIND_LINK; break;
            default: kind = SH_FS_KIND_OTHER; break;
        }
#endif
        if (kind == SH_FS_KIND_OTHER) {
            sh_fs_info info;
            if (sh_lstat(path, &info) == SH_FS_OK) kind = info.kind;
        }

        out->name = strdup(ent->d_name);
        out->path = path;
        out->kind = kind;
        return 1;
    }
    return errno ? -1 : 0;
}

void sh_dir_close(sh_dir *d) {
    if (!d) return;
    if (d->dir) closedir(d->dir);
    free(d->base);
    free(d);
}
