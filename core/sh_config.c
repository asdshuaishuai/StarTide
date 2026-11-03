/*
 * sh_config.c - skillctl configuration handling.
 */
#include "sh_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_json.h"

static const char *DEFAULT_EXCLUDES[] = {
    "node_modules", "__pycache__", ".git",   ".svn",    ".hg",     "target",
    "dist",        "build",        "out",    "vendor",  "coverage", ".venv",
    "venv",        ".cache",       ".next",  ".nuxt",   ".turbo",  ".gradle",
    ".npm",        ".pnpm-store",  ".yarn",  ".cargo",  ".rustup", ".m2",
    ".Trash",      ".docker",      ".vscode", ".idea",  ".android", ".sdkman",
    ".nvm",        ".pyenv",       ".rbenv", ".gem",    ".nuget",  ".conda",
    ".julia",      ".stack",       "Pods",   "DerivedData", "Caches", "Containers",
    "Group Containers", "Mobile Documents", "CloudStorage", "Developer", "Safari",
    "Messages",    "Mail",         "Photos", "Screen Time", NULL};

static const char *DEFAULT_EXTERNAL_MARKERS[] = {"plugins/cache", "bundled-skills",
                                                 "/node_modules/", NULL};

/*
 * Path fragments that hold vendored copies of other people's skills:
 * package caches, virtual environments and SDK checkouts.  They are not
 * part of what the user owns, so they never enter the store.
 */
static const char *DEFAULT_EXCLUDE_PATHS[] = {
    "/pkg/mod/",       "/site-packages/", "/.pub-cache/",   "/.flutter_sdk/",
    "/worktrees/",     "/.venv/",         "/.gradle/caches/", "/node_modules/",
    "/.cargo/registry/", "/.npm/",        "/.m2/repository/", "/.codex/.tmp/",
    "/plugins/marketplaces/", "/connectors-marketplace/", "/vendor_imports/",
    NULL};

/*
 * Directories shaped like an agent root that are actually a store owned by
 * another application.  Replacing them would break that application.
 */
static const char *DEFAULT_EXCLUDE_AGENTS[] = {"cc-switch", NULL};

static char *dup_str(const char *s) { return s ? strdup(s) : NULL; }

char *sh_config_default_home(void) {
    const char *env = getenv("SKILLCTL_HOME");
    if (env && *env) return strdup(env);

    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");
    if (!home || !*home) return strdup(".skillctl");
    return sh_path_join(home, ".skillctl");
}

char *sh_config_path_in_home(const char *home, const char *name) {
    return sh_path_join(home, name);
}

void sh_config_defaults(sh_config *cfg, const char *home_dir) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->home_dir = dup_str(home_dir);
    cfg->home = sh_config_default_home();
    cfg->max_depth = 16;
    cfg->follow_symlinks = 0;
    cfg->hash_during_scan = 1;
    cfg->view_scope_all = 0;

    for (int i = 0; DEFAULT_EXCLUDES[i]; i++) {
        cfg->exclude = realloc(cfg->exclude, (cfg->exclude_count + 1) * sizeof(char *));
        cfg->exclude[cfg->exclude_count++] = strdup(DEFAULT_EXCLUDES[i]);
    }
    for (int i = 0; DEFAULT_EXTERNAL_MARKERS[i]; i++) {
        cfg->external_markers =
            realloc(cfg->external_markers, (cfg->external_marker_count + 1) * sizeof(char *));
        cfg->external_markers[cfg->external_marker_count++] = strdup(DEFAULT_EXTERNAL_MARKERS[i]);
    }

    for (int i = 0; DEFAULT_EXCLUDE_PATHS[i]; i++) {
        cfg->exclude_paths = realloc(cfg->exclude_paths, (cfg->exclude_path_count + 1) * sizeof(char *));
        cfg->exclude_paths[cfg->exclude_path_count++] = strdup(DEFAULT_EXCLUDE_PATHS[i]);
    }
    for (int i = 0; DEFAULT_EXCLUDE_AGENTS[i]; i++) {
        cfg->exclude_agents =
            realloc(cfg->exclude_agents, (cfg->exclude_agent_count + 1) * sizeof(char *));
        cfg->exclude_agents[cfg->exclude_agent_count++] = strdup(DEFAULT_EXCLUDE_AGENTS[i]);
    }

    /* By default the user's home directory is the only thing we scan. */
    sh_config_add_scan_root(cfg, home_dir);
}

int sh_config_add_scan_root(sh_config *cfg, const char *path) {
    if (!path || !*path) return 0;
    /* A leading "~" is expanded against the user's home, never treated as
     * a literal relative directory. */
    char *expanded = sh_path_expand(path, cfg->home_dir);
    if (!expanded) return 0;
    char *abs = sh_path_is_abs(expanded) ? expanded : sh_path_join(cfg->home_dir, expanded);
    if (abs != expanded) free(expanded);
    for (size_t i = 0; i < cfg->scan_root_count; i++) {
        if (strcmp(cfg->scan_roots[i], abs) == 0) { free(abs); return 0; }
    }
    cfg->scan_roots = realloc(cfg->scan_roots, (cfg->scan_root_count + 1) * sizeof(char *));
    cfg->scan_roots[cfg->scan_root_count++] = abs;
    return 1;
}

int sh_config_add_exclude_path(sh_config *cfg, const char *path) {
    if (!path || !*path) return 0;
    for (size_t i = 0; i < cfg->exclude_path_count; i++)
        if (strcmp(cfg->exclude_paths[i], path) == 0) return 0;
    cfg->exclude_paths =
        realloc(cfg->exclude_paths, (cfg->exclude_path_count + 1) * sizeof(char *));
    if (!cfg->exclude_paths) { cfg->exclude_path_count = 0; return 0; }
    cfg->exclude_paths[cfg->exclude_path_count++] = strdup(path);
    return 1;
}

int sh_config_add_agent(sh_config *cfg, const char *id, const char *root, const char *label) {
    return sh_config_add_agent_ex(cfg, id, root, label, NULL, 0);
}

int sh_config_add_agent_ex(sh_config *cfg, const char *id, const char *root, const char *label,
                           const char *description, int known) {
    if (!id || !root) return 0;
    char *expanded = sh_path_expand(root, cfg->home_dir);
    if (!expanded) return 0;
    char *abs = sh_path_is_abs(expanded) ? expanded : sh_path_join(cfg->home_dir, expanded);
    if (abs != expanded) free(expanded);
    for (size_t i = 0; i < cfg->agent_count; i++) {
        if (strcmp(cfg->agents[i].id, id) == 0) {
            free(cfg->agents[i].root);
            free(cfg->agents[i].label);
            free(cfg->agents[i].description);
            cfg->agents[i].root = abs;
            cfg->agents[i].label = dup_str(label ? label : id);
            cfg->agents[i].description = dup_str(description);
            cfg->agents[i].known = known;
            return 1;
        }
    }
    cfg->agents = realloc(cfg->agents, (cfg->agent_count + 1) * sizeof(sh_agent));
    if (!cfg->agents) { free(abs); return 0; }
    /* realloc does not zero the new slot; leaving it uninitialised would
     * hand out a garbage include pointer and count. */
    memset(&cfg->agents[cfg->agent_count], 0, sizeof(sh_agent));
    cfg->agents[cfg->agent_count].id = strdup(id);
    cfg->agents[cfg->agent_count].root = abs;
    cfg->agents[cfg->agent_count].label = dup_str(label ? label : id);
    cfg->agents[cfg->agent_count].description = dup_str(description);
    cfg->agents[cfg->agent_count].known = known;
    cfg->agent_count++;
    return 1;
}

/* ----------------------------------------------------- known agents */

/*
 * Agents with an Agent Client Protocol presence and a conventional
 * skills directory under the home.  Detection probes these first so the
 * well known agents get proper names and descriptions; directories that
 * match none of these are still discovered generically.
 */
static const sh_known_agent KNOWN_AGENTS[] = {
    {"zcode", "ZCode", "ZCode CLI", ".zcode/skills"},
    {"claude", "Claude Code", "Anthropic 的编码 agent", ".claude/skills"},
    {"codex", "Codex CLI", "OpenAI 的编码 CLI", ".codex/skills"},
    {"cursor", "Cursor", "Cursor 编辑器 CLI", ".cursor/skills"},
    {"gemini", "Gemini CLI", "Google 的 Gemini CLI", ".gemini/skills"},
    {"opencode", "OpenCode", "sst 的开源编码 agent", ".opencode/skills"},
    {"qwen", "Qwen Code", "阿里巴巴的 Qwen 编码 CLI", ".qwen/skills"},
    {"qoder", "Qoder CLI", "Qoder 的 CLI", ".qoder/skills"},
    {"goose", "Goose", "Block 的编码 agent", ".goose/skills"},
    {"copilot", "GitHub Copilot", "GitHub Copilot CLI", ".copilot/skills"},
    {"droid", "Factory Droid", "Factory AI 的 agent", ".factory/skills"},
    {"kiro", "Kiro CLI", "AWS 的 Kiro CLI", ".kiro/skills"},
    {"kimi", "Kimi CLI", "Moonshot AI 的 Kimi CLI", ".kimi/skills"},
    {"junie", "Junie", "JetBrains 的编码 agent", ".junie/skills"},
    {"openhands", "OpenHands", "OpenHands agent 平台", ".openhands/skills"},
    {"cline", "Cline", "Cline 编码助手", ".cline/skills"},
    {"roo", "Roo Code", "Roo Code 编码助手", ".roo/skills"},
    {"windsurf", "Windsurf", "Windsurf 编辑器", ".windsurf/skills;.codeium/windsurf/skills"},
    {"augment", "Augment Code", "Augment 的编码 CLI", ".augment/skills"},
    {"amp", "Amp", "Amp 编码 agent", ".amp/skills"},
    {"aider", "Aider", "AI 结对编程 CLI", ".aider/skills"},
    {"crush", "Crush", "Charm 的编码 agent", ".crush/skills"},
    {"commandcode", "Command Code", "Command Code 编码工具", ".commandcode/skills"},
    {"trae", "Trae", "Trae 编辑器的 agent", ".trae/skills"},
    {"mistral", "Mistral Vibe", "Mistral AI 的编码 agent", ".mistral/skills"},
    {NULL, NULL, NULL, NULL},
};

const sh_known_agent *sh_config_known_list(size_t *out_count) {
    size_t n = 0;
    while (KNOWN_AGENTS[n].id) n++;
    *out_count = n;
    return KNOWN_AGENTS;
}

/* First existing candidate path of a whitelist entry, absolute, or NULL. */
static char *known_agent_root(const char *paths, const char *home_dir) {
    const char *p = paths;
    while (*p) {
        const char *sep = strchr(p, ';');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        char *cand = malloc(strlen(home_dir) + len + 2);
        if (!cand) return NULL;
        int written = snprintf(cand, strlen(home_dir) + len + 2, "%s/%.*s", home_dir, (int)len, p);
        if (written <= 0) { free(cand); return NULL; }
        sh_fs_info info;
        int ok = sh_lstat(cand, &info) == SH_FS_OK && (info.is_dir || info.is_link);
        if (ok) return cand;
        free(cand);
        if (!sep) break;
        p = sep + 1;
    }
    return NULL;
}

int sh_config_add_agent_include(sh_config *cfg, const char *id, const char *root) {
    if (!id || !root) return 0;
    for (size_t i = 0; i < cfg->agent_count; i++) {
        if (strcmp(cfg->agents[i].id, id) != 0) continue;
        char *expanded = sh_path_expand(root, cfg->home_dir);
        if (!expanded) return 0;
        char *abs = sh_path_is_abs(expanded) ? expanded : sh_path_join(cfg->home_dir, expanded);
        if (abs != expanded) free(expanded);
        for (size_t k = 0; k < cfg->agents[i].include_count; k++) {
            if (strcmp(cfg->agents[i].include[k], abs) == 0) { free(abs); return 0; }
        }
        cfg->agents[i].include = realloc(cfg->agents[i].include,
                                         (cfg->agents[i].include_count + 1) * sizeof(char *));
        cfg->agents[i].include[cfg->agents[i].include_count++] = abs;
        return 1;
    }
    return 0;
}

const sh_agent *sh_config_find_agent(const sh_config *cfg, const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < cfg->agent_count; i++)
        if (strcmp(cfg->agents[i].id, id) == 0) return &cfg->agents[i];
    return NULL;
}

void sh_config_discover_agents(sh_config *cfg) {
    /*
     * Whitelist first: known agents are probed at their documented
     * locations and carry proper names and descriptions.
     */
    size_t known_count = 0;
    const sh_known_agent *known = sh_config_known_list(&known_count);
    for (size_t i = 0; i < known_count; i++) {
        if (sh_config_is_protected_agent(cfg, known[i].id)) continue;
        char *root = known_agent_root(known[i].paths, cfg->home_dir);
        if (root) {
            sh_config_add_agent_ex(cfg, known[i].id, root, known[i].label, known[i].description, 1);
            free(root);
        }
    }

    /* Any remaining <home>/.<name>/skills directory is a generic discovery. */
    sh_dir *d = sh_dir_open(cfg->home_dir);
    if (!d) return;

    sh_dirent ent;
    while (sh_dir_next(d, &ent) == 1) {
        if (ent.kind != SH_FS_KIND_DIR || ent.name[0] != '.') { free(ent.name); free(ent.path); continue; }

        char *skills = sh_path_join(ent.path, "skills");
        sh_fs_info info;
        /*
         * lstat, not stat: a dangling skills symlink (a view that was
         * deleted by hand or by a failed unlink) must still register the
         * agent, so that link can rebuild the view and repair the root.
         */
        if (sh_lstat(skills, &info) == SH_FS_OK && (info.is_dir || info.is_link)) {
            char *id = ent.name + 1; /* strip the leading dot */

            /* Whitelist agents were already probed above. */
            int already = 0;
            for (size_t i = 0; i < known_count; i++)
                if (strcmp(known[i].id, id) == 0) { already = 1; break; }

            if (!already && !sh_config_is_protected_agent(cfg, id))
                sh_config_add_agent(cfg, id, skills, id);
        }
        free(skills);
        free(ent.name);
        free(ent.path);
    }
    sh_dir_close(d);

    /*
     * ~/.agents/skills is the cross agent convention directory.  Skills
     * living there belong in every agent's view, which is how they are
     * shared today through hand written symlinks.
     */
    char *shared = sh_path_join(cfg->home_dir, ".agents/skills");
    sh_fs_info info;
    if (sh_stat(shared, &info) == SH_FS_OK && info.is_dir) {
        for (size_t i = 0; i < cfg->agent_count; i++)
            sh_config_add_agent_include(cfg, cfg->agents[i].id, shared);
    }
    free(shared);
}

int sh_config_is_excluded(const sh_config *cfg, const char *name) {
    for (size_t i = 0; i < cfg->exclude_count; i++)
        if (strcmp(cfg->exclude[i], name) == 0) return 1;
    return 0;
}

int sh_config_is_excluded_path(const sh_config *cfg, const char *path) {
    if (!path) return 0;
    for (size_t i = 0; i < cfg->exclude_path_count; i++)
        if (strstr(path, cfg->exclude_paths[i])) return 1;
    return 0;
}

int sh_config_is_protected_agent(const sh_config *cfg, const char *id) {
    if (!id) return 0;
    for (size_t i = 0; i < cfg->exclude_agent_count; i++)
        if (strcmp(cfg->exclude_agents[i], id) == 0) return 1;
    return 0;
}

int sh_config_is_external(const sh_config *cfg, const char *path) {
    for (size_t i = 0; i < cfg->external_marker_count; i++)
        if (strstr(path, cfg->external_markers[i])) return 1;
    return 0;
}

sh_fs_status sh_config_load(sh_config *cfg, const char *path) {
    char *data = NULL;
    size_t len = 0;
    sh_fs_status st = sh_read_file(path, &data, &len);
    if (st != SH_FS_OK) return st;

    sh_json *root = sh_json_parse(data, len);
    free(data);
    if (!root) return SH_FS_IO_ERROR;
    if (sh_json_type_of(root) != SH_JSON_OBJ) { sh_json_free(root); return SH_FS_IO_ERROR; }

    const sh_json *scan = sh_json_obj_get(root, "scan");
    size_t n;
    if (scan) {
        const sh_json *roots = sh_json_obj_get(scan, "roots");
        n = sh_json_arr_len(roots);
        if (n) {
            /* An explicit list replaces the default. */
            for (size_t i = 0; i < cfg->scan_root_count; i++) free(cfg->scan_roots[i]);
            free(cfg->scan_roots);
            cfg->scan_roots = NULL;
            cfg->scan_root_count = 0;
            for (size_t i = 0; i < n; i++) {
                const char *s = sh_json_as_str(sh_json_arr_get(roots, i));
                if (s) sh_config_add_scan_root(cfg, s);
            }
        }
        const sh_json *exc = sh_json_obj_get(scan, "exclude");
        n = sh_json_arr_len(exc);        if (n) {
            for (size_t i = 0; i < cfg->exclude_count; i++) free(cfg->exclude[i]);
            free(cfg->exclude);
            cfg->exclude = NULL;
            cfg->exclude_count = 0;
            for (size_t i = 0; i < n; i++) {
                const char *s = sh_json_as_str(sh_json_arr_get(exc, i));
                if (!s) continue;
                cfg->exclude = realloc(cfg->exclude, (cfg->exclude_count + 1) * sizeof(char *));
                cfg->exclude[cfg->exclude_count++] = strdup(s);
            }
        }
        const sh_json *exc_paths = sh_json_obj_get(scan, "exclude_paths");
        n = sh_json_arr_len(exc_paths);
        if (n) {
            for (size_t i = 0; i < cfg->exclude_path_count; i++) free(cfg->exclude_paths[i]);
            free(cfg->exclude_paths);
            cfg->exclude_paths = NULL;
            cfg->exclude_path_count = 0;
            for (size_t i = 0; i < n; i++) {
                const char *s = sh_json_as_str(sh_json_arr_get(exc_paths, i));
                if (!s) continue;
                cfg->exclude_paths =
                    realloc(cfg->exclude_paths, (cfg->exclude_path_count + 1) * sizeof(char *));
                cfg->exclude_paths[cfg->exclude_path_count++] = strdup(s);
            }
        }
        const sh_json *depth = sh_json_obj_get(scan, "max_depth");
        if (depth) cfg->max_depth = (int)sh_json_as_num(depth);
        const sh_json *follow = sh_json_obj_get(scan, "follow_symlinks");
        if (follow) cfg->follow_symlinks = sh_json_as_bool(follow);
        const sh_json *hash = sh_json_obj_get(scan, "hash");
        if (hash) cfg->hash_during_scan = sh_json_as_bool(hash);
    }

    const sh_json *view = sh_json_obj_get(root, "view");
    if (view) {
        const char *scope = sh_json_obj_str(view, "scope");
        if (scope) cfg->view_scope_all = strcmp(scope, "all") == 0;
    }

    const sh_json *protected_agents = sh_json_obj_get(root, "protected_agents");
    n = sh_json_arr_len(protected_agents);
    if (n) {
        for (size_t i = 0; i < cfg->exclude_agent_count; i++) free(cfg->exclude_agents[i]);
        free(cfg->exclude_agents);
        cfg->exclude_agents = NULL;
        cfg->exclude_agent_count = 0;
        for (size_t i = 0; i < n; i++) {
            const char *s = sh_json_as_str(sh_json_arr_get(protected_agents, i));
            if (!s) continue;
            cfg->exclude_agents =
                realloc(cfg->exclude_agents, (cfg->exclude_agent_count + 1) * sizeof(char *));
            cfg->exclude_agents[cfg->exclude_agent_count++] = strdup(s);
        }
    }

    const sh_json *agents = sh_json_obj_get(root, "agents");
    n = sh_json_arr_len(agents);
    for (size_t i = 0; i < n; i++) {
        const sh_json *a = sh_json_arr_get(agents, i);
        const char *id = sh_json_obj_str(a, "id");
        const char *root_path = sh_json_obj_str(a, "root");
        if (!id || !root_path) continue;
        sh_config_add_agent(cfg, id, root_path, sh_json_obj_str(a, "label"));
    }

    sh_json_free(root);
    return SH_FS_OK;
}

sh_fs_status sh_config_save(const sh_config *cfg, const char *path) {
    sh_jw w;
    sh_jw_init(&w);
    sh_jw_obj_begin(&w);

    sh_jw_key(&w, "version");
    sh_jw_num(&w, 1);

    sh_jw_key(&w, "scan");
    sh_jw_obj_begin(&w);
    sh_jw_key(&w, "roots");
    sh_jw_arr_begin(&w);
    for (size_t i = 0; i < cfg->scan_root_count; i++) sh_jw_str(&w, cfg->scan_roots[i]);
    sh_jw_arr_end(&w);
    sh_jw_key(&w, "exclude");
    sh_jw_arr_begin(&w);
    for (size_t i = 0; i < cfg->exclude_count; i++) sh_jw_str(&w, cfg->exclude[i]);
    sh_jw_arr_end(&w);
    sh_jw_key(&w, "exclude_paths");
    sh_jw_arr_begin(&w);
    for (size_t i = 0; i < cfg->exclude_path_count; i++) sh_jw_str(&w, cfg->exclude_paths[i]);
    sh_jw_arr_end(&w);
    sh_jw_key(&w, "max_depth");
    sh_jw_num(&w, cfg->max_depth);
    sh_jw_key(&w, "follow_symlinks");
    sh_jw_bool(&w, cfg->follow_symlinks);
    sh_jw_key(&w, "hash");
    sh_jw_bool(&w, cfg->hash_during_scan);
    sh_jw_obj_end(&w);

    sh_jw_key(&w, "view");
    sh_jw_obj_begin(&w);
    sh_jw_key(&w, "scope");
    sh_jw_str(&w, cfg->view_scope_all ? "all" : "own");
    sh_jw_obj_end(&w);

    sh_jw_key(&w, "protected_agents");
    sh_jw_arr_begin(&w);
    for (size_t i = 0; i < cfg->exclude_agent_count; i++) sh_jw_str(&w, cfg->exclude_agents[i]);
    sh_jw_arr_end(&w);

    sh_jw_key(&w, "agents");
    sh_jw_arr_begin(&w);    for (size_t i = 0; i < cfg->agent_count; i++) {
        sh_jw_obj_begin(&w);
        sh_jw_key(&w, "id");
        sh_jw_str(&w, cfg->agents[i].id);
        sh_jw_key(&w, "root");
        sh_jw_str(&w, cfg->agents[i].root);
        sh_jw_key(&w, "label");
        sh_jw_str(&w, cfg->agents[i].label ? cfg->agents[i].label : cfg->agents[i].id);
        sh_jw_obj_end(&w);
    }
    sh_jw_arr_end(&w);

    sh_jw_obj_end(&w);

    sh_fs_status st = sh_write_file(path, sh_jw_text(&w), sh_jw_len(&w));
    sh_jw_free(&w);
    return st;
}

void sh_config_free(sh_config *cfg) {
    if (!cfg) return;
    free(cfg->home);
    free(cfg->home_dir);
    for (size_t i = 0; i < cfg->scan_root_count; i++) free(cfg->scan_roots[i]);
    free(cfg->scan_roots);
    for (size_t i = 0; i < cfg->exclude_count; i++) free(cfg->exclude[i]);
    free(cfg->exclude);
    for (size_t i = 0; i < cfg->exclude_path_count; i++) free(cfg->exclude_paths[i]);
    free(cfg->exclude_paths);
    for (size_t i = 0; i < cfg->exclude_agent_count; i++) free(cfg->exclude_agents[i]);
    free(cfg->exclude_agents);
    for (size_t i = 0; i < cfg->external_marker_count; i++) free(cfg->external_markers[i]);
    free(cfg->external_markers);
    for (size_t i = 0; i < cfg->agent_count; i++) {
        free(cfg->agents[i].id);
        free(cfg->agents[i].root);
        free(cfg->agents[i].label);
        free(cfg->agents[i].description);
        for (size_t k = 0; k < cfg->agents[i].include_count; k++) free(cfg->agents[i].include[k]);
        free(cfg->agents[i].include);
    }
    free(cfg->agents);
    memset(cfg, 0, sizeof(*cfg));
}
