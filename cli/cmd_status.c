/*
 * cmd_status.c - the status and doctor commands.
 */
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "cli_embedder.h"
#include "sh_index.h"
#include "sh_json.h"

static const char *root_state(const char *root, char **out_target) {
    sh_fs_info info;
    if (sh_lstat(root, &info) != SH_FS_OK) return "missing";
    if (info.is_link) {
        char *target = sh_readlink(root);
        if (out_target) *out_target = target; else free(target);
        return "linked";
    }
    if (info.is_dir) return "directory";
    return "other";
}

/* The display name of an agent: whitelist label when there is one. */
static const char *agent_label(const sh_agent *agent) {
    return agent->label && *agent->label ? agent->label : agent->id;
}

/* First existing candidate path of a whitelist entry, absolute, or NULL. */
static char *known_probe_root(const char *paths) {
    const char *home = cli_home_dir();
    const char *p = paths;
    while (*p) {
        const char *sep = strchr(p, ';');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        char *cand = malloc(strlen(home) + len + 2);
        if (!cand) return NULL;
        snprintf(cand, strlen(home) + len + 2, "%s/%.*s", home, (int)len, p);
        sh_fs_info info;
        int ok = sh_lstat(cand, &info) == SH_FS_OK && (info.is_dir || info.is_link);
        if (ok) return cand;
        free(cand);
        if (!sep) break;
        p = sep + 1;
    }
    return NULL;
}

int cmd_status(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const sh_config *cfg = cli_config();
    const char *home = cli_store_home();

    /* Store statistics. */
    char *units = sh_store_units_dir(home);
    size_t unit_count = 0;
    uint64_t store_bytes = 0;
    sh_dir *d = sh_dir_open(units);
    if (d) {
        sh_dirent ent;
        while (sh_dir_next(d, &ent) == 1) {
            if (ent.kind != SH_FS_KIND_DIR) { free(ent.name); free(ent.path); continue; }
            unit_count++;
            store_bytes += sh_dir_size(ent.path);
            free(ent.name);
            free(ent.path);
        }
        sh_dir_close(d);
    }
    free(units);

    char *catalog_path = sh_store_catalog_path(home);
    sh_catalog cat;
    sh_catalog_load(&cat, catalog_path);
    free(catalog_path);

    size_t stored_entries = 0;
    for (size_t i = 0; i < cat.count; i++)
        if (cat.entries[i].in_store) stored_entries++;

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "command");
        sh_jw_str(&w, "status");
        sh_jw_key(&w, "store");
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "home");
        sh_jw_str(&w, home);
        sh_jw_key(&w, "units");
        sh_jw_num(&w, (long long)unit_count);
        sh_jw_key(&w, "bytes");
        sh_jw_numf(&w, (double)store_bytes);
        sh_jw_key(&w, "catalog");
        sh_jw_num(&w, (long long)cat.count);
        sh_jw_key(&w, "catalog_stored");
        sh_jw_num(&w, (long long)stored_entries);
        sh_jw_obj_end(&w);
        sh_jw_key(&w, "agents");
        sh_jw_arr_begin(&w);
        for (size_t a = 0; a < cfg->agent_count; a++) {
            const sh_agent *agent = &cfg->agents[a];
            if (cli_agent && strcmp(cli_agent, agent->id) != 0) continue;
            char *target = NULL;
            const char *state = root_state(agent->root, &target);

            char *view = sh_store_view_dir(home, agent->id);
            size_t links = 0, foreign = 0;
            sh_dir *v = sh_dir_open(view);
            if (v) {
                sh_dirent ent;
                while (sh_dir_next(v, &ent) == 1) {
                    if (ent.kind == SH_FS_KIND_LINK) links++;
                    else if (ent.kind == SH_FS_KIND_DIR) foreign++;
                    free(ent.name);
                    free(ent.path);
                }
                sh_dir_close(v);
            }
            free(view);

            sh_jw_obj_begin(&w);
            sh_jw_key(&w, "id");
            sh_jw_str(&w, agent->id);
            sh_jw_key(&w, "label");
            sh_jw_str(&w, agent_label(agent));
            sh_jw_key(&w, "description");
            sh_jw_str(&w, agent->description ? agent->description : "");
            sh_jw_key(&w, "known");
            sh_jw_bool(&w, agent->known);
            sh_jw_key(&w, "root");
            sh_jw_str(&w, agent->root);
            sh_jw_key(&w, "state");
            sh_jw_str(&w, state);
            sh_jw_key(&w, "target");
            sh_jw_str(&w, target ? target : "");
            sh_jw_key(&w, "links");
            sh_jw_num(&w, (long long)links);
            sh_jw_key(&w, "foreign");
            sh_jw_num(&w, (long long)foreign);
            sh_jw_key(&w, "include");
            sh_jw_arr_begin(&w);
            for (size_t k = 0; k < agent->include_count; k++) sh_jw_str(&w, agent->include[k]);
            sh_jw_arr_end(&w);
            sh_jw_obj_end(&w);
            free(target);
        }
        sh_jw_arr_end(&w);

        /* The full whitelist, for clients that want to show what could be
         * detected beyond the agents installed right now. */
        sh_jw_key(&w, "known");
        sh_jw_arr_begin(&w);
        size_t known_count = 0;
        const sh_known_agent *known = sh_config_known_list(&known_count);
        for (size_t i = 0; i < known_count; i++) {
            char *root = known_probe_root(known[i].paths);
            sh_jw_obj_begin(&w);
            sh_jw_key(&w, "id");
            sh_jw_str(&w, known[i].id);
            sh_jw_key(&w, "label");
            sh_jw_str(&w, known[i].label);
            sh_jw_key(&w, "description");
            sh_jw_str(&w, known[i].description);
            sh_jw_key(&w, "installed");
            sh_jw_bool(&w, root != NULL);
            sh_jw_key(&w, "root");
            sh_jw_str(&w, root ? root : "");
            sh_jw_obj_end(&w);
            free(root);
        }
        sh_jw_arr_end(&w);
        sh_jw_obj_end(&w);
        printf("%s\n", sh_jw_text(&w));
        sh_jw_free(&w);
    } else {
        char size[32];
        cli_format_bytes(store_bytes, size, sizeof(size));
        cli_note("Store   %s", home);
        cli_note("  %zu units, %s, catalog holds %zu entries (%zu stored)", unit_count, size,
                 cat.count, stored_entries);
        cli_note("");
        cli_note("Agents");
        for (size_t a = 0; a < cfg->agent_count; a++) {
            const sh_agent *agent = &cfg->agents[a];
            if (cli_agent && strcmp(cli_agent, agent->id) != 0) continue;
            char *target = NULL;
            const char *state = root_state(agent->root, &target);

            char *view = sh_store_view_dir(home, agent->id);
            size_t links = 0, foreign = 0;
            sh_dir *v = sh_dir_open(view);
            if (v) {
                sh_dirent ent;
                while (sh_dir_next(v, &ent) == 1) {
                    if (ent.kind == SH_FS_KIND_LINK) links++;
                    else if (ent.kind == SH_FS_KIND_DIR) foreign++;
                    free(ent.name);
                    free(ent.path);
                }
                sh_dir_close(v);
            }
            free(view);

            cli_note("  %-16s %-9s %-52s %4zu links%s%s", agent_label(agent), state, agent->root,
                     links, foreign ? "  (foreign dirs present)" : "",
                     agent->known ? "" : "  (discovered)");
            free(target);
        }

        /* Whitelist entries whose skills directory is not installed. */
        size_t known_count = 0;
        const sh_known_agent *known = sh_config_known_list(&known_count);
        size_t absent = 0;
        for (size_t i = 0; i < known_count; i++) {
            char *root = known_probe_root(known[i].paths);
            if (root) { free(root); continue; }
            if (absent == 0) cli_note("");
            cli_note("  %-16s %-9s %s", known[i].label, "not inst.", known[i].description);
            absent++;
        }
        if (absent > 0) cli_note("  (%zu whitelist agents detected as not installed)", absent);
    }

    sh_catalog_free(&cat);
    return 0;
}

int cmd_doctor(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const sh_config *cfg = cli_config();
    const char *home = cli_store_home();

    int problems = 0;

    /* The store directory must be usable. */
    sh_fs_status st = sh_mkdir_p(home);
    if (st != SH_FS_OK) {
        cli_error("the store directory %s is not writable (%s)", home, sh_fs_strerror(st));
        problems++;
    }

    /* Symlink support. */
    char *probe_dir = sh_path_join(home, ".probe");
    char *probe = sh_path_join(probe_dir, "link");
    sh_mkdir_p(probe_dir);
    int symlinks_ok = sh_symlink("target", probe, 0) == SH_FS_OK;
    if (symlinks_ok) {
        char *back = sh_readlink(probe);
        if (!back || strcmp(back, "target") != 0) symlinks_ok = 0;
        free(back);
    }
    sh_remove_tree(probe_dir);
    free(probe_dir);
    free(probe);
    if (!symlinks_ok) {
        cli_error("this filesystem does not support symlinks, skillctl cannot work here");
        problems++;
    }

    /* Agents that were discovered. */
    size_t with_root = 0;
    for (size_t a = 0; a < cfg->agent_count; a++) {
        sh_fs_info info;
        if (sh_lstat(cfg->agents[a].root, &info) == SH_FS_OK) with_root++;
    }

    /* ---- vector path readiness ------------------------------------- */
    char vec_backend[32];
    char vec_sig[17];
    long long vec_count = 0;
    long long vec_dim = 0;
    int vec_present = 0;
    int vec_match = 0;
    {
        cli_embedder emb;
        sh_vectors vecs;
        char *vpath = sh_path_join(home, "index/vectors.bin");
        int have_emb = cli_embedder_open(&emb, home);
        int have_vecs = sh_vectors_load(vpath, &vecs) && vecs.count > 0;

        snprintf(vec_backend, sizeof(vec_backend), "%s",
                 have_emb ? cli_embedder_backend(&emb) : "none");
        snprintf(vec_sig, sizeof(vec_sig), "%s", have_emb ? cli_embedder_sig(&emb) : "");
        if (have_vecs) {
            vec_present = 1;
            vec_count = (long long)vecs.count;
            vec_dim = (long long)vecs.dim;
            if (have_emb && vecs.sig[0]) vec_match = strcmp(vecs.sig, vec_sig) == 0;
        }
        /* Released here: only plain scalars travel into the report. */
        free(vpath);
        sh_vectors_free(&vecs);
        cli_embedder_close(&emb);
    }

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "command");
        sh_jw_str(&w, "doctor");
        sh_jw_key(&w, "store");
        sh_jw_str(&w, home);
        sh_jw_key(&w, "symlinks");
        sh_jw_bool(&w, symlinks_ok);
        sh_jw_key(&w, "agents_detected");
        sh_jw_num(&w, (long long)cfg->agent_count);
        sh_jw_key(&w, "agents_with_root");
        sh_jw_num(&w, (long long)with_root);
        sh_jw_key(&w, "vector_backend");
        sh_jw_str(&w, vec_backend);
        sh_jw_key(&w, "vector_model_sig");
        sh_jw_str(&w, vec_sig);
        sh_jw_key(&w, "vectors_count");
        sh_jw_num(&w, vec_count);
        sh_jw_key(&w, "vectors_dim");
        sh_jw_num(&w, vec_dim);
        sh_jw_key(&w, "vectors_match_encoder");
        sh_jw_bool(&w, vec_match);
        sh_jw_key(&w, "problems");
        sh_jw_num(&w, (long long)problems);
        sh_jw_key(&w, "agent_list");
        sh_jw_arr_begin(&w);
        for (size_t a = 0; a < cfg->agent_count; a++) {
            if (cli_agent && strcmp(cli_agent, cfg->agents[a].id) != 0) continue;
            sh_jw_obj_begin(&w);
            sh_jw_key(&w, "id");
            sh_jw_str(&w, cfg->agents[a].id);
            sh_jw_key(&w, "label");
            sh_jw_str(&w, agent_label(&cfg->agents[a]));
            sh_jw_key(&w, "root");
            sh_jw_str(&w, cfg->agents[a].root);
            sh_jw_obj_end(&w);
        }
        sh_jw_arr_end(&w);
        sh_jw_obj_end(&w);
        printf("%s\n", sh_jw_text(&w));
        sh_jw_free(&w);
    } else {
        cli_note("Store          %s%s", home, st == SH_FS_OK ? "" : "  (not writable)");
        cli_note("Symlinks       %s", symlinks_ok ? "supported" : "NOT SUPPORTED");
        cli_note("Agents         %zu detected, %zu with an existing skills directory",
                 cfg->agent_count, with_root);
        if (strcmp(vec_backend, "none") == 0) {
            cli_note("Vector path    unavailable (no model found; find stays lexical)");
        } else {
            cli_note("Vector path    %s (model %.16s)", vec_backend, vec_sig);
        }
        if (!vec_present) {
            cli_note("Vectors        absent (run 'skillctl embed' to enable --mode hybrid)");
        } else if (vec_match) {
            cli_note("Vectors        %lld x %lld, match this model", vec_count, vec_dim);
        } else {
            cli_note("Vectors        %lld x %lld, built by a DIFFERENT model "
                     "(run 'skillctl embed')",
                     vec_count, vec_dim);
        }
        for (size_t a = 0; a < cfg->agent_count; a++) {
            if (cli_agent && strcmp(cli_agent, cfg->agents[a].id) != 0) continue;
            sh_fs_info info;
            const char *state =
                sh_lstat(cfg->agents[a].root, &info) == SH_FS_OK ? "present" : "missing";
            cli_note("  %-16s %-8s %s", agent_label(&cfg->agents[a]), state, cfg->agents[a].root);
        }
        cli_note("");
        cli_note("%s", problems ? "There are problems, see above." : "Everything looks fine.");
    }

    return problems ? 1 : 0;
}
