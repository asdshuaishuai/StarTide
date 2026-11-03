/*
 * cli_common.c - shared helpers for the skillctl command line.
 */
#include "cli.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int cli_json = 0;
int cli_dry_run = 0;
int cli_verbose = 0;
const char *cli_agent = NULL;
const char *cli_filter = NULL;
long cli_limit = 0;
const char *cli_extra_root = NULL;
const char *cli_scope = NULL;

static sh_config g_config;
static int g_config_loaded = 0;

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) h = getenv("USERPROFILE");
    return h && *h ? h : ".";
}

void cli_load_config(void) {
    if (g_config_loaded) return;
    sh_config_defaults(&g_config, home_dir());

    /* The store itself is never a source of skills. */
    sh_config_add_exclude_path(&g_config, g_config.home);

    char *cfg_path = sh_config_path_in_home(g_config.home, "config.json");
    if (sh_read_file(cfg_path, NULL, NULL) == SH_FS_OK) sh_config_load(&g_config, cfg_path);
    free(cfg_path);

    if (cli_extra_root) {
        char *abs = sh_path_expand(cli_extra_root, g_config.home_dir);
        sh_config_add_scan_root(&g_config, abs ? abs : cli_extra_root);
        free(abs);
    }

    /* Agents that are not in the config are discovered automatically. */
    sh_config_discover_agents(&g_config);

    /* A configured root that does not exist would be skipped silently by
     * the scanner; say so here instead. */
    for (size_t i = 0; i < g_config.scan_root_count; i++) {
        sh_fs_info info;
        if (sh_stat(g_config.scan_roots[i], &info) != SH_FS_OK || !info.is_dir)
            cli_error("%s: scan root does not exist", g_config.scan_roots[i]);
    }

    g_config_loaded = 1;
}

const sh_config *cli_config(void) {
    cli_load_config();
    return &g_config;
}

const char *cli_store_home(void) {
    cli_load_config();
    return g_config.home;
}

const char *cli_home_dir(void) {
    cli_load_config();
    return g_config.home_dir;
}

void cli_note(const char *fmt, ...) {
    if (cli_json) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

void cli_verbose_note(const char *fmt, ...) {
    if (!cli_verbose || cli_json) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

void cli_error(const char *fmt, ...) {
    fprintf(stderr, "skillctl: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void cli_format_bytes(uint64_t bytes, char *out, size_t out_len) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    else snprintf(out, out_len, "%.1f %s", value, units[unit]);
}

/* ------------------------------------------------------------- links */

static int unit_hash_cmp(const void *a, const void *b) {
    const sh_unit *x = *(const sh_unit *const *)a;
    const sh_unit *y = *(const sh_unit *const *)b;
    return strcmp(x->hash, y->hash);
}

static int unit_name_cmp(const void *a, const void *b) {
    const sh_unit *x = *(const sh_unit *const *)a;
    const sh_unit *y = *(const sh_unit *const *)b;
    int c = strcmp(x->name, y->name);
    if (c != 0) return c;
    return strcmp(x->hash, y->hash);
}

/*
 * Pick the primary unit among several that share a name: a unit under a
 * user managed root beats one from a plugin cache, and a newer one beats
 * an older one.
 */
static int better_unit(const sh_unit *cand, const sh_unit *cur) {
    if (!cur) return 1;
    if (cand->kind != cur->kind) return cand->kind == SH_UNIT_MANAGED;
    if (cand->mtime != cur->mtime) return cand->mtime > cur->mtime;
    return strcmp(cand->hash ? cand->hash : "", cur->hash ? cur->hash : "") < 0;
}

int cli_unique_units(const sh_scan_result *scan, const sh_unit ***out_reps, size_t *out_count) {
    *out_reps = NULL;
    *out_count = 0;

    /* Only units with a known identity can be linked. */
    size_t n = 0;
    const sh_unit **idx = calloc(scan->count ? scan->count : 1, sizeof(sh_unit *));
    if (!idx) return 0;
    for (size_t i = 0; i < scan->count; i++)
        if (scan->units[i].hash) idx[n++] = &scan->units[i];
    if (n == 0) { free(idx); return 1; }

    /* Collapse identical content into a single representative. */
    qsort((void *)idx, n, sizeof(sh_unit *), unit_hash_cmp);
    const sh_unit **reps = calloc(n, sizeof(sh_unit *));
    if (!reps) { free(idx); return 0; }
    size_t rep_count = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        const sh_unit *best = idx[i];
        while (j < n && strcmp(idx[j]->hash, best->hash) == 0) {
            if (better_unit(idx[j], best)) best = idx[j];
            j++;
        }
        reps[rep_count++] = best;
        i = j;
    }
    free(idx);

    /* Name order, so that same named skills end up next to each other. */
    qsort((void *)reps, rep_count, sizeof(sh_unit *), unit_name_cmp);

    *out_reps = reps;
    *out_count = rep_count;
    return 1;
}

void cli_units_free(const sh_unit **reps) { free((void *)reps); }

/* ------------------------------------------------ catalog completion */

void cli_catalog_units_free(cli_catalog_units *scratch) {
    if (!scratch) return;
    sh_catalog_free(&scratch->catalog);
    free(scratch->units);
    memset(scratch, 0, sizeof(*scratch));
}

int cli_units_with_catalog(const sh_unit ***reps, size_t *rep_count, const char *catalog_path,
                           cli_catalog_units *scratch) {
    memset(scratch, 0, sizeof(*scratch));
    if (!sh_catalog_load(&scratch->catalog, catalog_path)) return 0;
    if (scratch->catalog.count == 0) return 1;

    for (size_t i = 0; i < scratch->catalog.count; i++) {
        sh_catalog_entry *e = &scratch->catalog.entries[i];
        if (!e->in_store || !e->name || !e->id) continue;
        /*
         * The catalog's stored flag can go stale (gc removes units without
         * a catalog rewrite from older versions); trust the store itself,
         * otherwise a dead unit would be linked into the views.
         */
        if (!sh_store_has_unit(cli_store_home(), e->id)) continue;

        /* The scan already covers this identity. */
        int known = 0;
        for (size_t k = 0; k < *rep_count; k++)
            if (strcmp((*reps)[k]->hash, e->id) == 0) { known = 1; break; }
        if (known) continue;

        if (scratch->count == scratch->cap) {
            size_t cap = scratch->cap ? scratch->cap * 2 : 32;
            sh_unit *n = realloc(scratch->units, cap * sizeof(sh_unit));
            if (!n) return 0;
            scratch->units = n;
            scratch->cap = cap;
        }
        sh_unit *u = &scratch->units[scratch->count++];
        memset(u, 0, sizeof(*u));
        u->name = e->name;
        u->hash = e->id;
        u->kind = SH_UNIT_MANAGED;
        u->mtime = e->mtime;
        u->root = e->source_count > 0 ? e->sources[0] : NULL;

        const sh_unit **grown = realloc(*reps, (*rep_count + 1) * sizeof(sh_unit *));
        if (!grown) return 0;
        *reps = grown;
        (*reps)[(*rep_count)++] = u;
    }

    if (*rep_count > 1) qsort((void *)*reps, *rep_count, sizeof(sh_unit *), unit_name_cmp);
    return 1;
}

static int link_push(cli_link_list *l, const char *name, const char *id) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 128;
        sh_link_entry *n = realloc(l->items, cap * sizeof(sh_link_entry));
        if (!n) return 0;
        l->items = n;
        l->cap = cap;
    }
    l->items[l->count].name = strdup(name);
    l->items[l->count].id = strdup(id);
    l->count++;
    return 1;
}

static int link_cmp(const void *a, const void *b) {
    const sh_link_entry *x = a, *y = b;
    return strcmp(x->name, y->name);
}

cli_link_list cli_links_for_agent(const sh_scan_result *scan, const sh_unit **reps,
                                  size_t rep_count, const sh_agent *agent, int scope_all) {
    cli_link_list out;
    memset(&out, 0, sizeof(out));
    if (!agent || !agent->root) return out;

    /* The agent's home directory is the parent of its skills directory. */
    char *agent_home = sh_path_parent(agent->root);

    /* Which representative units can this agent see? */
    const sh_unit **keep = calloc(rep_count ? rep_count : 1, sizeof(sh_unit *));
    if (!keep) { free(agent_home); return out; }
    size_t keep_count = 0;

    for (size_t i = 0; i < rep_count; i++) {
        const sh_unit *u = reps[i];
        int reachable = 1;
        if (!scope_all) {
            reachable = 0;
            if (u->root) {
                /*
                 * An agent sees the skills inside its own home directory
                 * (~/.zcode, ~/.claude, ...) plus the shared
                 * ~/.agents/skills.  Plugin caches live inside the agent's
                 * home, so they come along.
                 */
                if (sh_path_has_prefix(u->root, agent_home)) reachable = 1;
                for (size_t k = 0; !reachable && k < agent->include_count; k++)
                    if (sh_path_has_prefix(u->root, agent->include[k])) reachable = 1;
            }
        }
        if (reachable) keep[keep_count++] = u;
    }
    free(agent_home);

    /* The kept units are still in name order, so same names are adjacent. */
    for (size_t i = 0; i < keep_count;) {
        size_t j = i;
        const sh_unit *primary = keep[i];
        while (j < keep_count && strcmp(keep[j]->name, keep[i]->name) == 0) {
            if (better_unit(keep[j], primary)) primary = keep[j];
            j++;
        }
        for (size_t k = i; k < j; k++) {
            if (keep[k] == primary) {
                link_push(&out, keep[k]->name, keep[k]->hash);
            } else {
                size_t need = strlen(keep[k]->name) + 16;
                char *suffix = malloc(need);
                snprintf(suffix, need, "%s__%.8s", keep[k]->name, keep[k]->hash);
                link_push(&out, suffix, keep[k]->hash);
                free(suffix);
            }
        }
        i = j;
    }
    free(keep);

    if (out.count > 1) qsort(out.items, out.count, sizeof(sh_link_entry), link_cmp);
    return out;
}

void cli_link_list_free(cli_link_list *l) {
    if (!l) return;
    for (size_t i = 0; i < l->count; i++) {
        free(l->items[i].name);
        free(l->items[i].id);
    }
    free(l->items);
    memset(l, 0, sizeof(*l));
}
