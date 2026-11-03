/*
 * sh_fs_win32.c - Windows implementation of the sh_fs backend.
 *
 * Paths cross the boundary as UTF-8 and are widened per call.  Symlinks
 * are detected through the reparse-point attribute; when the machine has
 * no symlink privilege the link creation falls back to a junction, which
 * needs no elevation and behaves like a directory link for our purposes.
 *
 * NOTE: written against the documented Win32 API and shape-checked
 * against the POSIX backend, but compiled only on Windows.  Parts of it
 * (especially junction creation) deserve a pass on a real Windows box
 * before shipping.
 */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <stddef.h>

#include "sh_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK (0xA000000CL)
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT (0xA0000003L)
#endif
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (0x2)
#endif
#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT (0x900A8)
#endif
#ifndef FSCTL_SET_REPARSE_POINT
#define FSCTL_SET_REPARSE_POINT (0x900A4)
#endif

struct sh_dir {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    int started;
    char *base;
};

const char *sh_fs_backend(void) { return "win32"; }

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

int sh_process_id(void) { return (int)GetCurrentProcessId(); }

/* ------------------------------------------------------- conversion */

static wchar_t *wide_dup(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *out = malloc((size_t)n * sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, n);
    return out;
}

static char *utf8_dup(const wchar_t *wide) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *out = malloc((size_t)n);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, n, NULL, NULL);
    return out;
}

/* Widen a path and normalise separators for the Win32 API. */
static wchar_t *winify(const char *path) {
    wchar_t *w = wide_dup(path);
    if (!w) return NULL;
    for (wchar_t *p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    return w;
}

/* ---------------------------------------------------------------- paths */

int sh_path_is_abs(const char *p) {
    if (!p || !*p) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') return 1;
    return 0;
}

char *sh_path_join(const char *a, const char *b) {
    if (!a || !*a) return b ? strdup(b) : NULL;
    if (!b || !*b) return strdup(a);
    if (sh_path_is_abs(b)) return strdup(b);

    size_t la = strlen(a);
    size_t lb = strlen(b);
    while (la > 0 && (a[la - 1] == '/' || a[la - 1] == '\\')) la--;
    while (lb > 0 && (b[0] == '/' || b[0] == '\\')) { b++; lb--; }

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
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) n--;
    size_t cut = n;
    while (cut > 0 && p[cut - 1] != '/' && p[cut - 1] != '\\') cut--;
    if (cut == 0) return strdup(".");
    while (cut > 1 && (p[cut - 1] == '/' || p[cut - 1] == '\\')) cut--;
    if (cut == 0) return strdup("/");
    char *out = malloc(cut + 1);
    if (!out) return NULL;
    memcpy(out, p, cut);
    out[cut] = '\0';
    return out;
}

const char *sh_path_base(const char *p) {
    if (!p || !*p) return "";
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) n--;
    size_t start = n;
    while (start > 0 && p[start - 1] != '/' && p[start - 1] != '\\') start--;
    return p + start;
}

char *sh_path_expand(const char *p, const char *home) {
    if (!p) return NULL;
    if (p[0] != '~') return strdup(p);
    if (p[1] == '\0') return home ? strdup(home) : strdup(p);
    if (p[1] != '/' && p[1] != '\\') return strdup(p);
    if (!home) {
        char buf[MAX_PATH];
        if (GetEnvironmentVariableA("USERPROFILE", buf, sizeof(buf)) == 0) return strdup(p);
        home = buf;
    }
    return sh_path_join(home, p + 2);
}

char *sh_path_normalize(const char *p) {
    if (!p) return NULL;

    int absolute = sh_path_is_abs(p);
    /* Keep a drive prefix such as C: intact. */
    char drive[4] = {0};
    const char *rest = p;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        drive[0] = p[0];
        drive[1] = ':';
        if (p[2] == '/' || p[2] == '\\') {
            drive[2] = '/';
            rest = p + 3;
        } else {
            rest = p + 2;
        }
        absolute = 1;
    }

    size_t len = strlen(rest);
    char **parts = calloc(len + 1, sizeof(char *));
    if (!parts) return NULL;
    size_t count = 0;

    const char *cur = rest;
    while (*cur) {
        while (*cur == '/' || *cur == '\\') cur++;
        if (!*cur) break;
        const char *start = cur;
        while (*cur && *cur != '/' && *cur != '\\') cur++;
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

    size_t cap = len + strlen(drive) + 2;
    char *out = malloc(cap);
    size_t used = 0;
    size_t dl = strlen(drive);
    memcpy(out, drive, dl);
    used += dl;
    if (absolute && drive[2] == '/') out[used++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t seg = strlen(parts[i]);
        if (i > 0) out[used++] = '/';
        if (used + seg + 1 > cap) {
            cap = used + seg + 2;
            out = realloc(out, cap);
        }
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
    wchar_t *w = winify(p);
    if (!w) return NULL;

    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFinalPathNameByHandleW(h, buf, MAX_PATH * 2, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n > MAX_PATH * 2) return NULL;

    wchar_t *path = buf;
    /* Strip the \\?\ and \\?\UNC\ prefixes. */
    if (n >= 6 && wcsncmp(path, L"\\\\?\\UNC\\", 6) == 0) {
        memmove(path + 2, path + 6, (n - 6 + 1) * sizeof(wchar_t));
        path[0] = L'\\';
        path[1] = L'\\';
    } else if (n >= 4 && wcsncmp(path, L"\\\\?\\", 4) == 0) {
        path += 4;
    }

    char *out = utf8_dup(path);
    /* Back to forward slashes, matching the internal convention. */
    for (char *q = out; q && *q; q++)
        if (*q == '\\') *q = '/';
    return out;
}

char *sh_path_relative(const char *base, const char *target) {
    if (!base || !target) return target ? strdup(target) : NULL;

    char *b = sh_path_normalize(base);
    char *t = sh_path_normalize(target);
    if (!b || !t) { free(b); free(t); return target ? strdup(target) : NULL; }

    /* Split into segments. */
    size_t nb = 0, nt = 0;
    char **bs = calloc(strlen(b) + 2, sizeof(char *));
    char **ts = calloc(strlen(t) + 2, sizeof(char *));
    if (bs && ts) {
        for (const char *p = b; *p;) {
            while (*p == '/') p++;
            if (!*p) break;
            const char *s = p;
            while (*p && *p != '/') p++;
            bs[nb++] = strndup(s, (size_t)(p - s));
        }
        for (const char *p = t; *p;) {
            while (*p == '/') p++;
            if (!*p) break;
            const char *s = p;
            while (*p && *p != '/') p++;
            ts[nt++] = strndup(s, (size_t)(p - s));
        }
    }

    size_t common = 0;
    while (common < nb && common < nt && strcmp(bs[common], ts[common]) == 0) common++;

    size_t up = nb - common;
    size_t cap = up * 3 + nt * 64 + 4;
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
    /* Case insensitive on Windows filesystems. */
    if (_strnicmp(path, prefix, pl) != 0) return 0;
    if (path[pl] == '\0') return 1;
    if (prefix[pl - 1] == '/' || prefix[pl - 1] == '\\') return 1;
    return path[pl] == '/' || path[pl] == '\\';
}

/* ------------------------------------------------------------------- fs */

static sh_fs_status last_status(void) {
    switch (GetLastError()) {
        case ERROR_SUCCESS: return SH_FS_OK;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return SH_FS_NOT_FOUND;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: return SH_FS_EXISTS;
        case ERROR_ACCESS_DENIED: return SH_FS_PERMISSION;
        default: return SH_FS_IO_ERROR;
    }
}

static void fill_info(const WIN32_FIND_DATAW *data, sh_fs_info *out) {
    memset(out, 0, sizeof(*out));
    DWORD attrs = data->dwFileAttributes;
    out->size = ((uint64_t)data->nFileSizeHigh << 32) | data->nFileSizeLow;
    out->mtime = ((int64_t)data->ftLastWriteTime.dwHighDateTime << 32 |
                  (int64_t)data->ftLastWriteTime.dwLowDateTime) /
                     10000000LL -
                 11644473600LL;

    /*
     * Every reparse point is a link as far as skillctl is concerned:
     * symlinks, but also mount points (junctions), which sh_symlink
     * creates when the machine has no symlink privilege.  Classifying a
     * junction as a directory would let the tree walkers walk straight
     * through it into the linked content, which is exactly what the link
     * semantics must prevent.
     */
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        out->kind = SH_FS_KIND_LINK;
        out->is_link = 1;
        return;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        out->kind = SH_FS_KIND_DIR;
        out->is_dir = 1;
    } else {
        out->kind = SH_FS_KIND_FILE;
        out->is_file = 1;
    }
}

sh_fs_status sh_lstat(const char *path, sh_fs_info *out) {
    wchar_t *w = winify(path);
    if (!w) return SH_FS_IO_ERROR;

    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(w, &data);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return last_status();
    FindClose(h);

    fill_info(&data, out);
    return SH_FS_OK;
}

sh_fs_status sh_stat(const char *path, sh_fs_info *out) {
    wchar_t *w = winify(path);
    if (!w) return SH_FS_IO_ERROR;

    /* Opening without OPEN_REPARSE_POINT follows links. */
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return last_status();

    BY_HANDLE_FILE_INFORMATION info;
    int ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) return last_status();

    WIN32_FIND_DATAW data;
    memset(&data, 0, sizeof(data));
    data.dwFileAttributes = info.dwFileAttributes;
    data.nFileSizeHigh = info.nFileSizeHigh;
    data.nFileSizeLow = info.nFileSizeLow;
    data.ftLastWriteTime = info.ftLastWriteTime;
    fill_info(&data, out);
    return SH_FS_OK;
}

sh_fs_status sh_mkdir_p(const char *path) {
    char *copy = strdup(path);
    if (!copy) return SH_FS_IO_ERROR;

    for (char *p = copy + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        *p = '\0';
        if (sh_path_is_abs(copy) && strlen(copy) <= 3 && copy[1] == ':') { *p = '/'; continue; }
        wchar_t *w = winify(copy);
        if (w) {
            if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                sh_fs_status st = last_status();
                free(w);
                free(copy);
                return st;
            }
            free(w);
        }
        *p = '/';
    }
    wchar_t *w = winify(copy);
    sh_fs_status st = SH_FS_OK;
    if (w) {
        if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            st = last_status();
        free(w);
    }
    free(copy);
    return st;
}

sh_fs_status sh_rename(const char *from, const char *to) {
    wchar_t *wf = winify(from);
    wchar_t *wt = winify(to);
    sh_fs_status st = SH_FS_OK;
    if (wf && wt) {
        if (!MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING)) st = last_status();
    } else {
        st = SH_FS_IO_ERROR;
    }
    free(wf);
    free(wt);
    return st;
}

int sh_exists(const char *path) {
    wchar_t *w = winify(path);
    if (!w) return 0;
    DWORD attrs = GetFileAttributesW(w);
    free(w);
    return attrs != INVALID_FILE_ATTRIBUTES;
}

int sh_is_link(const char *path) {
    wchar_t *w = winify(path);
    if (!w) return 0;
    DWORD attrs = GetFileAttributesW(w);
    free(w);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* Read the reparse buffer of a link and return its target as UTF-8. */
static char *read_reparse_target(const char *path) {
    wchar_t *w = winify(path);
    if (!w) return NULL;

    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    unsigned char buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf,
                              sizeof(buf), &returned, NULL);
    CloseHandle(h);
    if (!ok) return NULL;

    REPARSE_DATA_BUFFER *rd = (REPARSE_DATA_BUFFER *)buf;
    const wchar_t *name = NULL;
    USHORT len = 0;

    if (rd->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        /* GenericReparseBuffer shares storage with the symbolic link union. */
        const BYTE *payload = rd->GenericReparseBuffer.DataBuffer;
        const SYMBOLIC_LINK_REPARSE_BUFFER *sl =
            (const SYMBOLIC_LINK_REPARSE_BUFFER *)payload;
        if (sl->PrintNameLength) {
            name = (const wchar_t *)(payload + sl->PrintNameOffset);
            len = sl->PrintNameLength;
        } else {
            name = (const wchar_t *)(payload + sl->SubstituteNameOffset);
            len = sl->SubstituteNameLength;
        }
    } else if (rd->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        const BYTE *payload = rd->GenericReparseBuffer.DataBuffer;
        const MOUNT_POINT_REPARSE_BUFFER *mp =
            (const MOUNT_POINT_REPARSE_BUFFER *)payload;
        name = (const wchar_t *)(payload + mp->SubstituteNameOffset);
        len = mp->SubstituteNameLength;
    }
    if (!name || len == 0) return NULL;

    wchar_t *target = calloc(len + 1, sizeof(wchar_t));
    memcpy(target, name, len * sizeof(wchar_t));
    char *out = utf8_dup(target);
    free(target);
    for (char *q = out; q && *q; q++) {
        if (*q == '\\') *q = '/';
        /* Junction targets arrive as \??\C:\... */
    }
    if (out && strncmp(out, "/??/", 4) == 0) memmove(out, out + 4, strlen(out + 4) + 1);
    return out;
}

char *sh_readlink(const char *path) {
    if (!sh_is_link(path)) return NULL;
    return read_reparse_target(path);
}

/*
 * Junction layout: the reparse data buffer is not exposed in user mode
 * headers, so spell out the mount point shape ourselves.  The target
 * must be absolute, in \??\ form, and backslash separated.
 */
typedef struct {
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    WORD SubstituteNameOffset;
    WORD SubstituteNameLength;
    WORD PrintNameOffset;
    WORD PrintNameLength;
    WCHAR PathBuffer[1];
} junction_data;

static sh_fs_status create_junction(const char *target, const char *linkpath) {
    wchar_t *wl = winify(linkpath);
    if (!wl) return SH_FS_IO_ERROR;

    if (!CreateDirectoryW(wl, NULL)) {
        sh_fs_status st = last_status();
        free(wl);
        return st;
    }

    char *abs = sh_path_real(target);
    if (!abs) abs = strdup(target);
    size_t need = strlen(abs) + 16;
    char *subst = malloc(need);
    snprintf(subst, need, "\\??\\%s", abs);
    for (char *p = subst; *p; p++)
        if (*p == '/') *p = '\\';
    free(abs);

    size_t subst_bytes = strlen(subst) * sizeof(WCHAR);
    size_t total = offsetof(junction_data, PathBuffer) + subst_bytes + sizeof(WCHAR);

    junction_data *jd = calloc(1, total);
    if (!jd) { free(subst); free(wl); return SH_FS_IO_ERROR; }
    jd->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    jd->ReparseDataLength = (WORD)(offsetof(junction_data, PathBuffer) -
                                   offsetof(junction_data, SubstituteNameOffset) + subst_bytes);
    jd->SubstituteNameOffset = 0;
    jd->SubstituteNameLength = (WORD)subst_bytes;
    jd->PrintNameOffset = (WORD)subst_bytes;
    jd->PrintNameLength = 0;
    MultiByteToWideChar(CP_UTF8, 0, subst, -1, jd->PathBuffer, (int)(subst_bytes / sizeof(WCHAR) + 1));
    free(subst);

    HANDLE h = CreateFileW(wl, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wl);
    if (h == INVALID_HANDLE_VALUE) {
        sh_fs_status st = last_status();
        free(jd);
        return st;
    }

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, jd, (DWORD)total, NULL, 0,
                              &returned, NULL);
    CloseHandle(h);
    free(jd);
    return ok ? SH_FS_OK : last_status();
}

sh_fs_status sh_symlink(const char *target, const char *linkpath, int target_is_dir) {
    wchar_t *wt = winify(target);
    wchar_t *wl = winify(linkpath);
    sh_fs_status st = SH_FS_OK;
    if (!wt || !wl) {
        st = SH_FS_IO_ERROR;
    } else {
        DWORD flags = target_is_dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
        if (!CreateSymbolicLinkW(wl, wt, flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            DWORD err = GetLastError();
            if (err == ERROR_INVALID_PARAMETER)
                flags &= ~SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
            if (!CreateSymbolicLinkW(wl, wt, flags)) {
                /* No symlink privilege: a junction still works for dirs. */
                if (target_is_dir) st = create_junction(target, linkpath);
                else st = last_status();
            }
        }
    }
    free(wt);
    free(wl);
    return st;
}

sh_fs_status sh_read_file(const char *path, char **out, size_t *out_len) {
    wchar_t *w = winify(path);
    if (!w) return SH_FS_IO_ERROR;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return last_status();

    DWORD high = 0;
    DWORD low = GetFileSize(h, &high);
    uint64_t size = ((uint64_t)high << 32) | low;
    char *buf = malloc((size_t)size + 1);
    if (!buf) { CloseHandle(h); return SH_FS_IO_ERROR; }

    DWORD got = 0;
    int ok = ReadFile(h, buf, low, &got, NULL) && got == low;
    CloseHandle(h);
    if (!ok) { free(buf); return SH_FS_IO_ERROR; }
    buf[got] = '\0';
    if (out) *out = buf; else free(buf);
    if (out_len) *out_len = got;
    return SH_FS_OK;
}

sh_fs_status sh_write_file(const char *path, const void *data, size_t len) {
    wchar_t *w = winify(path);
    if (!w) return SH_FS_IO_ERROR;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return last_status();

    DWORD wrote = 0;
    int ok = len == 0 || (WriteFile(h, data, (DWORD)len, &wrote, NULL) && wrote == len);
    CloseHandle(h);
    return ok ? SH_FS_OK : SH_FS_IO_ERROR;
}

sh_fs_status sh_remove_tree(const char *path) {
    wchar_t *w = winify(path);
    if (!w) return SH_FS_IO_ERROR;

    DWORD attrs = GetFileAttributesW(w);
    if (attrs == INVALID_FILE_ATTRIBUTES) { free(w); return last_status(); }

    sh_fs_status st = SH_FS_OK;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        /*
         * A link (symlink or junction): remove the reparse point itself,
         * never the content it points at.  A directory flavoured link is
         * removed with RemoveDirectoryW, DeleteFileW would fail on it.
         */
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            if (!RemoveDirectoryW(w)) st = last_status();
        } else if (!DeleteFileW(w)) {
            st = last_status();
        }
    } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
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
        if (!RemoveDirectoryW(w)) st = last_status();
    } else {
        if (!DeleteFileW(w)) st = last_status();
    }
    free(w);
    return st;
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
        if (ent.kind == SH_FS_KIND_LINK) {
            char *link = sh_readlink(ent.path);
            if (link) {
                /* Whether the link needs the directory flavour is a
                 * property of its target, so stat through the link. */
                sh_fs_info ti;
                sh_stat(ent.path, &ti);
                sh_symlink(link, target, ti.is_dir);
                free(link);
            }
        } else if (ent.kind == SH_FS_KIND_DIR) {
            result = sh_copy_tree(ent.path, target);
            if (result != SH_FS_OK) { free(target); break; }
        } else {
            wchar_t *wf = winify(ent.path);
            wchar_t *wt = winify(target);
            if (wf && wt) {
                if (!CopyFileW(wf, wt, FALSE)) result = last_status();
            }
            free(wf);
            free(wt);
            if (result != SH_FS_OK) { free(target); break; }
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
    size_t need = strlen(path) + 4;
    char *pattern = malloc(need);
    if (!pattern) return NULL;
    snprintf(pattern, need, "%s/*", path);

    wchar_t *w = winify(pattern);
    free(pattern);
    if (!w) return NULL;

    sh_dir *d = calloc(1, sizeof(sh_dir));
    if (!d) { free(w); return NULL; }

    d->handle = FindFirstFileW(w, &d->data);
    free(w);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->started = 1;
    d->base = strdup(path);
    return d;
}

int sh_dir_next(sh_dir *d, sh_dirent *out) {
    if (!d || d->handle == INVALID_HANDLE_VALUE) return -1;

    for (;;) {
        if (!d->started) {
            if (!FindNextFileW(d->handle, &d->data)) {
                return GetLastError() == ERROR_NO_MORE_FILES ? 0 : -1;
            }
        }
        d->started = 0;

        const wchar_t *name = d->data.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;

        char *utf8 = utf8_dup(name);
        char *joined = sh_path_join(d->base, utf8);
        if (!utf8 || !joined) { free(utf8); free(joined); return -1; }

        sh_fs_kind kind;
        DWORD attrs = d->data.dwFileAttributes;
        /* Symlinks and junctions alike are links; see fill_info. */
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            kind = SH_FS_KIND_LINK;
        } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            kind = SH_FS_KIND_DIR;
        } else {
            kind = SH_FS_KIND_FILE;
        }

        out->name = utf8;
        out->path = joined;
        out->kind = kind;
        return 1;
    }
}

void sh_dir_close(sh_dir *d) {
    if (!d) return;
    if (d->handle != INVALID_HANDLE_VALUE) FindClose(d->handle);
    free(d->base);
    free(d);
}

#endif /* _WIN32 */
