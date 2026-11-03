/*
 * sh_scan.c - skill discovery.
 */
#include "sh_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_hash.h"
#include "sh_yaml.h"

typedef struct {
    const sh_scan_opts *opts;
    sh_scan_result *out;
    char **visited; /* realpaths, only used when following symlinks */
    size_t visited_count;
    size_t visited_cap;
} scan_ctx;

static int strlist_add(char ***list, size_t *count, size_t *cap, const char *value) {
    for (size_t i = 0; i < *count; i++)
        if (strcmp((*list)[i], value) == 0) return 0;
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2 : 32;
        char **n = realloc(*list, nc * sizeof(char *));
        if (!n) return 0;
        *list = n;
        *cap = nc;
    }
    (*list)[(*count)++] = strdup(value);
    return 1;
}

static int strlist_has(char **list, size_t count, const char *value) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(list[i], value) == 0) return 1;
    return 0;
}

static int visited_add(scan_ctx *ctx, const char *real) {
    if (!real) return 1;
    if (strlist_has(ctx->visited, ctx->visited_count, real)) return 0;
    return strlist_add(&ctx->visited, &ctx->visited_count, &ctx->visited_cap, real);
}

/* Is "name" a skill root directory name? */
static int is_root_name(const char *name) {
    return strcmp(name, "skills") == 0 || strcmp(name, "skill") == 0;
}

/* Does dir hold a regular file named SKILL.md? */
static int has_skill_md(const char *dir) {
    char *fm = sh_path_join(dir, "SKILL.md");
    sh_fs_info info;
    int found = 0;
    if (fm && sh_lstat(fm, &info) == SH_FS_OK && info.is_file) found = 1;
    free(fm);
    return found;
}

static sh_unit *unit_push(sh_scan_result *r) {
    if (r->count == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 128;
        sh_unit *n = realloc(r->units, cap * sizeof(sh_unit));
        if (!n) return NULL;
        r->units = n;
        r->cap = cap;
    }
    sh_unit *u = &r->units[r->count++];
    memset(u, 0, sizeof(*u));
    return u;
}

/* Read name and description from a unit's SKILL.md. */
static void read_frontmatter(sh_unit *u) {
    char *fm = sh_path_join(u->path, "SKILL.md");
    char *data = NULL;
    size_t len = 0;
    if (fm && sh_read_file(fm, &data, &len) == SH_FS_OK) {
        sh_yaml_doc doc;
        if (sh_yaml_parse(data, len, &doc) == 1) {
            const char *name = sh_yaml_get(&doc, "name");
            if (name && *name) u->name = strdup(name);
            const char *desc = sh_yaml_get(&doc, "description");
            if (desc && *desc) u->description = strdup(desc);
        }
        sh_yaml_free(&doc);
        free(data);
    }
    free(fm);
    if (!u->name) u->name = strdup(u->basename);
}

static void record_unit(scan_ctx *ctx, const char *path, int is_link, const char *link_target) {
    sh_scan_result *r = ctx->out;
    sh_unit *u = unit_push(r);
    if (!u) return;

    u->path = strdup(path);
    u->basename = strdup(sh_path_base(path));
    u->link_target = is_link && link_target ? strdup(link_target) : NULL;
    u->kind = sh_config_is_external(ctx->opts->config, path) ? SH_UNIT_EXTERNAL : SH_UNIT_MANAGED;

    sh_fs_info info;
    if (sh_lstat(path, &info) == SH_FS_OK) u->mtime = info.mtime;

    read_frontmatter(u);
}

static void walk(scan_ctx *ctx, const char *dir, int depth);

static void visit_dir(scan_ctx *ctx, const char *dir, int depth) {
    const sh_config *cfg = ctx->opts->config;
    sh_scan_result *r = ctx->out;

    r->dirs_visited++;

    if (ctx->opts->on_progress && (r->dirs_visited % 200) == 0)
        ctx->opts->on_progress(dir, r->count, ctx->opts->ud);

    const char *base = sh_path_base(dir);
    if (is_root_name(base)) strlist_add(&r->roots, &r->root_count, &r->root_cap, dir);

    sh_dir *d = sh_dir_open(dir);
    if (!d) {
        r->permissions_denied++;
        return;
    }

    sh_dirent ent;
    while (sh_dir_next(d, &ent) == 1) {
        if (sh_config_is_excluded(cfg, ent.name)) { free(ent.name); free(ent.path); continue; }
        if (sh_config_is_excluded_path(cfg, ent.path)) { free(ent.name); free(ent.path); continue; }

        if (ent.kind == SH_FS_KIND_DIR) {
            walk(ctx, ent.path, depth + 1);
        } else if (ent.kind == SH_FS_KIND_LINK) {
            r->links_skipped++;
            /* A link that points at a skill is recorded as an alias, never followed. */
            char *probe = sh_path_join(ent.path, "SKILL.md");
            sh_fs_info info;
            int is_skill = 0;
            if (probe && sh_lstat(probe, &info) == SH_FS_OK && info.is_file) is_skill = 1;
            free(probe);

            if (is_skill) {
                char *target = sh_readlink(ent.path);
                strlist_add(&r->aliases, &r->alias_count, &r->alias_cap, ent.path);
                free(target);
            } else if (cfg->follow_symlinks) {
                char *real = sh_path_real(ent.path);
                if (real) {
                    if (visited_add(ctx, real)) walk(ctx, ent.path, depth + 1);
                    free(real);
                }
            }
        }
        free(ent.name);
        free(ent.path);
    }
    sh_dir_close(d);
}

static void walk(scan_ctx *ctx, const char *dir, int depth) {
    if (depth > ctx->opts->config->max_depth) return;

    if (has_skill_md(dir)) {
        /* A unit.  Its parent becomes a skill root when it is not one already. */
        record_unit(ctx, dir, 0, NULL);

        char *parent = sh_path_parent(dir);
        if (parent && strcmp(parent, "/") != 0 && strcmp(parent, ".") != 0)
            strlist_add(&ctx->out->roots, &ctx->out->root_count, &ctx->out->root_cap, parent);
        free(parent);
        return; /* do not descend: nested skills belong to the outer skill */
    }

    visit_dir(ctx, dir, depth);
}

/* Deepest skill root that contains path. */
static char *deepest_root(sh_scan_result *r, const char *path) {
    char *best = NULL;
    size_t best_len = 0;

    /* Try every ancestor of path. */
    char *cur = strdup(path);
    while (cur && *cur) {
        char *parent = sh_path_parent(cur);
        if (strlist_has(r->roots, r->root_count, cur)) {
            size_t len = strlen(cur);
            if (len >= best_len) { best = strdup(cur); best_len = len; }
        }
        if (strcmp(cur, "/") == 0 || strcmp(cur, ".") == 0) { free(parent); break; }
        free(cur);
        cur = parent;
    }
    free(cur);
    return best;
}

static int depth_between(const char *root, const char *path) {
    if (!root || !path) return 0;
    size_t rl = strlen(root);
    if (strncmp(path, root, rl) != 0) return 0;
    const char *rest = path + rl;
    int depth = 0;
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        while (*rest && *rest != '/') rest++;
        depth++;
    }
    return depth;
}

int sh_scan_run(const sh_scan_opts *opts, sh_scan_result *out) {
    memset(out, 0, sizeof(*out));

    scan_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.opts = opts;
    ctx.out = out;

    for (size_t i = 0; i < opts->config->scan_root_count; i++) {
        const char *root = opts->config->scan_roots[i];
        if (sh_config_is_excluded_path(opts->config, root)) continue;
        sh_fs_info info;
        if (sh_stat(root, &info) != SH_FS_OK || !info.is_dir) continue;
        char *real = sh_path_real(root);
        if (real) {
            if (visited_add(&ctx, real)) walk(&ctx, root, 0);
            free(real);
        } else {
            walk(&ctx, root, 0);
        }
    }

    /* Attach each unit to its deepest skill root. */
    for (size_t i = 0; i < out->count; i++) {
        sh_unit *u = &out->units[i];
        u->root = deepest_root(out, u->path);
        u->depth = depth_between(u->root, u->path);
    }

    if (opts->compute_hash) {
        for (size_t i = 0; i < out->count; i++) {
            sh_unit *u = &out->units[i];
            if (u->hash) continue;
            char hex[65];
            uint64_t size = 0;
            size_t files = 0;
            if (sh_sha256_tree(u->path, hex, &size, &files) == SH_FS_OK) {
                u->hash = strdup(hex);
                u->size = size;
                u->file_count = files;
            }
            if (opts->on_progress) opts->on_progress(u->path, out->count, opts->ud);
        }
    } else {
        for (size_t i = 0; i < out->count; i++) {
            sh_unit *u = &out->units[i];
            u->size = sh_dir_size(u->path);
        }
    }

    for (size_t i = 0; i < ctx.visited_count; i++) free(ctx.visited[i]);
    free(ctx.visited);
    return 0;
}

sh_unit *sh_scan_find_by_path(sh_scan_result *r, const char *path) {
    for (size_t i = 0; i < r->count; i++)
        if (strcmp(r->units[i].path, path) == 0) return &r->units[i];
    return NULL;
}

void sh_scan_result_free(sh_scan_result *r) {
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) {
        free(r->units[i].path);
        free(r->units[i].basename);
        free(r->units[i].name);
        free(r->units[i].description);
        free(r->units[i].hash);
        free(r->units[i].root);
        free(r->units[i].link_target);
    }
    free(r->units);
    for (size_t i = 0; i < r->root_count; i++) free(r->roots[i]);
    free(r->roots);
    for (size_t i = 0; i < r->alias_count; i++) free(r->aliases[i]);
    free(r->aliases);
    memset(r, 0, sizeof(*r));
}
