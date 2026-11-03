/*
 * cli.h - shared helpers for the skillctl command line.
 */
#ifndef SKILLCTL_CLI_H
#define SKILLCTL_CLI_H

#include <stdio.h>

#include "sh_config.h"
#include "sh_scan.h"
#include "sh_store.h"

/* Global flags, set by main() from argv. */
extern int cli_json;
extern int cli_dry_run;
extern int cli_verbose;
extern const char *cli_agent;
extern const char *cli_filter;
extern long cli_limit;
extern const char *cli_extra_root;
extern const char *cli_scope; /* "own", "all" or NULL to use the config */

const sh_config *cli_config(void);
const char *cli_store_home(void);
const char *cli_home_dir(void);

/* Load the configuration, honouring SKILLCTL_HOME. */
void cli_load_config(void);

/* Print a line to stdout, unless --json is active. */
void cli_note(const char *fmt, ...);
void cli_verbose_note(const char *fmt, ...);
void cli_error(const char *fmt, ...);

/* Human readable byte counts. */
void cli_format_bytes(uint64_t bytes, char *out, size_t out_len);

/* ---------------------------------------------------------- link plan */

typedef struct {
    sh_link_entry *items;
    size_t count;
    size_t cap;
} cli_link_list;

/*
 * Collapse the scan into one representative unit per distinct content
 * hash, in name order.  The caller frees the array with cli_units_free.
 * Naming (which skill keeps the plain name and which gets a suffix) is
 * decided per view, in cli_links_for_agent, because the right primary
 * depends on which skills a given agent can see.
 */
int cli_unique_units(const sh_scan_result *scan, const sh_unit ***out_reps, size_t *out_count);
void cli_units_free(const sh_unit **reps);

typedef struct {
    sh_catalog catalog; /* keeps the strings the synthesized units point at */
    sh_unit *units;     /* synthesized units */
    size_t count;
    size_t cap;
} cli_catalog_units;

/*
 * Add skills that are already in the store but whose original location
 * the scan can no longer see: once an agent's skills directory is a
 * symlink into the view, the walk skips it.  Without this, rebuilding a
 * view would drop every skill that had been adopted earlier.  The
 * catalog's first source path becomes the synthesized unit's root, which
 * is what the per agent reachability test uses.
 */
int cli_units_with_catalog(const sh_unit ***reps, size_t *rep_count, const char *catalog_path,
                           cli_catalog_units *scratch);
void cli_catalog_units_free(cli_catalog_units *scratch);

/*
 * Build the link list for one agent: the representative units that are
 * reachable from the agent's home (or every unit with scope "all"),
 * grouped by name so that the primary keeps the plain name and the rest
 * are suffixed with the first eight hash characters.
 */
cli_link_list cli_links_for_agent(const sh_scan_result *scan, const sh_unit **reps,
                                  size_t rep_count, const sh_agent *agent, int scope_all);
void cli_link_list_free(cli_link_list *l);

/* ---------------------------------------------------------- commands */

int cmd_scan(int argc, char **argv);
int cmd_catalog(int argc, char **argv);
int cmd_import(int argc, char **argv);
int cmd_link(int argc, char **argv);
int cmd_unlink(int argc, char **argv);
int cmd_sync(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_doctor(int argc, char **argv);
int cmd_gc(int argc, char **argv);

#endif /* SKILLCTL_CLI_H */
