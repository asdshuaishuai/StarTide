/*
 * sh_yaml.h - a small YAML subset parser, sized for skill frontmatter.
 *
 * Real skills in the wild use block scalars for `description` (350 of 950
 * sampled files), double quotes, single quotes, nested `metadata:` maps and
 * plain multi-line scalars.  A single-line regex truncates a third of the
 * corpus, so this parser understands enough YAML to be correct for them.
 *
 * Supported:
 *   key: value                  plain scalar
 *   key: "value" / key: 'value' quoted scalar
 *   key: >   key: |             block scalar (folded / literal, with - + chomping)
 *   key:                      followed by an indented block (nested map or list)
 *   # comments, blank lines, CRLF line endings, UTF-8 BOM
 */
#ifndef SH_YAML_H
#define SH_YAML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *key;
    char *value;   /* scalar value, NULL for block entries */
    char *block;   /* raw text of a nested/block value, NULL for scalars */
    int is_block;
} sh_yaml_entry;

typedef struct {
    sh_yaml_entry *entries;
    size_t count;
    size_t cap;
} sh_yaml_doc;

/*
 * Parse a document.  Returns 1 when frontmatter was found and parsed,
 * 0 when the document has no frontmatter, -1 on a hard error (out of memory).
 */
int sh_yaml_parse(const char *text, size_t len, sh_yaml_doc *out);

/* Scalar value of key, or NULL.  Never returns a block's raw text. */
const char *sh_yaml_get(const sh_yaml_doc *doc, const char *key);

/* Raw block text of key, or NULL. */
const char *sh_yaml_get_block(const sh_yaml_doc *doc, const char *key);

void sh_yaml_free(sh_yaml_doc *doc);

#ifdef __cplusplus
}
#endif

#endif /* SH_YAML_H */
