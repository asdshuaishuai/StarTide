/*
 * sh_yaml.c - YAML subset parser for skill frontmatter.
 */
#include "sh_yaml.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *ptr;
    size_t len;
    int indent;
    int blank;
} yline;

typedef struct {
    yline *lines;
    size_t count;
    size_t cap;
} ylines;

static int yl_push(ylines *l, const char *ptr, size_t len, int indent, int blank) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 128;
        yline *n = realloc(l->lines, cap * sizeof(yline));
        if (!n) return 0;
        l->lines = n;
        l->cap = cap;
    }
    l->lines[l->count].ptr = ptr;
    l->lines[l->count].len = len;
    l->lines[l->count].indent = indent;
    l->lines[l->count].blank = blank;
    l->count++;
    return 1;
}

static char *dupn(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *trim_dup(const char *s, size_t n) {
    while (n > 0 && (s[0] == ' ' || s[0] == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return dupn(s, n);
}

/* ------------------------------------------------------------- scalars */

static int is_blank_line(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!isspace((unsigned char)p[i])) return 0;
    return 1;
}

/* Strip a trailing " # comment" from an unquoted scalar. */
static void strip_inline_comment(char *s) {
    int in_single = 0, in_double = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\'' && !in_double) in_single = !in_single;
        else if (s[i] == '"' && !in_single) in_double = !in_double;
        else if (s[i] == '#' && !in_single && !in_double) {
            if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') { s[i] = '\0'; return; }
        }
    }
}

/* Resolve a quoted or plain scalar to its text value.  malloc'd. */
static char *scalar_value(const char *raw, size_t len) {
    char *tmp = trim_dup(raw, len);
    if (!tmp) return NULL;

    size_t n = strlen(tmp);
    if (n >= 2 && tmp[0] == '"' && tmp[n - 1] == '"') {
        char *out = malloc(n + 1);
        size_t o = 0;
        for (size_t i = 1; i < n - 1; i++) {
            if (tmp[i] == '\\' && i + 1 < n - 1) {
                char c = tmp[++i];
                switch (c) {
                    case 'n': out[o++] = '\n'; break;
                    case 't': out[o++] = '\t'; break;
                    case 'r': out[o++] = '\r'; break;
                    case '"': out[o++] = '"'; break;
                    case '\\': out[o++] = '\\'; break;
                    default: out[o++] = c; break;
                }
            } else {
                out[o++] = tmp[i];
            }
        }
        out[o] = '\0';
        free(tmp);
        return out;
    }
    if (n >= 2 && tmp[0] == '\'' && tmp[n - 1] == '\'') {
        char *out = malloc(n + 1);
        size_t o = 0;
        for (size_t i = 1; i < n - 1; i++) {
            if (tmp[i] == '\'' && i + 1 < n - 1 && tmp[i + 1] == '\'') { out[o++] = '\''; i++; }
            else out[o++] = tmp[i];
        }
        out[o] = '\0';
        free(tmp);
        return out;
    }

    strip_inline_comment(tmp);
    char *out = trim_dup(tmp, strlen(tmp));
    free(tmp);
    return out;
}

/* --------------------------------------------------------- doc building */

static int doc_push(sh_yaml_doc *doc, char *key, char *value, char *block, int is_block) {
    if (doc->count == doc->cap) {
        size_t cap = doc->cap ? doc->cap * 2 : 16;
        sh_yaml_entry *n = realloc(doc->entries, cap * sizeof(sh_yaml_entry));
        if (!n) return 0;
        doc->entries = n;
        doc->cap = cap;
    }
    doc->entries[doc->count].key = key;
    doc->entries[doc->count].value = value;
    doc->entries[doc->count].block = block;
    doc->entries[doc->count].is_block = is_block;
    doc->count++;
    return 1;
}

/* --------------------------------------------------------------- parser */

/* Collect the lines belonging to a block that starts after a key. */
static size_t block_extent(ylines *l, size_t start, int parent_indent) {
    size_t i = start;
    size_t end = start;
    while (i < l->count) {
        if (!l->lines[i].blank && l->lines[i].indent <= parent_indent) break;
        if (!l->lines[i].blank) end = i + 1;
        i++;
    }
    return end;
}

/* Grow the render buffer until it can hold used + need bytes. */
static char *grow_to(char *out, size_t *cap, size_t used, size_t need) {
    if (used + need <= *cap) return out;
    while (used + need > *cap) *cap *= 2;
    return realloc(out, *cap);
}

/* Render a block scalar (folded or literal) from the collected lines. */
static char *render_block_scalar(ylines *l, size_t start, size_t end, int folded, int chomp) {
    if (start >= end) return dupn("", 0);

    /* Content indent: the indentation of the first non-blank line. */
    int content_indent = -1;
    for (size_t i = start; i < end; i++) {
        if (!l->lines[i].blank) { content_indent = l->lines[i].indent; break; }
    }
    if (content_indent < 0) content_indent = 0;

    size_t cap = 4096;
    char *out = malloc(cap);
    size_t used = 0;
    out[0] = '\0';

    int pending_blanks = 0;
    int wrote_any = 0;

    for (size_t i = start; i < end; i++) {
        if (l->lines[i].blank) { pending_blanks++; continue; }

        const char *p = l->lines[i].ptr;
        size_t n = l->lines[i].len;
        int strip = content_indent;
        if (strip > (int)n) strip = (int)n;
        p += strip;
        n -= (size_t)strip;

        if (wrote_any) {
            if (folded) {
                if (pending_blanks > 0) {
                    out = grow_to(out, &cap, used, (size_t)pending_blanks + 2);
                    for (int b = 0; b < pending_blanks; b++) out[used++] = '\n';
                } else {
                    out = grow_to(out, &cap, used, 2);
                    out[used++] = ' ';
                }
            } else {
                out = grow_to(out, &cap, used, (size_t)pending_blanks + 3);
                out[used++] = '\n';
                for (int b = 0; b < pending_blanks; b++) out[used++] = '\n';
            }
        } else if (!folded) {
            /* A literal block keeps its leading blank lines. */
            out = grow_to(out, &cap, used, (size_t)pending_blanks + 2);
            for (int b = 0; b < pending_blanks; b++) out[used++] = '\n';
        }
        pending_blanks = 0;

        out = grow_to(out, &cap, used, n + 2);
        memcpy(out + used, p, n);
        used += n;
        wrote_any = 1;
    }

    if (!folded) {
        out = grow_to(out, &cap, used, (size_t)pending_blanks + 2);
        for (int b = 0; b < pending_blanks; b++) out[used++] = '\n';
    }
    out[used] = '\0';

    if (chomp == '-') {
        while (used > 0 && out[used - 1] == '\n') out[--used] = '\0';
    } else if (chomp == '+') {
        /* keep everything */
    } else {
        while (used > 0 && out[used - 1] == '\n') { out[--used] = '\0'; }
        if (used + 1 < cap) out[used++] = '\n';
        out[used] = '\0';
    }
    return out;
}

/* Render a nested block (map or list) as raw text. */
static char *render_nested(ylines *l, size_t start, size_t end, int parent_indent) {
    size_t cap = 1024;
    char *out = malloc(cap);
    size_t used = 0;
    out[0] = '\0';

    for (size_t i = start; i < end; i++) {
        if (l->lines[i].blank) {
            if (used + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[used++] = '\n';
            continue;
        }
        int strip = parent_indent + 2;
        if (strip > (int)l->lines[i].len) strip = (int)l->lines[i].len;
        size_t n = l->lines[i].len - (size_t)strip;
        const char *p = l->lines[i].ptr + strip;
        if (used + n + 2 > cap) {
            while (used + n + 2 > cap) cap *= 2;
            out = realloc(out, cap);
        }
        memcpy(out + used, p, n);
        used += n;
        out[used++] = '\n';
    }
    out[used] = '\0';
    return out;
}

int sh_yaml_parse(const char *text, size_t len, sh_yaml_doc *out) {
    memset(out, 0, sizeof(*out));
    if (!text) return 0;

    size_t start = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        start = 3;

    /* Split into lines, remembering offsets. */
    ylines lines = {0};
    size_t i = start;
    while (i <= len) {
        size_t eol = i;
        while (eol < len && text[eol] != '\n') eol++;
        size_t line_end = eol;
        if (line_end > i && text[line_end - 1] == '\r') line_end--;

        const char *p = text + i;
        size_t n = line_end - i;

        int indent = 0;
        while (indent < (int)n && (p[indent] == ' ' || p[indent] == '\t')) indent++;

        if (!yl_push(&lines, p, n, indent, is_blank_line(p, n))) break;
        if (eol >= len) break;
        i = eol + 1;
    }

    if (lines.count == 0) { free(lines.lines); return 0; }

    /* The first non-blank line must be the "---" fence. */
    size_t li = 0;
    while (li < lines.count && lines.lines[li].blank) li++;
    if (li >= lines.count) { free(lines.lines); return 0; }
    {
        const char *p = lines.lines[li].ptr;
        size_t n = lines.lines[li].len;
        if (!(n >= 3 && strncmp(p, "---", 3) == 0)) { free(lines.lines); return 0; }
    }
    li++;

    /* Find the closing fence. */
    size_t close = li;
    while (close < lines.count) {
        const char *p = lines.lines[close].ptr;
        size_t n = lines.lines[close].len;
        if ((n >= 3 && strncmp(p, "---", 3) == 0) || (n >= 3 && strncmp(p, "...", 3) == 0)) break;
        close++;
    }
    if (close >= lines.count) { free(lines.lines); return 0; }

    int found = 0;
    size_t k = li;
    while (k < close) {
        if (lines.lines[k].blank) { k++; continue; }
        if (lines.lines[k].indent > 0) { k++; continue; } /* stray continuation */

        const char *p = lines.lines[k].ptr;
        size_t n = lines.lines[k].len;

        /* Locate the "key:" separator. */
        size_t sep = (size_t)-1;
        for (size_t c = 0; c < n; c++) {
            if (p[c] == ':') {
                if (c + 1 == n || p[c + 1] == ' ' || p[c + 1] == '\t') { sep = c; break; }
            }
        }
        if (sep == (size_t)-1) { k++; continue; } /* not a mapping entry (e.g. a list item) */

        char *key = trim_dup(p, sep);
        const char *rest = p + sep + 1;
        size_t rest_len = n - sep - 1;

        /* Skip whitespace after the colon. */
        while (rest_len > 0 && (*rest == ' ' || *rest == '\t')) { rest++; rest_len--; }

        k++;

        if (rest_len == 0) {
            /* Either an empty scalar or a nested block below. */
            size_t end = block_extent(&lines, k, 0);
            if (end > k) {
                char *block = render_nested(&lines, k, end, 0);
                doc_push(out, key, NULL, block, 1);
                found = 1;
                k = end;
            } else {
                doc_push(out, key, dupn("", 0), NULL, 0);
                found = 1;
            }
            continue;
        }

        /* Block scalar? */
        if (rest[0] == '>' || rest[0] == '|') {
            int folded = rest[0] == '>';
            int chomp = 0; /* 0 = clip, '-' = strip, '+' = keep */
            int explicit_indent = 0;
            for (size_t c = 1; c < rest_len; c++) {
                if (rest[c] == '-') chomp = '-';
                else if (rest[c] == '+') chomp = '+';
                else if (rest[c] >= '1' && rest[c] <= '9') explicit_indent = rest[c] - '0';
            }
            (void)explicit_indent;

            size_t end = block_extent(&lines, k, 0);
            char *value = render_block_scalar(&lines, k, end, folded, chomp);
            doc_push(out, key, value, NULL, 1);
            found = 1;
            k = end;
            continue;
        }

        /* Plain or quoted scalar, possibly continued on more-indented lines. */
        {
            size_t cont_end = block_extent(&lines, k, 0);
            size_t extra = 0;
            for (size_t c = k; c < cont_end; c++) extra += lines.lines[c].len + 1;

            char *joined = malloc(rest_len + extra + 2);
            memcpy(joined, rest, rest_len);
            size_t used = rest_len;
            for (size_t c = k; c < cont_end; c++) {
                joined[used++] = ' ';
                memcpy(joined + used, lines.lines[c].ptr, lines.lines[c].len);
                used += lines.lines[c].len;
            }
            joined[used] = '\0';

            char *value = scalar_value(joined, used);
            free(joined);
            doc_push(out, key, value, NULL, 0);
            found = 1;
            k = cont_end;
        }
    }

    free(lines.lines);
    return found ? 1 : 0;
}

const char *sh_yaml_get(const sh_yaml_doc *doc, const char *key) {
    if (!doc || !key) return NULL;
    for (size_t i = 0; i < doc->count; i++) {
        if (doc->entries[i].key && strcmp(doc->entries[i].key, key) == 0)
            return doc->entries[i].value;
    }
    return NULL;
}

const char *sh_yaml_get_block(const sh_yaml_doc *doc, const char *key) {
    if (!doc || !key) return NULL;
    for (size_t i = 0; i < doc->count; i++) {
        if (doc->entries[i].key && strcmp(doc->entries[i].key, key) == 0)
            return doc->entries[i].block;
    }
    return NULL;
}

void sh_yaml_free(sh_yaml_doc *doc) {
    if (!doc) return;
    for (size_t i = 0; i < doc->count; i++) {
        free(doc->entries[i].key);
        free(doc->entries[i].value);
        free(doc->entries[i].block);
    }
    free(doc->entries);
    memset(doc, 0, sizeof(*doc));
}
