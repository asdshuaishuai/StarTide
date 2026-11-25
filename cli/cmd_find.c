/*
 * cmd_find.c - the skill router commands: find, show, router-skill.
 *
 * find  <intent...>   rank the catalog against a free-text intent
 * show  <name|id>     print the matched SKILL.md (the on-demand fetch)
 * router-skill        print (or --install) the router skill that teaches
 *                     agents this workflow
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "sh_json.h"
#include "sh_route.h"

/* The router skill taught to agents. Kept in the binary so a PATH-only
 * install can emit it without shipping extra files. */
static const char *ROUTER_SKILL =
    "---\n"
    "name: skill-router\n"
    "description: >-\n"
    "  Find and load the right skill on demand instead of keeping every\n"
    "  skill description in context. Use when you need a skill but are\n"
    "  not sure which one, or when the skill catalog is too large to load.\n"
    "---\n"
    "\n"
    "# Skill Router\n"
    "\n"
    "Use the `skillctl` CLI to resolve an intent to a skill, then read\n"
    "only that skill. Two calls, minimal context:\n"
    "\n"
    "1. Rank candidates by intent (names + one-line descriptions):\n"
    "\n"
    "   skillctl find \"build a godot UI with shaders\"\n"
    "   skillctl find --json --limit 5 \"deploy macos app\"\n"
    "\n"
    "2. Fetch the winner on demand (prints its SKILL.md):\n"
    "\n"
    "   skillctl show <name>\n"
    "   skillctl show <name> --meta    # name/description/path only\n"
    "\n"
    "Rules:\n"
    "\n"
    "- Do not scan skill directories by hand; `find` searches the whole\n"
    "  catalog in one call.\n"
    "- Prefer an exact name from the hit list. `show` accepts a name, a\n"
    "  unique prefix, or a unit id; on ambiguity it lists the candidates\n"
    "  and exits 2.\n"
    "- One intent per call; use --limit to keep output small (default 8).\n"
    "- If nothing matches, rephrase with the domain vocabulary (engine,\n"
    "  language, artifact) before concluding no skill exists.\n";

/* Join argv[from..] into one space-separated query string. */
static char *join_args(int argc, char **argv, int from) {
    size_t len = 0;
    char *out;
    size_t at = 0;

    for (int i = from; i < argc; i++) {
        if (strcmp(argv[i], "--meta") == 0 || strcmp(argv[i], "--install") == 0) continue;
        len += strlen(argv[i]) + 1;
    }
    if (len == 0) return NULL;
    out = calloc(len + 1, 1);
    if (!out) return NULL;
    for (int i = from; i < argc; i++) {
        if (strcmp(argv[i], "--meta") == 0 || strcmp(argv[i], "--install") == 0) continue;
        if (at) out[at++] = ' ';
        size_t n = strlen(argv[i]);
        memcpy(out + at, argv[i], n);
        at += n;
    }
    out[at] = '\0';
    return out;
}

/* Collapse whitespace and truncate, for one-line text output. */
static void one_line(const char *src, char *dst, size_t cap, size_t max_chars) {
    size_t out = 0;
    int pending_space = 0;

    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] && out + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            pending_space = out > 0;
            continue;
        }
        if (pending_space) {
            if (out >= max_chars) {
                memcpy(dst + out, "…", 4);
                out += 3;
                break;
            }
            dst[out++] = ' ';
            pending_space = 0;
        }
        if (out >= max_chars) {
            memcpy(dst + out, "…", 4);
            out += 3;
            break;
        }
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static const char *first_path(const sh_catalog_entry *e) {
    if (e->source_count > 0 && e->sources[0] && e->sources[0][0]) return e->sources[0];
    return "";
}

int cmd_find(int argc, char **argv) {
    const sh_config *cfg = cli_config();
    char *catalog_path = sh_store_catalog_path(cli_store_home());
    sh_catalog cat;
    char *query = join_args(argc, argv, 1);
    size_t limit = cli_limit > 0 ? (size_t)cli_limit : 8;
    sh_route_hit *hits = NULL;
    size_t got;

    if (!query || !query[0]) {
        fprintf(stderr, "usage: skillctl find <words describing the skill>\n");
        free(catalog_path);
        free(query);
        return 2;
    }

    if (!sh_catalog_load(&cat, catalog_path) || cat.count == 0) {
        free(catalog_path);
        free(query);
        sh_catalog_free(&cat);
        fprintf(stderr, "skillctl: the catalog is empty; run 'skillctl scan' first\n");
        return 1;
    }

    got = sh_route_search(&cat, query, limit, &hits);

    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_arr_begin(&w);
        for (size_t i = 0; i < got; i++) {
            const sh_catalog_entry *e = &cat.entries[hits[i].index];
            sh_jw_obj_begin(&w);
            sh_jw_key(&w, "name");
            sh_jw_str(&w, e->name ? e->name : "");
            sh_jw_key(&w, "description");
            sh_jw_str(&w, e->description ? e->description : "");
            sh_jw_key(&w, "id");
            sh_jw_str(&w, e->id);
            sh_jw_key(&w, "score");
            sh_jw_num(&w, hits[i].score);
            sh_jw_key(&w, "stored");
            sh_jw_bool(&w, e->in_store);
            sh_jw_key(&w, "path");
            sh_jw_str(&w, first_path(e));
            sh_jw_obj_end(&w);
        }
        sh_jw_arr_end(&w);
        fputs(sh_jw_text(&w), stdout);
        fputc('\n', stdout);
        sh_jw_free(&w);
    } else if (got == 0) {
        printf("no skills matched \"%s\"\n", query);
    } else {
        for (size_t i = 0; i < got; i++) {
            const sh_catalog_entry *e = &cat.entries[hits[i].index];
            char desc[512];
            one_line(e->description, desc, sizeof(desc), 120);
            printf("%s%s — %s\n", e->name ? e->name : e->id, e->in_store ? " [stored]" : "",
                   desc);
        }
    }

    sh_route_hits_free(hits);
    sh_catalog_free(&cat);
    free(catalog_path);
    free(query);
    (void)cfg;
    return 0;
}

/* Read one file into memory. Returns NULL on failure. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || size > (16 * 1024 * 1024)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)size;
    return buf;
}

static char *join_path(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *out = malloc(la + lb + 2);
    if (!out) return NULL;
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
    return out;
}

int cmd_show(int argc, char **argv) {
    char *catalog_path = sh_store_catalog_path(cli_store_home());
    sh_catalog cat;
    char *needle = join_args(argc, argv, 1);
    int meta_only = 0;
    int match_kind = 0;
    size_t candidates = 0;
    int index;

    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--meta") == 0) meta_only = 1;

    if (!needle || !needle[0]) {
        fprintf(stderr, "usage: skillctl show <name|id> [--meta]\n");
        free(catalog_path);
        free(needle);
        return 2;
    }
    if (!sh_catalog_load(&cat, catalog_path) || cat.count == 0) {
        free(catalog_path);
        free(needle);
        sh_catalog_free(&cat);
        fprintf(stderr, "skillctl: the catalog is empty; run 'skillctl scan' first\n");
        return 1;
    }

    index = sh_route_resolve(&cat, needle, &match_kind, &candidates);
    if (index < 0) {
        if (candidates > 0) {
            fprintf(stderr, "skillctl: \"%s\" is ambiguous (%zu candidates):\n", needle,
                    candidates);
            for (size_t i = 0; i < cat.count && i < 200; i++) {
                char name[256];
                size_t n = 0;
                const char *src = cat.entries[i].name ? cat.entries[i].name : "";
                for (; src[n] && n + 1 < sizeof(name); n++)
                    name[n] = (char)tolower((unsigned char)src[n]);
                name[n] = '\0';
                if (strstr(name, needle) || strncmp(cat.entries[i].id, needle, strlen(needle)) == 0)
                    fprintf(stderr, "  %s  (%.16s)\n",
                            cat.entries[i].name ? cat.entries[i].name : "",
                            cat.entries[i].id);
            }
            fprintf(stderr, "pass the exact name or a unit id to disambiguate\n");
        } else {
            fprintf(stderr, "skillctl: no skill named \"%s\"\n", needle);
        }
        free(catalog_path);
        free(needle);
        sh_catalog_free(&cat);
        return 2;
    }

    {
        const sh_catalog_entry *e = &cat.entries[index];
        char *path = NULL;

        if (e->in_store) {
            char *units = sh_store_units_dir(cli_store_home());
            char *dir = join_path(units, e->id);
            path = join_path(dir, "SKILL.md");
            free(dir);
            free(units);
        }
        if (!path || !read_file(path, NULL)) {
            free(path);
            path = join_path(first_path(e), "SKILL.md");
        }

        if (meta_only) {
            if (cli_json) {
                sh_jw w;
                sh_jw_init(&w);
                sh_jw_obj_begin(&w);
                sh_jw_key(&w, "name");
                sh_jw_str(&w, e->name ? e->name : "");
                sh_jw_key(&w, "description");
                sh_jw_str(&w, e->description ? e->description : "");
                sh_jw_key(&w, "id");
                sh_jw_str(&w, e->id);
                sh_jw_key(&w, "stored");
                sh_jw_bool(&w, e->in_store);
                sh_jw_key(&w, "path");
                sh_jw_str(&w, path ? path : "");
                sh_jw_obj_end(&w);
                fputs(sh_jw_text(&w), stdout);
                fputc('\n', stdout);
                sh_jw_free(&w);
            } else {
                printf("name: %s\n", e->name ? e->name : "");
                printf("description: %s\n", e->description ? e->description : "");
                printf("id: %s\n", e->id);
                printf("stored: %s\n", e->in_store ? "yes" : "no");
                printf("path: %s\n", path ? path : "");
            }
        } else {
            size_t len = 0;
            char *body = path ? read_file(path, &len) : NULL;
            if (!body) {
                fprintf(stderr, "skillctl: cannot read %s\n", path ? path : "(no path)");
                free(path);
                free(catalog_path);
                free(needle);
                sh_catalog_free(&cat);
                return 1;
            }
            fwrite(body, 1, len, stdout);
            if (len == 0 || body[len - 1] != '\n') fputc('\n', stdout);
            free(body);
        }
        free(path);
    }

    free(catalog_path);
    free(needle);
    sh_catalog_free(&cat);
    return 0;
}

int cmd_router_skill(int argc, char **argv) {
    int install = 0;

    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--install") == 0) install = 1;

    if (!install) {
        fputs(ROUTER_SKILL, stdout);
        return 0;
    }

    {
        const sh_config *cfg = cli_config();
        const char *root = NULL;
        char *dir = NULL;
        char *path = NULL;
        sh_fs_status st;

        /* Prefer the shared agents root, else the first configured agent. */
        for (size_t i = 0; i < cfg->agent_count; i++) {
            if (strcmp(cfg->agents[i].id, "agents") == 0) {
                root = cfg->agents[i].root;
                break;
            }
        }
        if (!root && cfg->agent_count > 0) root = cfg->agents[0].root;
        if (!root) {
            fprintf(stderr, "skillctl: no agent roots configured\n");
            return 1;
        }

        dir = join_path(root, "skill-router");
        st = sh_mkdir_p(dir);
        if (st != SH_FS_OK) {
            fprintf(stderr, "skillctl: cannot create %s\n", dir);
            free(dir);
            return 1;
        }
        path = join_path(dir, "SKILL.md");
        st = sh_write_file(path, ROUTER_SKILL, strlen(ROUTER_SKILL));
        if (st != SH_FS_OK) {
            fprintf(stderr, "skillctl: cannot write %s\n", path);
            free(dir);
            free(path);
            return 1;
        }
        printf("installed router skill: %s\n", path);
        free(dir);
        free(path);
    }
    return 0;
}
