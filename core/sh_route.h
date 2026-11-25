/*
 * sh_route.h - the skill router.
 *
 * Ranking catalog entries against a free-text intent lets an agent pull
 * the one skill it needs ("skillctl find <intent>" then "skillctl show
 * <name>") instead of loading every skill description into its context.
 * Pure computation over the catalog: no disk access, no globals.
 */
#ifndef SKILLCTL_ROUTE_H
#define SKILLCTL_ROUTE_H

#include <stddef.h>

#include "sh_store.h"

typedef struct {
    size_t index; /* index into the catalog the search was run on */
    long score;
} sh_route_hit;

/*
 * Score every catalog entry against the query (case-insensitive; tokens
 * split on whitespace) and return the best `limit` hits, highest score
 * first, ties broken by name. Allocates *out (free with
 * sh_route_hits_free); returns the number of hits, 0 for an empty query
 * or no match.
 */
size_t sh_route_search(const sh_catalog *cat, const char *query, size_t limit,
                       sh_route_hit **out);
void sh_route_hits_free(sh_route_hit *hits);

/*
 * Resolve a name or id to exactly one catalog entry.
 *
 * Tried in order: exact name, unique id prefix, unique name prefix, then
 * substring ranking that must have a unique best score. Returns the
 * catalog index or -1. *match_kind receives 1 (name), 2 (id), 3 (prefix),
 * 4 (substring) on success and 0 on failure; *out_candidates receives the
 * number of entries that tied, so the caller can tell "not found" from
 * "ambiguous".
 */
int sh_route_resolve(const sh_catalog *cat, const char *needle, int *match_kind,
                     size_t *out_candidates);

#endif
