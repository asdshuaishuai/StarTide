/*
 * sh_config.h - skillctl configuration.
 *
 * The configuration lives in <home>/config.json.  Everything has a default
 * so the tool works on a fresh machine with no config file at all.
 */
#ifndef SH_CONFIG_H
#define SH_CONFIG_H

#include <stddef.h>

#include "sh_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *id;    /* short name, e.g. "zcode" */
    char *root;  /* absolute path of the agent's skills directory */
    char *label; /* human readable name */
    /* Extra roots whose skills belong in this agent's view. */
    char **include;
    size_t include_count;
    /* One-line description, from the known-agent whitelist when known. */
    char *description;
    /* 1 when the agent came from the built-in whitelist. */
    int known;
} sh_agent;

/*
 * The detection whitelist: agents with an established Agent Client
 * Protocol presence (agentclientprotocol.com) and a conventional skills
 * directory under the home.  Detection probes these first; anything else
 * found on disk that is not on the list is still picked up as a generic
 * discovery, it just misses the rich metadata.
 */
typedef struct {
    const char *id;
    const char *label;
    const char *description;
    /* Semicolon separated candidate paths, relative to the home. */
    const char *paths;
} sh_known_agent;

/* The built-in whitelist.  Terminated by an entry with id == NULL. */
const sh_known_agent *sh_config_known_list(size_t *out_count);

typedef struct {
    char *home;        /* skillctl's own directory, e.g. ~/.skillctl */
    char *home_dir;    /* the user's home directory */

    char **scan_roots;
    size_t scan_root_count;

    char **exclude;    /* directory names skipped during the walk */
    size_t exclude_count;

    char **exclude_paths; /* path fragments skipped during the walk */
    size_t exclude_path_count;

    char **exclude_agents; /* agent ids whose root must never be replaced */
    size_t exclude_agent_count;

    char **external_markers; /* path fragments marking read-only sources */
    size_t external_marker_count;

    int max_depth;
    int follow_symlinks;
    int hash_during_scan;
    int view_scope_all; /* 1: every agent view holds every unit */

    sh_agent *agents;
    size_t agent_count;
} sh_config;

/* Default skillctl home: $SKILLCTL_HOME, else ~/.skillctl. */
char *sh_config_default_home(void);

/* Fill in defaults for the given user home directory. */
void sh_config_defaults(sh_config *cfg, const char *home_dir);

/* Merge a config.json on top of the defaults.  Missing keys keep defaults. */
sh_fs_status sh_config_load(sh_config *cfg, const char *path);

/* Write the current configuration back to <home>/config.json. */
sh_fs_status sh_config_save(const sh_config *cfg, const char *path);

void sh_config_free(sh_config *cfg);

/* Add an agent, replacing an existing one with the same id. */
int sh_config_add_agent(sh_config *cfg, const char *id, const char *root, const char *label);

/* Add an agent with whitelist metadata (description, known flag). */
int sh_config_add_agent_ex(sh_config *cfg, const char *id, const char *root, const char *label,
                           const char *description, int known);

/* Add an extra root to an agent's view. */
int sh_config_add_agent_include(sh_config *cfg, const char *id, const char *root);

/* Look up an agent by id. */
const sh_agent *sh_config_find_agent(const sh_config *cfg, const char *id);

/* Add a scan root, ignoring duplicates. */
int sh_config_add_scan_root(sh_config *cfg, const char *path);

/* Add a path fragment that the walk must skip. */
int sh_config_add_exclude_path(sh_config *cfg, const char *path);

/* Discover agents by looking for <home>/.<name>/skills directories. */
void sh_config_discover_agents(sh_config *cfg);

/* True when the path belongs to a read-only source such as a plugin cache. */
int sh_config_is_external(const sh_config *cfg, const char *path);

/* True when the directory name is excluded from the walk. */
int sh_config_is_excluded(const sh_config *cfg, const char *name);

/* True when the path contains an excluded fragment. */
int sh_config_is_excluded_path(const sh_config *cfg, const char *path);

/* True when the agent id is protected from having its root replaced. */
int sh_config_is_protected_agent(const sh_config *cfg, const char *id);

/* Path of the config file inside skillctl's home. */
char *sh_config_path_in_home(const char *home, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SH_CONFIG_H */
