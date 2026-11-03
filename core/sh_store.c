/*
 * sh_store.c - content addressed store, catalog and view projection.
 */
#include "sh_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sh_json.h"

char *sh_store_units_dir(const char *h) { return sh_path_join(h, "units"); }
char *sh_store_views_dir(const char *h) { return sh_path_join(h, "views"); }
char *sh_store_trash_dir(const char *h) { return sh_path_join(h, "trash"); }
char *sh_store_catalog_path(const char *h) { return sh_path_join(h, "catalog.jsonl"); }

char *sh_store_unit_path(const char *store_home, const char *id) {
    char *units = sh_store_units_dir(store_home);
    char *dir = sh_path_join(units, id);
    free(units);
    return dir;
}

char *sh_store_view_dir(const char *store_home, const char *agent) {
    char *views = sh_store_views_dir(store_home);
    char *dir = sh_path_join(views, agent);
    free(views);
    return dir;
}

sh_fs_status sh_store_init(const char *store_home) {
    sh_fs_status st = sh_mkdir_p(store_home);
    if (st != SH_FS_OK) return st;
    char *units = sh_store_units_dir(store_home);
    st = sh_mkdir_p(units);
    free(units);
    if (st != SH_FS_OK) return st;
    char *views = sh_store_views_dir(store_home);
    st = sh_mkdir_p(views);
    free(views);
    if (st != SH_FS_OK) return st;
    char *trash = sh_store_trash_dir(store_home);
    st = sh_mkdir_p(trash);
    free(trash);
    return st;
}

int sh_store_has_unit(const char *store_home, const char *id) {
    char *path = sh_store_unit_path(store_home, id);
    sh_fs_info info;
    int found = sh_stat(path, &info) == SH_FS_OK && info.is_dir;
    free(path);
    return found;
}

sh_fs_status sh_store_import(const char *store_home, const char *src, const char *id,
                             char **out_path) {
    if (sh_store_has_unit(store_home, id)) {
        if (out_path) *out_path = sh_store_unit_path(store_home, id);
        return SH_FS_OK;
    }

    char *units = sh_store_units_dir(store_home);
    if (sh_mkdir_p(units) != SH_FS_OK) { free(units); return SH_FS_IO_ERROR; }

    /* Stage into a temporary directory, then rename into place, so a crash
     * never leaves a half-copied unit behind. */
    char tmpname[128];
    snprintf(tmpname, sizeof(tmpname), ".incoming-%d-%s", sh_process_id(), id);
    char *tmp = sh_path_join(units, tmpname);
    free(units);

    sh_remove_tree(tmp);
    sh_fs_status st = sh_copy_tree(src, tmp);
    if (st != SH_FS_OK) {
        sh_remove_tree(tmp);
        free(tmp);
        return st;
    }

    char *final = sh_store_unit_path(store_home, id);
    st = sh_rename(tmp, final);
    if (st != SH_FS_OK) {
        /* Another process may have won the race. */
        if (sh_store_has_unit(store_home, id)) st = SH_FS_OK;
        else {
            sh_remove_tree(tmp);
            free(tmp);
            free(final);
            return st;
        }
    }
    free(tmp);

    if (out_path) *out_path = final;
    else free(final);
    return SH_FS_OK;
}

sh_fs_status sh_store_remove_unit(const char *store_home, const char *id) {
    char *path = sh_store_unit_path(store_home, id);
    sh_fs_status st = sh_remove_tree(path);
    free(path);
    return st;
}

/* ------------------------------------------------------------ catalog */

static void entry_push_source(sh_catalog_entry *e, const char *path) {
    for (size_t i = 0; i < e->source_count; i++)
        if (strcmp(e->sources[i], path) == 0) return;
    if (e->source_count == e->source_cap) {
        size_t cap = e->source_cap ? e->source_cap * 2 : 4;
        char **n = realloc(e->sources, cap * sizeof(char *));
        if (!n) return;
        e->sources = n;
        e->source_cap = cap;
    }
    e->sources[e->source_count++] = strdup(path);
}

void sh_catalog_add_source(sh_catalog_entry *e, const char *path) {
    if (e && path) entry_push_source(e, path);
}

sh_catalog_entry *sh_catalog_find(sh_catalog *c, const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->entries[i].id, id) == 0) return &c->entries[i];
    return NULL;
}

sh_catalog_entry *sh_catalog_upsert(sh_catalog *c, const char *id) {
    sh_catalog_entry *e = sh_catalog_find(c, id);
    if (e) return e;
    if (c->count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 128;
        sh_catalog_entry *n = realloc(c->entries, cap * sizeof(sh_catalog_entry));
        if (!n) return NULL;
        c->entries = n;
        c->cap = cap;
    }
    e = &c->entries[c->count++];
    memset(e, 0, sizeof(*e));
    e->id = strdup(id);
    return e;
}

int sh_catalog_load(sh_catalog *c, const char *path) {
    memset(c, 0, sizeof(*c));
    char *data = NULL;
    size_t len = 0;
    if (sh_read_file(path, &data, &len) != SH_FS_OK) return 0;

    size_t pos = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && data[eol] != '\n') eol++;
        size_t line_len = eol - pos;
        if (line_len > 0) {
            sh_json *rec = sh_json_parse(data + pos, line_len);
            if (rec && sh_json_type_of(rec) == SH_JSON_OBJ) {
                const char *id = sh_json_obj_str(rec, "id");
                if (id) {
                    sh_catalog_entry *e = sh_catalog_upsert(c, id);
                    const char *name = sh_json_obj_str(rec, "name");
                    if (name && !e->name) e->name = strdup(name);
                    const char *desc = sh_json_obj_str(rec, "description");
                    if (desc && !e->description) e->description = strdup(desc);
                    e->size = (uint64_t)sh_json_obj_num(rec, "size", 0);
                    e->file_count = (size_t)sh_json_obj_num(rec, "files", 0);
                    e->mtime = (int64_t)sh_json_obj_num(rec, "mtime", 0);
                    /* The flag is written as a JSON bool; accept a number too. */
                    const sh_json *stored = sh_json_obj_get(rec, "stored");
                    e->in_store = stored && (sh_json_as_bool(stored) || sh_json_as_num(stored) != 0);
                    const sh_json *srcs = sh_json_obj_get(rec, "sources");
                    for (size_t i = 0; i < sh_json_arr_len(srcs); i++) {
                        const char *s = sh_json_as_str(sh_json_arr_get(srcs, i));
                        if (s) entry_push_source(e, s);
                    }
                }
            }
            sh_json_free(rec);
        }
        pos = eol + 1;
    }
    free(data);
    return 1;
}

int sh_catalog_save(const sh_catalog *c, const char *path) {
    sh_jbuf buf;
    sh_jbuf_init(&buf);

    for (size_t i = 0; i < c->count; i++) {
        const sh_catalog_entry *e = &c->entries[i];
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "id");
        sh_jw_str(&w, e->id);
        sh_jw_key(&w, "name");
        sh_jw_str(&w, e->name ? e->name : "");
        sh_jw_key(&w, "description");
        sh_jw_str(&w, e->description ? e->description : "");
        sh_jw_key(&w, "size");
        sh_jw_numf(&w, (double)e->size);
        sh_jw_key(&w, "files");
        sh_jw_num(&w, (long long)e->file_count);
        sh_jw_key(&w, "mtime");
        sh_jw_num(&w, (long long)e->mtime);
        sh_jw_key(&w, "stored");
        sh_jw_bool(&w, e->in_store);
        sh_jw_key(&w, "sources");
        sh_jw_arr_begin(&w);
        for (size_t s = 0; s < e->source_count; s++) sh_jw_str(&w, e->sources[s]);
        sh_jw_arr_end(&w);
        sh_jw_obj_end(&w);

        sh_jbuf_puts(&buf, sh_jw_text(&w));
        sh_jbuf_putc(&buf, '\n');
        sh_jw_free(&w);
    }

    sh_fs_status st = sh_write_file(path, buf.data ? buf.data : "", buf.len);
    sh_jbuf_free(&buf);
    return st == SH_FS_OK;
}

void sh_catalog_free(sh_catalog *c) {
    if (!c) return;
    for (size_t i = 0; i < c->count; i++) {
        free(c->entries[i].id);
        free(c->entries[i].name);
        free(c->entries[i].description);
        for (size_t s = 0; s < c->entries[i].source_count; s++) free(c->entries[i].sources[s]);
        free(c->entries[i].sources);
    }
    free(c->entries);
    memset(c, 0, sizeof(*c));
}

/* -------------------------------------------------------------- views */

static char *timestamp_name(void) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return strdup(buf);
}

/*
 * Destination inside trash/ for content moved out of a view.  The stamp
 * keeps a later run from colliding with an older trash entry of the same
 * name.
 */
static char *trash_dest(const char *trash, const char *name) {
    char *stamp = timestamp_name();
    if (!stamp) return NULL;
    size_t need = strlen(trash) + strlen(name) + strlen(stamp) + 4;
    char *dest = malloc(need);
    if (dest) snprintf(dest, need, "%s/%s-%s", trash, name, stamp);
    free(stamp);
    return dest;
}

/* Relative link target from views/<agent>/<name> to units/<id>. */
static char *unit_link_target(const char *agent, const char *id) {
    char *rel = malloc(strlen(id) + 16);
    snprintf(rel, strlen(id) + 16, "../../units/%s", id);
    (void)agent;
    return rel;
}

static int find_entry(const sh_link_entry *entries, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(entries[i].name, name) == 0) return 1;
    return 0;
}

sh_fs_status sh_view_sync(const char *store_home, const char *agent,
                          const sh_link_entry *entries, size_t count, int replace_real_dirs,
                          int dry_run, char **out_trash) {
    char *view = sh_store_view_dir(store_home, agent);
    sh_fs_status st = SH_FS_OK;

    if (!dry_run) {
        st = sh_mkdir_p(view);
        if (st != SH_FS_OK) { free(view); return st; }
    }

    char *trash = NULL;
    if (!dry_run && replace_real_dirs) {
        trash = sh_store_trash_dir(store_home);
        sh_mkdir_p(trash);
    }

    /* First pass: drop links that are no longer wanted. */
    if (!dry_run) {
        sh_dir *d = sh_dir_open(view);
        if (d) {
            sh_dirent ent;
            while (sh_dir_next(d, &ent) == 1) {
                if (ent.name[0] == '.') { free(ent.name); free(ent.path); continue; }
                if (find_entry(entries, count, ent.name)) { free(ent.name); free(ent.path); continue; }

                if (ent.kind == SH_FS_KIND_LINK) {
                    sh_remove_tree(ent.path);
                } else if (ent.kind == SH_FS_KIND_DIR && replace_real_dirs && trash) {
                    /* Foreign content: keep it aside instead of destroying it. */
                    char *dest = trash_dest(trash, ent.name);
                    if (dest) {
                        sh_fs_status rst = sh_rename(ent.path, dest);
                        if (rst != SH_FS_OK) st = rst;
                        free(dest);
                    } else {
                        st = SH_FS_IO_ERROR;
                    }
                }
                free(ent.name);
                free(ent.path);
            }
            sh_dir_close(d);
        }
    }

    /* Second pass: create or repoint every wanted link. */
    if (!dry_run) {
        for (size_t i = 0; i < count; i++) {
            char *link = sh_path_join(view, entries[i].name);
            char *target = unit_link_target(agent, entries[i].id);

            char *existing = sh_readlink(link);
            if (existing) {
                /*
                 * The stored link text may differ from target while both
                 * point at the same unit: a Windows junction keeps an
                 * absolute substitute name, not the relative target.  So
                 * compare the resolved paths when both sides resolve.
                 */
                int same = 0;
                char *unit = sh_store_unit_path(store_home, entries[i].id);
                char *real_link = sh_path_real(link);
                char *real_unit = sh_path_real(unit);
                if (real_link && real_unit) same = strcmp(real_link, real_unit) == 0;
                else same = strcmp(existing, target) == 0;
                free(real_link);
                free(real_unit);
                free(unit);
                if (!same) {
                    sh_remove_tree(link);
                    sh_fs_status lst = sh_symlink(target, link, 1);
                    if (lst != SH_FS_OK) st = lst;
                }
                free(existing);
            } else {
                sh_fs_info info;
                if (sh_lstat(link, &info) == SH_FS_OK) {
                    if (info.is_dir) {
                        if (replace_real_dirs && trash) {
                            char *dest = trash_dest(trash, entries[i].name);
                            if (dest) {
                                sh_fs_status rst = sh_rename(link, dest);
                                if (rst != SH_FS_OK) st = rst;
                                free(dest);
                            } else {
                                st = SH_FS_IO_ERROR;
                            }
                        } else {
                            free(link);
                            free(target);
                            continue; /* leave foreign content alone */
                        }
                    } else {
                        sh_remove_tree(link);
                    }
                }
                sh_fs_status lst = sh_symlink(target, link, 1);
                if (lst != SH_FS_OK) st = lst;
            }
            free(link);
            free(target);
        }
    }

    if (out_trash) *out_trash = trash; else free(trash);
    free(view);
    return st;
}

/* -------------------------------------------------------------- roots */

sh_fs_status sh_link_root(const char *root_path, const char *view_dir, const char *store_home,
                          const char *agent, int dry_run, char **out_trash) {
    char *parent = sh_path_parent(root_path);

    /* Already a link? */
    char *existing = sh_readlink(root_path);
    if (existing) {
        char *real_existing = sh_path_real(root_path);
        char *real_view = sh_path_real(view_dir);
        int matches = real_existing && real_view && strcmp(real_existing, real_view) == 0;
        free(real_existing);
        free(real_view);
        free(existing);
        free(parent);
        return matches ? SH_FS_OK : SH_FS_EXISTS;
    }

    char *real_view = sh_path_real(view_dir);
    /*
     * The link target is resolved relative to the directory that holds the
     * link, which the kernel sees by its real path.  Both sides of the
     * relative path must therefore be real, otherwise a home directory that
     * is itself reached through a symlink (macOS puts /tmp behind
     * /private/tmp) produces a path that resolves one level too high.
     */
    char *real_parent = sh_path_real(parent);
    char *base = real_parent ? real_parent : parent;
    char *target = real_view ? sh_path_relative(base, real_view) : strdup(view_dir);
    free(real_parent);

    if (dry_run) {
        free(target);
        free(parent);
        return SH_FS_OK;
    }

    sh_fs_status st = SH_FS_OK;
    sh_fs_info info;
    if (sh_lstat(root_path, &info) == SH_FS_OK && info.is_dir) {
        /*
         * Move the original aside.  Never delete it.  The trash entry is
         * named after the agent, because every agent's directory is called
         * "skills" and a shared name would collide.
         */
        char *trash = sh_store_trash_dir(store_home);
        char *stamp = timestamp_name();
        size_t need = strlen(trash) + strlen(stamp) + strlen(agent ? agent : "agent") + 32;
        char *dest = malloc(need);
        snprintf(dest, need, "%s/%s-%s", trash, agent ? agent : "agent", stamp);
        sh_mkdir_p(trash);
        char *final = sh_path_join(dest, sh_path_base(root_path));
        st = sh_mkdir_p(dest);
        if (st == SH_FS_OK) st = sh_rename(root_path, final);
        if (st != SH_FS_OK) sh_remove_tree(final);
        if (out_trash) *out_trash = dest; else free(dest);
        free(final);
        free(stamp);
        free(trash);
        if (st != SH_FS_OK) {
            free(target);
            free(parent);
            return st;
        }
    }

    st = sh_symlink(target, root_path, 1);
    free(target);
    free(parent);
    return st;
}

sh_fs_status sh_unlink_root(const char *root_path, const char *store_home, const char *agent) {
    char *existing = sh_readlink(root_path);
    if (!existing) return SH_FS_NOT_FOUND;

    /* Find the newest trash entry that holds the original directory. */
    char *trash = sh_store_trash_dir(store_home);
    sh_dir *d = sh_dir_open(trash);
    if (!d) { free(trash); free(existing); return SH_FS_NOT_FOUND; }

    char *best = NULL;
    sh_dirent ent;
    while (sh_dir_next(d, &ent) == 1) {
        if (ent.kind != SH_FS_KIND_DIR) { free(ent.name); free(ent.path); continue; }

        /* Only look at trash entries belonging to this agent. */
        if (agent && !sh_path_has_prefix(ent.path, trash)) { free(ent.name); free(ent.path); continue; }
        if (agent) {
            const char *base = sh_path_base(ent.path);
            size_t alen = strlen(agent);
            if (strncmp(base, agent, alen) != 0 || base[alen] != '-') {
                free(ent.name);
                free(ent.path);
                continue;
            }
        }

        char *candidate = sh_path_join(ent.path, sh_path_base(root_path));
        sh_fs_info info;
        if (sh_stat(candidate, &info) == SH_FS_OK && info.is_dir) {
            free(best);
            best = candidate;
        } else {
            free(candidate);
        }
        free(ent.name);
        free(ent.path);
    }
    sh_dir_close(d);

    if (!best) { free(trash); free(existing); return SH_FS_NOT_FOUND; }

    sh_remove_tree(root_path);
    sh_fs_status st = sh_rename(best, root_path);
    free(best);
    free(trash);
    free(existing);
    return st;
}
