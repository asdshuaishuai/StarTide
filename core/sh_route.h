/*
 * sh_route.h - the skill router.
 *
 * Ranking catalog entries against a free-text intent lets an agent pull
 * the one skill it needs instead of loading every skill description into
 * its context. The engine is a BM25F ranking (Robertson & Zaragoza,
 * "The Probabilistic Relevance Framework: BM25 and Beyond", 2009;
 * Robertson/Zaragoza/Taylor, "Simple BM25 extension to multiple weighted
 * fields", CIKM 2004) over three fields - name, description and source
 * path - with CJK bigram indexing for Chinese text and thesaurus-style
 * query expansion (Manning/Raghavan/Schuetze, Introduction to
 * Information Retrieval, ch. 9).
 *
 * Pure computation over the catalog: no disk access, no globals.
 */
#ifndef SKILLCTL_ROUTE_H
#define SKILLCTL_ROUTE_H

#include <stddef.h>

#include "sh_store.h"

typedef struct {
    size_t index;    /* index into the catalog the search was run on */
    long score;      /* scaled BM25F score, for ordering and display */
    size_t variants; /* how many units share this name (deduped) */
} sh_route_hit;

size_t sh_route_search(const sh_catalog *cat, const char *query, size_t limit,
                       sh_route_hit **out);
void sh_route_hits_free(sh_route_hit *hits);

/*
 * Resolve a name or id to exactly one catalog entry.
 *
 * Tried in order: exact name (duplicates are ambiguous by design - the
 * same name commonly ships from several roots with different content, so
 * the caller must disambiguate by id), unique id prefix, unique name
 * prefix, then substring ranking that must have a unique best score.
 * Returns the catalog index or -1. *match_kind receives 1 (name), 2 (id),
 * 3 (prefix), 4 (substring) on success and 0 on failure; *out_candidates
 * receives how many entries tied, so the caller can tell "not found" from
 * "ambiguous".
 */
int sh_route_resolve(const sh_catalog *cat, const char *needle, int *match_kind,
                     size_t *out_candidates);

#endif
