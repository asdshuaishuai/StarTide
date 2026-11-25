/*
 * main.c - the skillctl command line entry point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"

#define SKILLCTL_VERSION "0.1.0"

static void print_help(void) {
    printf(
        "skillctl " SKILLCTL_VERSION " - unify the skills scattered across your machine\n"
        "\n"
        "Usage: skillctl <command> [options]\n"
        "\n"
        "Commands:\n"
        "  scan        find every skill on disk and record it in the catalog\n"
        "  catalog     list the skills skillctl knows about\n"
        "  import      copy the discovered skills into the managed store\n"
        "  link        replace each agent's skills directory with a symlink to the store\n"
        "  unlink      restore an agent's original skills directory\n"
        "  sync        refresh the views and adopt anything new that appeared\n"
        "  status      show the state of the store and of every agent\n"
        "  doctor      check that this machine can run skillctl\n"
        "  gc          remove store units that nothing points at\n"
        "  find        rank skills against a free-text intent (skill router)\n"
        "  show        print the SKILL.md of one skill (on-demand fetch)\n"
        "  router-skill print (or --install) the router skill for agents\n"
        "  index       build the BM25F index over every skill body\n"
        "  embed       build the vector index (needs SKILLCTL_EMBEDDER)\n"
        "\n"
        "Options:\n"
        "  --json            machine readable output\n"
        "  --dry-run         show what would happen, change nothing\n"
        "  --verbose         more detail while working\n"
        "  --agent <id>      restrict the command to one agent\n"
        "  --filter <text>   filter catalog entries by name or description\n"
        "  --limit <n>       cap the number of entries printed\n"
        "  --root <path>     add a directory to the scan roots\n"
        "  --scope <own|all> what a view contains: only the agent's own skills, or every skill\n"
        "  --stream          emit progress events as newline delimited JSON\n"
        "  --no-hash         skip content hashing (faster, less precise)\n"
        "  --meta            with show: name/description/path instead of the body\n"
        "  --install         with router-skill: write it into the agent root\n"
        "  --mode <m>        with find: auto, bm25, vector or hybrid\n"
        "  --explain         with find: show per-list ranks\n"
        "  --rerank          with find: re-order the top-20 (off by default)\n"
        "  --rebuild         with index: force a full rebuild\n"
        "  --if-stale        with index: skip when the catalog has not moved\n"
        "  --version         print the version\n"
        "  --help            print this help\n"
        "\n"
        "The store lives in $SKILLCTL_HOME, or ~/.skillctl by default.\n");
}

static int dispatch(int argc, char **argv) {
    const char *cmd = argv[0];

    if (strcmp(cmd, "scan") == 0) return cmd_scan(argc, argv);
    if (strcmp(cmd, "catalog") == 0 || strcmp(cmd, "list") == 0) return cmd_catalog(argc, argv);
    if (strcmp(cmd, "import") == 0) return cmd_import(argc, argv);
    if (strcmp(cmd, "link") == 0) return cmd_link(argc, argv);
    if (strcmp(cmd, "unlink") == 0) return cmd_unlink(argc, argv);
    if (strcmp(cmd, "sync") == 0) return cmd_sync(argc, argv);
    if (strcmp(cmd, "status") == 0) return cmd_status(argc, argv);
    if (strcmp(cmd, "doctor") == 0) return cmd_doctor(argc, argv);
    if (strcmp(cmd, "gc") == 0) return cmd_gc(argc, argv);
    if (strcmp(cmd, "find") == 0 || strcmp(cmd, "route") == 0) return cmd_find(argc, argv);
    if (strcmp(cmd, "show") == 0 || strcmp(cmd, "open") == 0) return cmd_show(argc, argv);
    if (strcmp(cmd, "router-skill") == 0) return cmd_router_skill(argc, argv);
    if (strcmp(cmd, "index") == 0) return cmd_index(argc, argv);
    if (strcmp(cmd, "embed") == 0) return cmd_embed(argc, argv);

    fprintf(stderr, "skillctl: unknown command '%s'\n", cmd);
    print_help();
    return 2;
}

int main(int argc, char **argv) {
    char **rest = calloc((size_t)argc + 1, sizeof(char *));
    int rest_count = 0;
    const char *command = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_help();
            free(rest);
            return 0;
        }
        if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("skillctl %s\n", SKILLCTL_VERSION);
            free(rest);
            return 0;
        }
        if (strcmp(a, "--json") == 0) { cli_json = 1; continue; }
        if (strcmp(a, "--all") == 0) { continue; } /* every agent is the default */
        if (strcmp(a, "--dry-run") == 0) { cli_dry_run = 1; continue; }
        if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) { cli_verbose = 1; continue; }
        if (strcmp(a, "--no-hash") == 0 || strcmp(a, "--no-catalog") == 0 ||
            strcmp(a, "--stream") == 0 || strcmp(a, "--meta") == 0 ||
            strcmp(a, "--install") == 0 || strcmp(a, "--explain") == 0 ||
            strcmp(a, "--rebuild") == 0 || strcmp(a, "--if-stale") == 0 ||
            strcmp(a, "--rerank") == 0 || strcmp(a, "--no-rerank") == 0) {
            rest[rest_count++] = argv[i];
            continue;
        }

        if (strncmp(a, "--agent=", 8) == 0) { cli_agent = a + 8; continue; }
        if (strcmp(a, "--agent") == 0 && i + 1 < argc) { cli_agent = argv[++i]; continue; }
        if (strncmp(a, "--filter=", 9) == 0) { cli_filter = a + 9; continue; }
        if (strcmp(a, "--filter") == 0 && i + 1 < argc) { cli_filter = argv[++i]; continue; }
        if (strncmp(a, "--limit=", 8) == 0) { cli_limit = atol(a + 8); continue; }
        if (strcmp(a, "--limit") == 0 && i + 1 < argc) { cli_limit = atol(argv[++i]); continue; }
        if (strncmp(a, "--root=", 7) == 0) { cli_extra_root = a + 7; continue; }
        if (strcmp(a, "--root") == 0 && i + 1 < argc) { cli_extra_root = argv[++i]; continue; }
        if (strncmp(a, "--mode=", 7) == 0) {
            rest[rest_count++] = "--mode=";
            rest[rest_count - 1] = argv[i];
            continue;
        }
        if (strcmp(a, "--mode") == 0 && i + 1 < argc) {
            rest[rest_count++] = argv[i];
            rest[rest_count++] = argv[++i];
            continue;
        }
        if (strncmp(a, "--scope=", 8) == 0) { cli_scope = a + 8; continue; }
        if (strcmp(a, "--scope") == 0 && i + 1 < argc) { cli_scope = argv[++i]; continue; }

        if (a[0] == '-') {
            fprintf(stderr, "skillctl: unknown option '%s'\n", a);
            free(rest);
            return 2;
        }
        if (!command) { command = a; continue; }
        rest[rest_count++] = argv[i];
    }

    if (!command) {
        print_help();
        free(rest);
        return 0;
    }

    /* Validate --scope before dispatching: an unrecognised value must not
     * silently degrade to the default scope. */
    if (cli_scope && strcmp(cli_scope, "own") != 0 && strcmp(cli_scope, "all") != 0) {
        fprintf(stderr, "skillctl: unknown scope '%s' (expected own or all)\n", cli_scope);
        free(rest);
        return 2;
    }

    /* dispatch() expects the command name as argv[0]. */
    char **args = calloc((size_t)rest_count + 2, sizeof(char *));
    args[0] = (char *)command;
    for (int i = 0; i < rest_count; i++) args[i + 1] = rest[i];

    int rc = dispatch(rest_count + 1, args);
    free(args);
    free(rest);
    return rc;
}
