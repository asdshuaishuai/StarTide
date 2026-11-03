/*
 * sh_json.c - minimal JSON reader and writer.
 */
#include "sh_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sh_json {
    sh_json_type type;
    /* scalars */
    char *str;
    double num;
    int boolean;
    /* containers */
    sh_json **items;
    char **keys;
    size_t count;
    size_t cap;
};

/* ------------------------------------------------------------- reading */

static sh_json *json_new(sh_json_type t) {
    sh_json *j = calloc(1, sizeof(sh_json));
    if (j) j->type = t;
    return j;
}

void sh_json_free(sh_json *j) {
    if (!j) return;
    for (size_t i = 0; i < j->count; i++) {
        if (j->items) sh_json_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->str);
    free(j);
}

static int container_push(sh_json *j, char *key, sh_json *child) {
    if (j->count == j->cap) {
        size_t cap = j->cap ? j->cap * 2 : 8;
        sh_json **ni = realloc(j->items, cap * sizeof(sh_json *));
        if (!ni) return 0;
        j->items = ni;
        if (j->type == SH_JSON_OBJ) {
            char **nk = realloc(j->keys, cap * sizeof(char *));
            if (!nk) return 0;
            j->keys = nk;
        }
        j->cap = cap;
    }
    if (j->type == SH_JSON_OBJ) j->keys[j->count] = key;
    j->items[j->count] = child;
    j->count++;
    return 1;
}

typedef struct {
    const char *p;
    const char *end;
} jparser;

static sh_json *parse_value(jparser *jp);

static void skip_ws(jparser *jp) {
    while (jp->p < jp->end && isspace((unsigned char)*jp->p)) jp->p++;
}

static char *parse_string_raw(jparser *jp) {
    if (jp->p >= jp->end || *jp->p != '"') return NULL;
    jp->p++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (jp->p < jp->end && *jp->p != '"') {
        if (len + 5 >= cap) { cap *= 2; out = realloc(out, cap); }
        if (*jp->p == '\\') {
            jp->p++;
            if (jp->p >= jp->end) break;
            switch (*jp->p) {
                case 'n': out[len++] = '\n'; break;
                case 't': out[len++] = '\t'; break;
                case 'r': out[len++] = '\r'; break;
                case 'b': out[len++] = '\b'; break;
                case 'f': out[len++] = '\f'; break;
                case '/': out[len++] = '/'; break;
                case '"': out[len++] = '"'; break;
                case '\\': out[len++] = '\\'; break;
                case 'u': {
                    if (jp->p + 4 < jp->end) {
                        unsigned code = 0;
                        for (int i = 1; i <= 4; i++) {
                            char c = jp->p[i];
                            code <<= 4;
                            if (c >= '0' && c <= '9') code |= (unsigned)(c - '0');
                            else if (c >= 'a' && c <= 'f') code |= (unsigned)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') code |= (unsigned)(c - 'A' + 10);
                        }
                        jp->p += 4;
                        /* UTF-8 encode (no surrogate pairing support) */
                        if (code < 0x80) out[len++] = (char)code;
                        else if (code < 0x800) {
                            out[len++] = (char)(0xC0 | (code >> 6));
                            out[len++] = (char)(0x80 | (code & 0x3F));
                        } else {
                            out[len++] = (char)(0xE0 | (code >> 12));
                            out[len++] = (char)(0x80 | ((code >> 6) & 0x3F));
                            out[len++] = (char)(0x80 | (code & 0x3F));
                        }
                    }
                    break;
                }
                default: out[len++] = *jp->p; break;
            }
            jp->p++;
        } else {
            out[len++] = *jp->p++;
        }
    }
    if (jp->p < jp->end && *jp->p == '"') jp->p++;
    out[len] = '\0';
    return out;
}

static sh_json *parse_value(jparser *jp) {
    skip_ws(jp);
    if (jp->p >= jp->end) return NULL;

    char c = *jp->p;
    if (c == '{') {
        jp->p++;
        sh_json *obj = json_new(SH_JSON_OBJ);
        if (!obj) return NULL;
        skip_ws(jp);
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; return obj; }
        while (jp->p < jp->end) {
            skip_ws(jp);
            char *key = parse_string_raw(jp);
            if (!key) break;
            skip_ws(jp);
            if (jp->p < jp->end && *jp->p == ':') jp->p++;
            sh_json *val = parse_value(jp);
            if (!val) { free(key); break; }
            container_push(obj, key, val);
            skip_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
            if (jp->p < jp->end && *jp->p == '}') { jp->p++; break; }
            break;
        }
        return obj;
    }
    if (c == '[') {
        jp->p++;
        sh_json *arr = json_new(SH_JSON_ARR);
        if (!arr) return NULL;
        skip_ws(jp);
        if (jp->p < jp->end && *jp->p == ']') { jp->p++; return arr; }
        while (jp->p < jp->end) {
            sh_json *val = parse_value(jp);
            if (!val) break;
            container_push(arr, NULL, val);
            skip_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
            if (jp->p < jp->end && *jp->p == ']') { jp->p++; break; }
            break;
        }
        return arr;
    }
    if (c == '"') {
        char *s = parse_string_raw(jp);
        if (!s) return NULL;
        sh_json *j = json_new(SH_JSON_STR);
        if (!j) { free(s); return NULL; }
        j->str = s;
        return j;
    }
    if (jp->end - jp->p >= 4 && strncmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        sh_json *j = json_new(SH_JSON_BOOL);
        if (j) j->boolean = 1;
        return j;
    }
    if (jp->end - jp->p >= 5 && strncmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        sh_json *j = json_new(SH_JSON_BOOL);
        if (j) j->boolean = 0;
        return j;
    }
    if (jp->end - jp->p >= 4 && strncmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        return json_new(SH_JSON_NULL);
    }
    {
        char *endp = NULL;
        double v = strtod(jp->p, &endp);
        if (endp && endp != jp->p) {
            jp->p = endp;
            sh_json *j = json_new(SH_JSON_NUM);
            if (j) j->num = v;
            return j;
        }
    }
    jp->p++;
    return json_new(SH_JSON_NULL);
}

sh_json *sh_json_parse(const char *text, size_t len) {
    if (!text) return NULL;
    jparser jp = {text, text + len};
    return parse_value(&jp);
}

sh_json_type sh_json_type_of(const sh_json *j) { return j ? j->type : SH_JSON_NULL; }

const sh_json *sh_json_obj_get(const sh_json *obj, const char *key) {
    if (!obj || obj->type != SH_JSON_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->count; i++)
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

size_t sh_json_arr_len(const sh_json *arr) { return arr && arr->type == SH_JSON_ARR ? arr->count : 0; }

const sh_json *sh_json_arr_get(const sh_json *arr, size_t i) {
    if (!arr || arr->type != SH_JSON_ARR || i >= arr->count) return NULL;
    return arr->items[i];
}

const char *sh_json_as_str(const sh_json *j) { return j && j->type == SH_JSON_STR ? j->str : NULL; }
double sh_json_as_num(const sh_json *j) { return j && j->type == SH_JSON_NUM ? j->num : 0.0; }
int sh_json_as_bool(const sh_json *j) { return j && j->type == SH_JSON_BOOL ? j->boolean : 0; }

const char *sh_json_obj_str(const sh_json *obj, const char *key) {
    return sh_json_as_str(sh_json_obj_get(obj, key));
}

double sh_json_obj_num(const sh_json *obj, const char *key, double fallback) {
    const sh_json *v = sh_json_obj_get(obj, key);
    return v && v->type == SH_JSON_NUM ? v->num : fallback;
}

/* ------------------------------------------------------------- writing */

void sh_jbuf_init(sh_jbuf *b) {
    b->data = malloc(256);
    b->cap = b->data ? 256 : 0;
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void sh_jbuf_free(sh_jbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void jbuf_reserve(sh_jbuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (b->len + extra + 1 > cap) cap *= 2;
    b->data = realloc(b->data, cap);
    b->cap = cap;
}

void sh_jbuf_putc(sh_jbuf *b, char c) {
    jbuf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void sh_jbuf_putn(sh_jbuf *b, const char *s, size_t n) {
    jbuf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sh_jbuf_puts(sh_jbuf *b, const char *s) { sh_jbuf_putn(b, s, strlen(s)); }

void sh_jbuf_json_str(sh_jbuf *b, const char *s) {
    sh_jbuf_putc(b, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
                case '"': sh_jbuf_puts(b, "\\\""); break;
                case '\\': sh_jbuf_puts(b, "\\\\"); break;
                case '\n': sh_jbuf_puts(b, "\\n"); break;
                case '\r': sh_jbuf_puts(b, "\\r"); break;
                case '\t': sh_jbuf_puts(b, "\\t"); break;
                case '\b': sh_jbuf_puts(b, "\\b"); break;
                case '\f': sh_jbuf_puts(b, "\\f"); break;
                default:
                    if (*p < 0x20) {
                        char tmp[8];
                        snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                        sh_jbuf_puts(b, tmp);
                    } else {
                        sh_jbuf_putc(b, (char)*p);
                    }
            }
        }
    }
    sh_jbuf_putc(b, '"');
}

void sh_jw_init(sh_jw *w) {
    sh_jbuf_init(&w->buf);
    w->depth = 0;
    w->in_key = 0;
    memset(w->need_comma, 0, sizeof(w->need_comma));
}

void sh_jw_free(sh_jw *w) { sh_jbuf_free(&w->buf); }

const char *sh_jw_text(sh_jw *w) { return w->buf.data ? w->buf.data : ""; }
size_t sh_jw_len(sh_jw *w) { return w->buf.len; }

static void jw_sep(sh_jw *w) {
    if (w->in_key) { w->in_key = 0; return; }
    if (w->depth > 0 && w->need_comma[w->depth]) sh_jbuf_putc(&w->buf, ',');
    if (w->depth > 0) w->need_comma[w->depth] = 1;
}

void sh_jw_obj_begin(sh_jw *w) {
    jw_sep(w);
    sh_jbuf_putc(&w->buf, '{');
    if (w->depth < 63) { w->depth++; w->need_comma[w->depth] = 0; }
}

void sh_jw_obj_end(sh_jw *w) {
    if (w->depth > 0) w->depth--;
    sh_jbuf_putc(&w->buf, '}');
}

void sh_jw_arr_begin(sh_jw *w) {
    jw_sep(w);
    sh_jbuf_putc(&w->buf, '[');
    if (w->depth < 63) { w->depth++; w->need_comma[w->depth] = 0; }
}

void sh_jw_arr_end(sh_jw *w) {
    if (w->depth > 0) w->depth--;
    sh_jbuf_putc(&w->buf, ']');
}

void sh_jw_key(sh_jw *w, const char *key) {
    jw_sep(w);
    sh_jbuf_json_str(&w->buf, key);
    sh_jbuf_putc(&w->buf, ':');
    w->in_key = 1;
}

void sh_jw_str(sh_jw *w, const char *value) {
    jw_sep(w);
    sh_jbuf_json_str(&w->buf, value);
}

void sh_jw_num(sh_jw *w, long long value) {
    jw_sep(w);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", value);
    sh_jbuf_puts(&w->buf, tmp);
}

void sh_jw_numf(sh_jw *w, double value) {
    jw_sep(w);
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%.6f", value);
    sh_jbuf_puts(&w->buf, tmp);
}

void sh_jw_bool(sh_jw *w, int value) {
    jw_sep(w);
    sh_jbuf_puts(&w->buf, value ? "true" : "false");
}

void sh_jw_null(sh_jw *w) {
    jw_sep(w);
    sh_jbuf_puts(&w->buf, "null");
}

void sh_jw_raw(sh_jw *w, const char *json) {
    jw_sep(w);
    sh_jbuf_puts(&w->buf, json);
}
