/*
 * cmd_store.c - the import and gc commands.
 */
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "sh_json.h"

int cmd_import(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const sh_config *cfg = cli_config();
    const char *home = cli_store_home();

    if (!cli_dry_run) sh_store_init(home);

    sh_scan_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.config = cfg;
    opts.compute_hash = 1;

    cli_note("Scanning for skills...");
    sh_scan_result scan;
    sh_scan_run(&opts, &scan);

    /* One import per unique identity. */
    size_t imported = 0, present = 0, skipped = 0;
    uint64_t bytes = 0;
    char *catalog_path = sh_store_catalog_path(home);
    sh_catalog cat;
    sh_catalog_load(&cat, catalog_path);

    for (size_t i = 0; i < scan.count; i++) {
        const sh_unit *u = &scan.units[i];
        if (!u->hash) { skipped++; continue; }

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
            present++;
            if (e) e->in_store = 1;
            continue;
        }
        if (cli_dry_run) { imported++; bytes += u->size; continue; }

        char *stored = NULL;
        if (sh_store_import(home, u->path, u->hash, &stored) == SH_FS_OK) {
            imported++;
            bytes += u->size;
            if (e) e->in_store = 1;
        } else {
            skipped++;
        }
        free(stored);
    }

    /* Self-heal: mark every catalog entry whose content is in the store,
     * including entries this scan could not see. */
    for (size_t i = 0; i < cat.count; i++) {
        sh_catalog_entry *e = &cat.entries[i];
        if (!e->in_store && sh_store_has_unit(home, e->id)) e->in_store = 1;
    }

    if (!cli_dry_run) sh_catalog_save(&cat, catalog_path);
    free(catalog_path);
    sh_catalog_free(&cat);

    char size[32];
    cli_format_bytes(bytes, size, sizeof(size));

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "command");
        sh_jw_str(&w, "import");
        sh_jw_key(&w, "dry_run");
        sh_jw_bool(&w, cli_dry_run);
        sh_jw_key(&w, "imported");
        sh_jw_num(&w, (long long)imported);
        sh_jw_key(&w, "already_present");
        sh_jw_num(&w, (long long)present);
        sh_jw_key(&w, "skipped");
        sh_jw_num(&w, (long long)skipped);
        sh_jw_key(&w, "bytes");
        sh_jw_numf(&w, (double)bytes);
        sh_jw_obj_end(&w);
        printf("%s\n", sh_jw_text(&w));
        sh_jw_free(&w);
    } else {
        cli_note("");
        cli_note("%s %zu skills (%zu already in the store, %zu skipped), %s",
                 cli_dry_run ? "Would import" : "Imported", imported, present, skipped, size);
        cli_note("Store: %s", home);
    }

    sh_scan_result_free(&scan);
    return 0;
}

int cmd_gc(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const char *home = cli_store_home();

    /* Collect the ids that are still referenced by a view. */
    char **referenced = NULL;
    size_t referenced_count = 0, referenced_cap = 0;

    char *views = sh_store_views_dir(home);
    sh_dir *d = sh_dir_open(views);
    if (d) {
        sh_dirent agent;
        while (sh_dir_next(d, &agent) == 1) {
            if (agent.kind != SH_FS_KIND_DIR) { free(agent.name); free(agent.path); continue; }
            sh_dir *v = sh_dir_open(agent.path);
            if (v) {
                sh_dirent link;
                while (sh_dir_next(v, &link) == 1) {
                    char *target = sh_readlink(link.path);
                    if (target) {
                        const char *base = sh_path_base(target);
                        if (referenced_count == referenced_cap) {
                            referenced_cap = referenced_cap ? referenced_cap * 2 : 64;
                            referenced = realloc(referenced, referenced_cap * sizeof(char *));
                        }
                        referenced[referenced_count++] = strdup(base);
                        free(target);
                    }
                    free(link.name);
                    free(link.path);
                }
                sh_dir_close(v);
            }
            free(agent.name);
            free(agent.path);
        }
        sh_dir_close(d);
    }
    free(views);

    /* Units in the store that nothing points at. */
    char *units = sh_store_units_dir(home);
    sh_dir *ud = sh_dir_open(units);
    size_t removed = 0;
    uint64_t freed = 0;
    char **orphans = NULL;
    size_t orphan_count = 0, orphan_cap = 0;

    if (ud) {
        sh_dirent ent;
        while (sh_dir_next(ud, &ent) == 1) {
            if (ent.kind != SH_FS_KIND_DIR) { free(ent.name); free(ent.path); continue; }
            int used = 0;
            for (size_t i = 0; i < referenced_count; i++)
                if (strcmp(referenced[i], ent.name) == 0) { used = 1; break; }
            if (!used) {
                uint64_t size = sh_dir_size(ent.path);
                freed += size;
                removed++;
                if (orphan_count == orphan_cap) {
                    orphan_cap = orphan_cap ? orphan_cap * 2 : 32;
                    orphans = realloc(orphans, orphan_cap * sizeof(char *));
                }
                orphans[orphan_count++] = strdup(ent.name);
                if (!cli_dry_run) sh_store_remove_unit(home, ent.name);
            }
            free(ent.name);
            free(ent.path);
        }
        sh_dir_close(ud);
    }
    free(units);

    /* Keep the catalog honest: entries whose unit was just removed must
     * not keep claiming stored. */
    if (removed > 0) {
        char *catalog_path = sh_store_catalog_path(home);
        sh_catalog cat;
        sh_catalog_load(&cat, catalog_path);
        for (size_t i = 0; i < orphan_count; i++) {
            sh_catalog_entry *e = sh_catalog_find(&cat, orphans[i]);
            if (e) e->in_store = 0;
        }
        if (!cli_dry_run) sh_catalog_save(&cat, catalog_path);
        free(catalog_path);
        sh_catalog_free(&cat);
    }

    char size[32];
    cli_format_bytes(freed, size, sizeof(size));

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "command");
        sh_jw_str(&w, "gc");
        sh_jw_key(&w, "dry_run");
        sh_jw_bool(&w, cli_dry_run);
        sh_jw_key(&w, "removed");
        sh_jw_num(&w, (long long)removed);
        sh_jw_key(&w, "bytes");
        sh_jw_numf(&w, (double)freed);
        sh_jw_key(&w, "units");
        sh_jw_arr_begin(&w);
        for (size_t i = 0; i < orphan_count; i++) sh_jw_str(&w, orphans[i]);
        sh_jw_arr_end(&w);
        sh_jw_obj_end(&w);
        printf("%s\n", sh_jw_text(&w));
        sh_jw_free(&w);
    } else {
        cli_note("%s %zu unreferenced store unit(s), %s", cli_dry_run ? "Would remove" : "Removed",
                 removed, size);
    }

    for (size_t i = 0; i < orphan_count; i++) free(orphans[i]);
    free(orphans);
    for (size_t i = 0; i < referenced_count; i++) free(referenced[i]);
    free(referenced);
    return 0;
}
