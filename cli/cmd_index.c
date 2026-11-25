/*
 * cmd_index.c - the index command.
 *
 * A full rebuild is the only rebuild: ~800 skill bodies index in about a
 * second in C, so an incremental delta segment would not pay for itself.
 * --if-stale skips the work when the catalog has not moved, which is what
 * import/sync call after they finish.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "sh_fs.h"
#include "sh_index.h"
#include "sh_json.h"

int cmd_index(int argc, char **argv) {
    int rebuild = 0;
    int if_stale = 0;
    char *catalog_path;
    char *index_dir;
    sh_catalog cat;
    sh_index_build_stats stats;
    int ok;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rebuild") == 0) rebuild = 1;
        if (strcmp(argv[i], "--if-stale") == 0) if_stale = 1;
    }

    catalog_path = sh_store_catalog_path(cli_store_home());
    index_dir = sh_index_dir(cli_store_home());
    memset(&cat, 0, sizeof(cat));
    memset(&stats, 0, sizeof(stats));

    if (!sh_catalog_load(&cat, catalog_path) || cat.count == 0) {
        if (cli_json) {
            printf("{\"error\":\"catalog_empty\",\"docs\":0}\n");
        } else {
            cli_error("skillctl: the catalog is empty; run 'skillctl scan' first");
        }
        free(catalog_path);
        free(index_dir);
        sh_catalog_free(&cat);
        return 1;
    }

    if (!rebuild && if_stale && !sh_index_is_stale(index_dir, catalog_path, cat.count)) {
        if (cli_json) {
            printf("{\"up_to_date\":true,\"docs\":0}\n");
        } else {
            cli_note("index is up to date (%zu catalog entries)", cat.count);
        }
        free(catalog_path);
        free(index_dir);
        sh_catalog_free(&cat);
        return 0;
    }

    ok = sh_index_build(index_dir, cli_store_home(), catalog_path, &cat, &stats);

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "ok");
        sh_jw_bool(&w, ok);
        sh_jw_key(&w, "docs");
        sh_jw_num(&w, (long long)stats.docs);
        sh_jw_key(&w, "terms");
        sh_jw_num(&w, (long long)stats.terms);
        sh_jw_key(&w, "postings");
        sh_jw_num(&w, (long long)stats.postings);
        sh_jw_key(&w, "merged");
        sh_jw_num(&w, (long long)stats.merged);
        sh_jw_key(&w, "seconds");
        sh_jw_numf(&w, stats.seconds);
        sh_jw_key(&w, "catalog");
        sh_jw_num(&w, (long long)cat.count);
        sh_jw_obj_end(&w);
        fputs(sh_jw_text(&w), stdout);
        fputc('\n', stdout);
        sh_jw_free(&w);
    } else if (ok) {
        cli_note("indexed %zu skills (%zu catalog entries, %zu merged as near-duplicates)",
                 stats.docs, cat.count, stats.merged);
        cli_note("%zu terms, %zu postings, %.2fs", stats.terms, stats.postings, stats.seconds);
    } else {
        cli_error("skillctl: index build failed");
    }

    free(catalog_path);
    free(index_dir);
    sh_catalog_free(&cat);
    return ok ? 0 : 1;
}
