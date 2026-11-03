/*
 * sh_json.h - minimal JSON reader and writer.
 *
 * The reader is used for skillctl's own config file.  The writer produces
 * the `--json` payloads consumed by the GUI.
 */
#ifndef SH_JSON_H
#define SH_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SH_JSON_NULL = 0,
    SH_JSON_BOOL,
    SH_JSON_NUM,
    SH_JSON_STR,
    SH_JSON_ARR,
    SH_JSON_OBJ
} sh_json_type;

typedef struct sh_json sh_json;

/* ------------------------------------------------------------- reading */

sh_json *sh_json_parse(const char *text, size_t len);
void sh_json_free(sh_json *j);

sh_json_type sh_json_type_of(const sh_json *j);
const sh_json *sh_json_obj_get(const sh_json *obj, const char *key);
size_t sh_json_arr_len(const sh_json *arr);
const sh_json *sh_json_arr_get(const sh_json *arr, size_t i);
const char *sh_json_as_str(const sh_json *j);
double sh_json_as_num(const sh_json *j);
int sh_json_as_bool(const sh_json *j);

/* Convenience: string value of key inside an object, or NULL. */
const char *sh_json_obj_str(const sh_json *obj, const char *key);
double sh_json_obj_num(const sh_json *obj, const char *key, double fallback);

/* ------------------------------------------------------------- writing */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sh_jbuf;

void sh_jbuf_init(sh_jbuf *b);
void sh_jbuf_free(sh_jbuf *b);
void sh_jbuf_puts(sh_jbuf *b, const char *s);
void sh_jbuf_putc(sh_jbuf *b, char c);
void sh_jbuf_putn(sh_jbuf *b, const char *s, size_t n);

/* Append a quoted, escaped JSON string. */
void sh_jbuf_json_str(sh_jbuf *b, const char *s);

typedef struct {
    sh_jbuf buf;
    int depth;
    int need_comma[64];
    int in_key;
} sh_jw;

void sh_jw_init(sh_jw *w);
void sh_jw_free(sh_jw *w);
const char *sh_jw_text(sh_jw *w);
size_t sh_jw_len(sh_jw *w);

void sh_jw_obj_begin(sh_jw *w);
void sh_jw_obj_end(sh_jw *w);
void sh_jw_arr_begin(sh_jw *w);
void sh_jw_arr_end(sh_jw *w);
void sh_jw_key(sh_jw *w, const char *key);
void sh_jw_str(sh_jw *w, const char *value);
void sh_jw_num(sh_jw *w, long long value);
void sh_jw_numf(sh_jw *w, double value);
void sh_jw_bool(sh_jw *w, int value);
void sh_jw_null(sh_jw *w);
void sh_jw_raw(sh_jw *w, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* SH_JSON_H */
