/*
 * sh_scan.h - discovers skill directories on disk.
 *
 * A "skill unit" is a directory holding a SKILL.md file.  Units are
 * maximal: the walk stops as soon as it finds one, so a skill nested
 * inside another skill is part of the outer skill's content rather than a
 * separate unit.  This mirrors how the agents actually enumerate skills.
 */
#ifndef SH_SCAN_H
#define SH_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "sh_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SH_UNIT_MANAGED = 0, /* under a user agent root: may be replaced in place */
    SH_UNIT_EXTERNAL     /* plugin cache, bundled pack: read-only source */
} sh_unit_kind;

typedef struct {
    char *path;         /* absolute path of the unit directory */
    char *basename;     /* directory name */
    char *name;         /* frontmatter name, falls back to the basename */
    char *description;  /* frontmatter description */
    char *hash;         /* content hash, 64 hex chars, may be NULL */
    char *root;         /* deepest skill root containing this unit */
    char *link_target;  /* set when the unit path is itself a symlink */
    uint64_t size;
    size_t file_count;
    int64_t mtime;
    int depth;          /* path components below root */
    sh_unit_kind kind;
} sh_unit;

typedef struct {
    sh_unit *units;
    size_t count;
    size_t cap;

    char **roots; /* discovered skill roots */
    size_t root_count;
    size_t root_cap;

    char **aliases; /* symlinked directories that look like skills */
    size_t alias_count;
    size_t alias_cap;

    size_t dirs_visited;
    size_t links_skipped;
    size_t permissions_denied;
} sh_scan_result;

typedef struct {
    const sh_config *config;
    int compute_hash;
    void (*on_progress)(const char *path, size_t units, void *ud);
    void *ud;
} sh_scan_opts;

int sh_scan_run(const sh_scan_opts *opts, sh_scan_result *out);

void sh_scan_result_free(sh_scan_result *r);

/* Find the unit whose path matches, or NULL. */
sh_unit *sh_scan_find_by_path(sh_scan_result *r, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SH_SCAN_H */
