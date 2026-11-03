/*
 * cmd_scan.c - the scan and catalog commands.
 */
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "sh_json.h"

static int stream_mode = 0;

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void on_progress(const char *path, size_t units, void *ud) {    (void)ud;
    if (!stream_mode || !cli_json) return;
    sh_jw w;
    sh_jw_init(&w);
    sh_jw_obj_begin(&w);
    sh_jw_key(&w, "event");
    sh_jw_str(&w, "progress");
    sh_jw_key(&w, "path");
    sh_jw_str(&w, path);
    sh_jw_key(&w, "units");
    sh_jw_num(&w, (long long)units);
    sh_jw_obj_end(&w);
    printf("%s\n", sh_jw_text(&w));
    fflush(stdout);
    sh_jw_free(&w);
}

/* Count how many units live under a given root. */
static size_t units_under(const sh_scan_result *scan, const char *root) {
    size_t n = 0;
    for (size_t i = 0; i < scan->count; i++)
        if (scan->units[i].root && strcmp(scan->units[i].root, root) == 0) n++;
    return n;
}

/* Merge the scan into the catalog on disk. */
static int update_catalog(const sh_scan_result *scan) {
    char *path = sh_store_catalog_path(cli_store_home());
    sh_catalog cat;
    sh_catalog_load(&cat, path);

    for (size_t i = 0; i < scan->count; i++) {
        const sh_unit *u = &scan->units[i];
        if (!u->hash) continue;
        sh_catalog_entry *e = sh_catalog_upsert(&cat, u->hash);
        if (!e) continue;
        if (u->name) { free(e->name); e->name = strdup(u->name); }
        if (u->description) { free(e->description); e->description = strdup(u->description); }
        e->size = u->size;
        e->file_count = u->file_count;
        if (u->mtime > e->mtime) e->mtime = u->mtime;
        if (sh_store_has_unit(cli_store_home(), u->hash)) e->in_store = 1;
        sh_catalog_add_source(e, u->path);
    }

    int ok = sh_catalog_save(&cat, path);
    free(path);
    sh_catalog_free(&cat);
    return ok;
}

int cmd_scan(int argc, char **argv) {
    int compute_hash = 1;
    int write_catalog = 1;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--no-hash") == 0) compute_hash = 0;
        else if (strcmp(argv[i], "--no-catalog") == 0) write_catalog = 0;
        else if (strcmp(argv[i], "--stream") == 0) stream_mode = 1;
    }

    const sh_config *cfg = cli_config();

    cli_note("Scanning %zu root(s)...", cfg->scan_root_count);
    for (size_t i = 0; i < cfg->scan_root_count; i++) cli_note("  %s", cfg->scan_roots[i]);

    sh_scan_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.config = cfg;
    opts.compute_hash = compute_hash;
    opts.on_progress = on_progress;

    sh_scan_result scan;
    sh_scan_run(&opts, &scan);

    /* Unique identities: count the distinct content hashes. */
    size_t unique = 0;
    {
        const char **hashes = calloc(scan.count ? scan.count : 1, sizeof(char *));
        size_t n = 0;
        for (size_t i = 0; i < scan.count; i++)
            if (scan.units[i].hash) hashes[n++] = scan.units[i].hash;
        if (n > 1) qsort((void *)hashes, n, sizeof(char *), str_cmp);
        for (size_t i = 0; i < n; i++)
            if (i == 0 || strcmp(hashes[i], hashes[i - 1]) != 0) unique++;
        free(hashes);
    }

    size_t external = 0;
    for (size_t i = 0; i < scan.count; i++)
        if (scan.units[i].kind == SH_UNIT_EXTERNAL) external++;

    if (write_catalog && compute_hash) update_catalog(&scan);

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "command");
        sh_jw_str(&w, "scan");
        sh_jw_key(&w, "roots");
        sh_jw_arr_begin(&w);
        for (size_t i = 0; i < cfg->scan_root_count; i++) sh_jw_str(&w, cfg->scan_roots[i]);
        sh_jw_arr_end(&w);
        sh_jw_key(&w, "stats");
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "units");
        sh_jw_num(&w, (long long)scan.count);
        sh_jw_key(&w, "unique");
        sh_jw_num(&w, (long long)unique);
        sh_jw_key(&w, "skill_roots");
        sh_jw_num(&w, (long long)scan.root_count);
        sh_jw_key(&w, "aliases");
        sh_jw_num(&w, (long long)scan.alias_count);
        sh_jw_key(&w, "external");
        sh_jw_num(&w, (long long)external);
        sh_jw_key(&w, "dirs_visited");
        sh_jw_num(&w, (long long)scan.dirs_visited);
        sh_jw_obj_end(&w);
        sh_jw_key(&w, "skill_roots_detail");
        sh_jw_arr_begin(&w);
        for (size_t i = 0; i < scan.root_count; i++) {
            sh_jw_obj_begin(&w);
            sh_jw_key(&w, "path");
            sh_jw_str(&w, scan.roots[i]);
            sh_jw_key(&w, "skills");
            sh_jw_num(&w, (long long)units_under(&scan, scan.roots[i]));
            sh_jw_obj_end(&w);
        }
        sh_jw_arr_end(&w);
        sh_jw_key(&w, "units_detail");
        sh_jw_arr_begin(&w);
        for (size_t i = 0; i < scan.count; i++) {
            const sh_unit *u = &scan.units[i];
            sh_jw_obj_begin(&w);
            sh_jw_key(&w, "id");
            sh_jw_str(&w, u->hash ? u->hash : "");
            sh_jw_key(&w, "name");
            sh_jw_str(&w, u->name ? u->name : "");
            sh_jw_key(&w, "description");
            sh_jw_str(&w, u->description ? u->description : "");
            sh_jw_key(&w, "path");
            sh_jw_str(&w, u->path);
            sh_jw_key(&w, "root");
            sh_jw_str(&w, u->root ? u->root : "");
            sh_jw_key(&w, "depth");
            sh_jw_num(&w, u->depth);
            sh_jw_key(&w, "size");
            sh_jw_numf(&w, (double)u->size);
            sh_jw_key(&w, "files");
            sh_jw_num(&w, (long long)u->file_count);
            sh_jw_key(&w, "external");
            sh_jw_bool(&w, u->kind == SH_UNIT_EXTERNAL);
            sh_jw_obj_end(&w);
        }
        sh_jw_arr_end(&w);
        sh_jw_obj_end(&w);
        printf("%s\n", sh_jw_text(&w));
        sh_jw_free(&w);
    } else {
        cli_note("");
        cli_note("  %zu skill directories  (%zu unique by content)", scan.count, unique);
        cli_note("  %zu skill roots, %zu symlinked aliases", scan.root_count, scan.alias_count);
        cli_note("  %zu read-only sources, %zu directories visited", external, scan.dirs_visited);

        cli_note("");
        cli_note("Skill roots:");
        for (size_t i = 0; i < scan.root_count; i++) {
            size_t n = units_under(&scan, scan.roots[i]);
            if (n == 0) continue;
            cli_note("  %-64s %4zu", scan.roots[i], n);
        }

        if (cli_limit == 0 && !cli_json) {
            cli_note("");
            cli_note("Run 'skillctl catalog' for the full list, or 'skillctl link' to unify.");
        }
    }

    sh_scan_result_free(&scan);
    return 0;
}

int cmd_catalog(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const sh_config *cfg = cli_config();
    char *path = sh_store_catalog_path(cli_store_home());
    sh_catalog cat;
    int loaded = sh_catalog_load(&cat, path);
    free(path);
    if (!loaded || cat.count == 0) {
        cli_note("The catalog is empty. Run 'skillctl scan' first.");
        sh_catalog_free(&cat);
        return 0;
    }

    /*
     * Which agents would a skill reach under scope "own"?  Mirrors the
     * reachability rule of cli_links_for_agent: a skill belongs to an
     * agent when any of its sources lives in the agent's home directory
     * (plugin caches included) or in one of the agent's include roots
     * (the shared ~/.agents/skills).  Clients can render scope "all" as
     * "every agent" themselves.
     */
    size_t shown = 0;
    for (size_t i = 0; i < cat.count; i++) {
        const sh_catalog_entry *e = &cat.entries[i];
        if (cli_filter && !strstr(e->name ? e->name : "", cli_filter) &&
            !strstr(e->description ? e->description : "", cli_filter))
            continue;
        if (cli_limit > 0 && shown >= (size_t)cli_limit) break;

        char size[32];
        cli_format_bytes(e->size, size, sizeof(size));
        if (cli_json) {
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
            sh_jw_key(&w, "stored");
            sh_jw_bool(&w, e->in_store);
            sh_jw_key(&w, "agents");
            sh_jw_arr_begin(&w);
            for (size_t a = 0; a < cfg->agent_count; a++) {
                const sh_agent *agent = &cfg->agents[a];
                char *agent_home = sh_path_parent(agent->root);
                int reachable = 0;
                for (size_t s = 0; !reachable && s < e->source_count; s++) {
                    if (sh_path_has_prefix(e->sources[s], agent_home)) reachable = 1;
                    if (sh_path_has_prefix(e->sources[s], agent->root)) reachable = 1;
                    for (size_t k = 0; !reachable && k < agent->include_count; k++)
                        if (sh_path_has_prefix(e->sources[s], agent->include[k])) reachable = 1;
                }
                free(agent_home);
                if (reachable) sh_jw_str(&w, agent->id);
            }
            sh_jw_arr_end(&w);
            sh_jw_key(&w, "sources");
            sh_jw_arr_begin(&w);
            for (size_t s = 0; s < e->source_count; s++) sh_jw_str(&w, e->sources[s]);
            sh_jw_arr_end(&w);
            sh_jw_obj_end(&w);
            printf("%s\n", sh_jw_text(&w));
            sh_jw_free(&w);
        } else {
            printf("%-40s %10s  %-4s  %s\n", e->name ? e->name : "(unnamed)", size,
                   e->in_store ? "yes" : "no", e->description ? e->description : "");
        }
        shown++;
    }

    if (!cli_json) cli_note("\n%zu of %zu skills shown.", shown, cat.count);
    sh_catalog_free(&cat);
    return 0;
}
