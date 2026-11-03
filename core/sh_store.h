/*
 * sh_store.h - the content addressed store of skill material.
 *
 * Layout under skillctl's home directory:
 *
 *   units/<id>/      real content, <id> is the first 16 hex chars of the
 *                    SHA-256 of the skill tree
 *   views/<agent>/   a directory of symlinks, one per skill, pointing at
 *                    ../../units/<id>
 *   trash/<agent>-<timestamp>/   originals replaced by a symlink, kept so the
 *                    operation can be undone
 *   catalog.jsonl    one JSON record per known skill
 */
#ifndef SH_STORE_H
#define SH_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "sh_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

char *sh_store_units_dir(const char *store_home);
char *sh_store_views_dir(const char *store_home);
char *sh_store_trash_dir(const char *store_home);
char *sh_store_catalog_path(const char *store_home);
char *sh_store_unit_path(const char *store_home, const char *id);
char *sh_store_view_dir(const char *store_home, const char *agent);

/* Create the directory skeleton. */
sh_fs_status sh_store_init(const char *store_home);

int sh_store_has_unit(const char *store_home, const char *id);

/*
 * Copy src into the store under the given id.  When the unit already
 * exists nothing happens.  out_path, when not NULL, receives the store
 * path of the unit.
 */
sh_fs_status sh_store_import(const char *store_home, const char *src, const char *id,
                             char **out_path);

/* Remove a unit directory. */
sh_fs_status sh_store_remove_unit(const char *store_home, const char *id);

/* ------------------------------------------------------- the catalog */

typedef struct {
    char *id;
    char *name;
    char *description;
    char **sources;
    size_t source_count;
    size_t source_cap;
    uint64_t size;
    size_t file_count;
    int64_t mtime;
    int in_store;
} sh_catalog_entry;

typedef struct {
    sh_catalog_entry *entries;
    size_t count;
    size_t cap;
} sh_catalog;

int sh_catalog_load(sh_catalog *c, const char *path);
int sh_catalog_save(const sh_catalog *c, const char *path);
sh_catalog_entry *sh_catalog_find(sh_catalog *c, const char *id);
sh_catalog_entry *sh_catalog_upsert(sh_catalog *c, const char *id);
void sh_catalog_add_source(sh_catalog_entry *e, const char *path);
void sh_catalog_free(sh_catalog *c);

/* ------------------------------------------------------------- views */

typedef struct {
    char *name; /* link name inside the view */
    char *id;   /* store unit id */
} sh_link_entry;

/*
 * Bring views/<agent> in line with the given entries.  Existing links that
 * point somewhere else are repointed, links that are no longer wanted are
 * removed.  Real directories are never touched unless replace_real_dirs is
 * set, in which case they are moved to the trash first.  With dry_run set
 * nothing on disk is created, removed or renamed.
 */
sh_fs_status sh_view_sync(const char *store_home, const char *agent,
                          const sh_link_entry *entries, size_t count, int replace_real_dirs,
                          int dry_run, char **trash_dir);

/*
 * Replace an agent's skills directory with a symlink to its view.
 * The original directory is moved into the trash, never deleted.
 */
sh_fs_status sh_link_root(const char *root_path, const char *view_dir, const char *store_home,
                          const char *agent, int dry_run, char **out_trash_path);

/* Restore an agent's skills directory from the trash. */
sh_fs_status sh_unlink_root(const char *root_path, const char *store_home, const char *agent);

#ifdef __cplusplus
}
#endif

#endif /* SH_STORE_H */
