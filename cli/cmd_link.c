/*
 * cmd_link.c - the link, unlink and sync commands.
 */
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "sh_json.h"

/* Resolve the effective scope for this run. */
static int scope_is_all(const sh_config *cfg) {
    if (cli_scope) return strcmp(cli_scope, "all") == 0;
    return cfg->view_scope_all;
}

/* Scan, then make sure every unit is in the store. */
static int scan_and_import(sh_scan_result *scan, size_t *imported, size_t *present) {
    const sh_config *cfg = cli_config();
    const char *home = cli_store_home();

    if (!cli_dry_run) sh_store_init(home);

    sh_scan_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.config = cfg;
    opts.compute_hash = 1;
    sh_scan_run(&opts, scan);

    char *catalog_path = sh_store_catalog_path(home);
    sh_catalog cat;
    sh_catalog_load(&cat, catalog_path);

    for (size_t i = 0; i < scan->count; i++) {
        const sh_unit *u = &scan->units[i];
        if (!u->hash) continue;

        sh_catalog_entry *e = sh_catalog_upsert(&cat, u->hash);
        if (e) {
            if (u->name) { free(e->name); e->name = strdup(u->name); }
            if (u->description) { free(e->description); e->description = strdup(u->description); }
            e->size = u->size;
            e->file_count = u->file_count;
            if (u->mtime > e->mtime) e->mtime = u->mtime;
            sh_catalog_add_source(e, u->path);
        }

        if (sh_store_has_unit(home, u->hash)) {
            (*present)++;
            if (e) e->in_store = 1;
            continue;
        }
        if (cli_dry_run) { (*imported)++; continue; }
        char *stored = NULL;
        if (sh_store_import(home, u->path, u->hash, &stored) == SH_FS_OK) {
            (*imported)++;
            if (e) e->in_store = 1;
        }
        free(stored);
    }

    if (!cli_dry_run) sh_catalog_save(&cat, catalog_path);
    free(catalog_path);
    sh_catalog_free(&cat);
    return 0;
}

int cmd_link(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const sh_config *cfg = cli_config();
    const char *home = cli_store_home();

    cli_note("Scanning and importing into %s ...", home);
    sh_scan_result scan;
    size_t imported = 0, present = 0;
    scan_and_import(&scan, &imported, &present);
    cli_note("%zu imported, %zu already present.", imported, present);

    const sh_unit **reps = NULL;
    size_t rep_count = 0;
    cli_unique_units(&scan, &reps, &rep_count);

    /*
     * The walk skips the symlinked roots, so skills adopted on an earlier
     * run are invisible to it.  The catalog knows them; add them back so
     * that rebuilding a view never drops what the store already holds.
     */
    cli_catalog_units scratch;
    char *catalog_path = sh_store_catalog_path(home);
    cli_units_with_catalog(&reps, &rep_count, catalog_path, &scratch);
    free(catalog_path);
    cli_note("%zu unique skills ready to link.", rep_count);

    sh_jw w;
    sh_jw_init(&w);
    sh_jw_obj_begin(&w);
    sh_jw_key(&w, "command");
    sh_jw_str(&w, "link");
    sh_jw_key(&w, "dry_run");
    sh_jw_bool(&w, cli_dry_run);
    sh_jw_key(&w, "imported");
    sh_jw_num(&w, (long long)imported);
    sh_jw_key(&w, "skills");
    sh_jw_num(&w, (long long)rep_count);
    sh_jw_key(&w, "agents");
    sh_jw_arr_begin(&w);

    int failures = 0;

    for (size_t a = 0; a < cfg->agent_count; a++) {
        const sh_agent *agent = &cfg->agents[a];
        if (cli_agent && strcmp(cli_agent, agent->id) != 0) continue;

        sh_fs_info info;
        if (sh_lstat(agent->root, &info) != SH_FS_OK) {
            cli_verbose_note("  %-12s root does not exist, skipping", agent->id);
            continue;
        }

        cli_link_list links =
            cli_links_for_agent(&scan, reps, rep_count, agent, scope_is_all(cfg));

        char *view = sh_store_view_dir(home, agent->id);
        char *trash = NULL;

        sh_fs_status st =
            sh_view_sync(home, agent->id, links.items, links.count, 1, cli_dry_run, &trash);
        if (st != SH_FS_OK) {
            cli_error("%s: could not build the view (%s)", agent->id, sh_fs_strerror(st));
            failures++;
            free(view);
            free(trash);
            cli_link_list_free(&links);
            continue;
        }

        char *backup = NULL;
        char *existing = sh_readlink(agent->root);
        const char *action = "linked";
        if (existing) {
            char *real_root = sh_path_real(agent->root);
            char *real_view = sh_path_real(view);
            int same = real_root && real_view && strcmp(real_root, real_view) == 0;
            free(real_root);
            free(real_view);
            action = same ? "already-linked" : "repointed";
            free(existing);
        }

        st = sh_link_root(agent->root, view, home, agent->id, cli_dry_run, &backup);
        if (st != SH_FS_OK && st != SH_FS_EXISTS) {
            cli_error("%s: could not replace the skills directory (%s)", agent->id,
                      sh_fs_strerror(st));
            failures++;
        } else if (st == SH_FS_EXISTS) {
            cli_error("%s: %s already points somewhere else, leaving it alone", agent->id,
                      agent->root);
            failures++;
        }

        cli_note("  %-12s %s  %zu links  ->  %s", agent->id, action, links.count, agent->root);

        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "id");
        sh_jw_str(&w, agent->id);
        sh_jw_key(&w, "root");
        sh_jw_str(&w, agent->root);
        sh_jw_key(&w, "view");
        sh_jw_str(&w, view);
        sh_jw_key(&w, "action");
        sh_jw_str(&w, action);
        sh_jw_key(&w, "links");
        sh_jw_num(&w, (long long)links.count);
        sh_jw_key(&w, "backup");
        sh_jw_str(&w, backup ? backup : "");
        sh_jw_obj_end(&w);

        free(backup);
        free(view);
        free(trash);
        cli_link_list_free(&links);
    }

    sh_jw_arr_end(&w);
    sh_jw_obj_end(&w);
    if (cli_json) {
        printf("%s\n", sh_jw_text(&w));
    }
    sh_jw_free(&w);

    cli_catalog_units_free(&scratch);
    cli_units_free(reps);
    sh_scan_result_free(&scan);
    return failures ? 1 : 0;
}

int cmd_unlink(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const sh_config *cfg = cli_config();
    const char *home = cli_store_home();

    int restored = 0;
    int failures = 0;
    for (size_t a = 0; a < cfg->agent_count; a++) {
        const sh_agent *agent = &cfg->agents[a];
        if (cli_agent && strcmp(cli_agent, agent->id) != 0) continue;

        char *existing = sh_readlink(agent->root);
        if (!existing) {
            cli_verbose_note("  %-12s not a symlink, nothing to restore", agent->id);
            continue;
        }
        free(existing);

        if (cli_dry_run) {
            cli_note("  %-12s would restore %s", agent->id, agent->root);
            restored++;
            continue;
        }

        sh_fs_status st = sh_unlink_root(agent->root, home, agent->id);
        if (st == SH_FS_OK) {
            cli_note("  %-12s restored %s", agent->id, agent->root);
            restored++;
            /* Drop the view; the store keeps the content. */
            char *view = sh_store_view_dir(home, agent->id);
            sh_remove_tree(view);
            free(view);
        } else {
            /*
             * The agent root still points at the view, so the view is the
             * only way in for this agent; leave it in place.
             */
            cli_error("%s: could not restore the original directory (%s)", agent->id,
                      sh_fs_strerror(st));
            failures++;
        }
    }

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "command");
        sh_jw_str(&w, "unlink");
        sh_jw_key(&w, "restored");
        sh_jw_num(&w, (long long)restored);
        sh_jw_key(&w, "failed");
        sh_jw_num(&w, (long long)failures);
        sh_jw_obj_end(&w);
        printf("%s\n", sh_jw_text(&w));
        sh_jw_free(&w);
    }
    return failures ? 1 : 0;
}

int cmd_sync(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const sh_config *cfg = cli_config();
    const char *home = cli_store_home();

    cli_note("Scanning and importing...");
    sh_scan_result scan;
    size_t imported = 0, present = 0;
    scan_and_import(&scan, &imported, &present);
    cli_note("%zu imported, %zu already present.", imported, present);

    const sh_unit **reps = NULL;
    size_t rep_count = 0;
    cli_unique_units(&scan, &reps, &rep_count);

    cli_catalog_units scratch;
    char *catalog_path = sh_store_catalog_path(home);
    cli_units_with_catalog(&reps, &rep_count, catalog_path, &scratch);
    free(catalog_path);

    int failures = 0;
    for (size_t a = 0; a < cfg->agent_count; a++) {
        const sh_agent *agent = &cfg->agents[a];
        if (cli_agent && strcmp(cli_agent, agent->id) != 0) continue;

        char *existing = sh_readlink(agent->root);
        if (!existing) {
            cli_verbose_note("  %-12s is not linked, skipping", agent->id);
            continue;
        }
        free(existing);

        cli_link_list links =
            cli_links_for_agent(&scan, reps, rep_count, agent, scope_is_all(cfg));
        char *trash = NULL;
        sh_fs_status st =
            sh_view_sync(home, agent->id, links.items, links.count, 1, cli_dry_run, &trash);
        if (st != SH_FS_OK) {
            cli_error("%s: could not bring the view in sync (%s)", agent->id, sh_fs_strerror(st));
            failures++;
        } else {
            cli_note("  %-12s %zu links in sync", agent->id, links.count);
        }
        free(trash);
        cli_link_list_free(&links);
    }

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "command");
        sh_jw_str(&w, "sync");
        sh_jw_key(&w, "imported");
        sh_jw_num(&w, (long long)imported);
        sh_jw_key(&w, "skills");
        sh_jw_num(&w, (long long)rep_count);
        sh_jw_key(&w, "failures");
        sh_jw_num(&w, (long long)failures);
        sh_jw_obj_end(&w);
        printf("%s\n", sh_jw_text(&w));
        sh_jw_free(&w);
    }

    cli_catalog_units_free(&scratch);
    cli_units_free(reps);
    sh_scan_result_free(&scan);
    return failures ? 1 : 0;
}
