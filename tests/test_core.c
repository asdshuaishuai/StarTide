/*
 * test_core.c - unit tests for the skillctl core primitives.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_config.h"
#include "sh_fs.h"
#include "sh_scan.h"
#include "sh_hash.h"
#include "sh_json.h"
#include "sh_yaml.h"
#include "sh_route.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                        \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                       \
    } while (0)

#define CHECK_STR(actual, expected)                                            \
    do {                                                                        \
        const char *a_ = (actual);                                             \
        const char *e_ = (expected);                                           \
        CHECK(a_ && e_ && strcmp(a_, e_) == 0, "expected \"%s\", got \"%s\"",   \
              e_ ? e_ : "(null)", a_ ? a_ : "(null)");                          \
    } while (0)

static void test_paths(void) {
    char *p;

    p = sh_path_join("/a/b", "c");
    CHECK_STR(p, "/a/b/c");
    free(p);

    p = sh_path_join("/a/b/", "/c");
    CHECK_STR(p, "/c");
    free(p);

    p = sh_path_join("a", "b");
    CHECK_STR(p, "a/b");
    free(p);

    p = sh_path_parent("/a/b/c");
    CHECK_STR(p, "/a/b");
    free(p);

    p = sh_path_parent("/a");
    CHECK_STR(p, "/");
    free(p);

    CHECK_STR(sh_path_base("/a/b/c"), "c");
    CHECK_STR(sh_path_base("c"), "c");

    p = sh_path_normalize("/a//b/./c/../d/");
    CHECK_STR(p, "/a/b/d");
    free(p);

    p = sh_path_expand("~/skills", "/home/u");
    CHECK_STR(p, "/home/u/skills");
    free(p);

    p = sh_path_expand("~other/x", "/home/u");
    CHECK_STR(p, "~other/x");
    free(p);

    CHECK(sh_path_has_prefix("/a/b/c", "/a/b"), "prefix should match");
    CHECK(!sh_path_has_prefix("/a/bc", "/a/b"), "prefix must respect the boundary");
    CHECK(sh_path_has_prefix("/a/b", "/a/b"), "equal paths match");

    p = sh_path_relative("/a/b", "/a/b/c/d");
    CHECK_STR(p, "c/d");
    free(p);

    p = sh_path_relative("/a/b/c", "/a/b");
    CHECK_STR(p, "..");
    free(p);

    p = sh_path_relative("/a/b", "/a/x/y");
    CHECK_STR(p, "../x/y");
    free(p);
}

static void test_yaml_basic(void) {
    const char *fm =
        "---\n"
        "name: 2d-essentials\n"
        "description: Use when working with 2D-specific systems\n"
        "---\n"
        "\n"
        "# Body\n";
    sh_yaml_doc doc;
    CHECK(sh_yaml_parse(fm, strlen(fm), &doc) == 1, "frontmatter should parse");
    CHECK_STR(sh_yaml_get(&doc, "name"), "2d-essentials");
    CHECK_STR(sh_yaml_get(&doc, "description"), "Use when working with 2D-specific systems");
    sh_yaml_free(&doc);
}

static void test_yaml_quoted_and_crlf(void) {
    const char *fm =
        "---\r\n"
        "name: \"flutter-building-forms\"\r\n"
        "description: 'single quoted'\r\n"
        "---\r\n";
    sh_yaml_doc doc;
    CHECK(sh_yaml_parse(fm, strlen(fm), &doc) == 1, "CRLF frontmatter should parse");
    CHECK_STR(sh_yaml_get(&doc, "name"), "flutter-building-forms");
    CHECK_STR(sh_yaml_get(&doc, "description"), "single quoted");
    sh_yaml_free(&doc);
}

static void test_yaml_block_scalar(void) {
    const char *fm =
        "---\n"
        "name: blocky\n"
        "description: >\n"
        "  first line\n"
        "  second line\n"
        "\n"
        "  third paragraph\n"
        "license: MIT\n"
        "---\n";
    sh_yaml_doc doc;
    CHECK(sh_yaml_parse(fm, strlen(fm), &doc) == 1, "block scalar should parse");
    const char *d = sh_yaml_get(&doc, "description");
    CHECK(d && strstr(d, "first line") != NULL, "folded text should contain the first line");
    CHECK(d && strstr(d, "second line") != NULL, "folded text should contain the second line");
    CHECK(d && strstr(d, "third paragraph") != NULL,
          "folded text should keep the later paragraph");
    CHECK_STR(sh_yaml_get(&doc, "license"), "MIT");
    sh_yaml_free(&doc);
}

static void test_yaml_literal_scalar(void) {
    const char *fm =
        "---\n"
        "name: lit\n"
        "description: |\n"
        "  line one\n"
        "  line two\n"
        "---\n";
    sh_yaml_doc doc;
    CHECK(sh_yaml_parse(fm, strlen(fm), &doc) == 1, "literal scalar should parse");
    const char *d = sh_yaml_get(&doc, "description");
    CHECK(d && strstr(d, "line one\n") != NULL, "literal keeps newlines");
    sh_yaml_free(&doc);
}

static void test_yaml_nested_metadata(void) {
    const char *fm =
        "---\n"
        "name: nested\n"
        "description: has metadata\n"
        "metadata:\n"
        "  category: engine\n"
        "  tags:\n"
        "    - godot\n"
        "    - 2d\n"
        "license: Apache-2.0\n"
        "---\n";
    sh_yaml_doc doc;
    CHECK(sh_yaml_parse(fm, strlen(fm), &doc) == 1, "nested metadata should parse");
    CHECK_STR(sh_yaml_get(&doc, "name"), "nested");
    CHECK_STR(sh_yaml_get(&doc, "description"), "has metadata");
    CHECK_STR(sh_yaml_get(&doc, "license"), "Apache-2.0");
    const char *block = sh_yaml_get_block(&doc, "metadata");
    CHECK(block && strstr(block, "category: engine") != NULL, "nested block is captured");
    sh_yaml_free(&doc);
}

static void test_yaml_no_frontmatter(void) {
    const char *text = "# Just a markdown file\n\nno frontmatter here\n";
    sh_yaml_doc doc;
    CHECK(sh_yaml_parse(text, strlen(text), &doc) == 0, "no frontmatter should report 0");
    sh_yaml_free(&doc);
}

static void test_yaml_key_order(void) {
    const char *fm =
        "---\n"
        "name: xlsx\n"
        "metadata:\n"
        "  a: b\n"
        "description: reorder\n"
        "license: MIT\n"
        "---\n";
    sh_yaml_doc doc;
    CHECK(sh_yaml_parse(fm, strlen(fm), &doc) == 1, "key order should not matter");
    CHECK_STR(sh_yaml_get(&doc, "description"), "reorder");
    sh_yaml_free(&doc);
}

static void test_hash(void) {
    char hex[65];
    sh_sha256_hex("", 0, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    sh_sha256_hex("abc", 3, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    sh_sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    char shortid[16];
    sh_hash_short(hex, 8, shortid, sizeof(shortid));
    CHECK_STR(shortid, "248d6a61");
}

static void test_json(void) {
    const char *text =
        "{\"version\": 1, \"name\": \"demo\", \"list\": [1, 2, 3], "
        "\"nested\": {\"flag\": true, \"ratio\": 0.5}}";
    sh_json *j = sh_json_parse(text, strlen(text));
    CHECK(j != NULL, "json should parse");
    CHECK_STR(sh_json_obj_str(j, "name"), "demo");
    CHECK(sh_json_obj_num(j, "version", 0) == 1, "version should be 1");
    CHECK(sh_json_arr_len(sh_json_obj_get(j, "list")) == 3, "list should have 3 entries");
    const sh_json *nested = sh_json_obj_get(j, "nested");
    CHECK(sh_json_as_bool(sh_json_obj_get(nested, "flag")) == 1, "flag should be true");
    CHECK(sh_json_obj_num(nested, "ratio", 0) == 0.5, "ratio should be 0.5");
    sh_json_free(j);

    sh_jw w;
    sh_jw_init(&w);
    sh_jw_obj_begin(&w);
    sh_jw_key(&w, "a");
    sh_jw_str(&w, "x\"y");
    sh_jw_key(&w, "b");
    sh_jw_arr_begin(&w);
    sh_jw_num(&w, 1);
    sh_jw_num(&w, 2);
    sh_jw_arr_end(&w);
    sh_jw_key(&w, "c");
    sh_jw_bool(&w, 1);
    sh_jw_obj_end(&w);
    CHECK_STR(sh_jw_text(&w), "{\"a\":\"x\\\"y\",\"b\":[1,2],\"c\":true}");
    sh_jw_free(&w);
}

static void test_fs_basics(void) {
    const char *tmp = "/tmp/skillctl_test_fs";
    sh_remove_tree(tmp);
    CHECK(sh_mkdir_p(tmp) == SH_FS_OK, "mkdir -p should succeed");

    char *nested = sh_path_join(tmp, "a/b/c");
    CHECK(sh_mkdir_p(nested) == SH_FS_OK, "nested mkdir should succeed");

    char *file = sh_path_join(tmp, "a/b/c/f.txt");
    CHECK(sh_write_file(file, "hello", 5) == SH_FS_OK, "write should succeed");

    char *data = NULL;
    size_t len = 0;
    CHECK(sh_read_file(file, &data, &len) == SH_FS_OK, "read should succeed");
    CHECK(len == 5 && data && memcmp(data, "hello", 5) == 0, "contents should round-trip");
    free(data);

    sh_fs_info info;
    CHECK(sh_lstat(nested, &info) == SH_FS_OK && info.is_dir, "nested should be a directory");

    /* Symlink round-trip. */
    char *link = sh_path_join(tmp, "link");
    CHECK(sh_symlink(nested, link, 1) == SH_FS_OK, "symlink should be created");
    CHECK(sh_lstat(link, &info) == SH_FS_OK && info.is_link, "link should be a link");
    CHECK(sh_stat(link, &info) == SH_FS_OK && info.is_dir, "stat should follow the link");
    char *target = sh_readlink(link);
    CHECK(target && strcmp(target, nested) == 0, "readlink should return the target");
    free(target);

    CHECK(sh_dir_size(tmp) >= 5, "dir size should count the file");
    sh_remove_tree(tmp);
}

static void test_tree_hash_stability(void) {
    const char *root = "/tmp/skillctl_test_hash";
    sh_remove_tree(root);
    char *skill = sh_path_join(root, "skill");
    sh_mkdir_p(skill);

    char *fm = sh_path_join(skill, "SKILL.md");
    sh_write_file(fm, "---\nname: demo\ndescription: d\n---\n", 38);

    char *ref = sh_path_join(skill, "references");
    sh_mkdir_p(ref);
    char *rf = sh_path_join(ref, "a.md");
    sh_write_file(rf, "reference\n", 10);

    char h1[65], h2[65];
    uint64_t size = 0;
    size_t files = 0;
    CHECK(sh_sha256_tree(skill, h1, &size, &files) == SH_FS_OK, "tree hash should work");
    CHECK(files == 2, "two files should be counted, got %zu", files);

    /* A dot-file must not change the identity. */
    char *lock = sh_path_join(skill, ".skills_store_lock.json");
    sh_write_file(lock, "{\"x\":1}", 7);
    CHECK(sh_sha256_tree(skill, h2, NULL, NULL) == SH_FS_OK, "rehash should work");
    CHECK_STR(h1, h2);

    /* A real content change must change the identity. */
    sh_write_file(rf, "reference changed\n", 17);
    CHECK(sh_sha256_tree(skill, h2, NULL, NULL) == SH_FS_OK, "rehash should work");
    CHECK(strcmp(h1, h2) != 0, "content change must change the hash");

    sh_remove_tree(root);
}


#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

/* A directory the user cannot read must be skipped, never forced. */
static void test_scan_skips_denied_dirs(void) {
    if (geteuid() == 0) return; /* root ignores file modes; test is meaningless */

    const char *root = "/tmp/skillctl_test_deny";
    sh_remove_tree(root);
    char *denied = sh_path_join(root, "locked");
    sh_mkdir_p(denied);
    char *ok = sh_path_join(root, "ok");
    sh_mkdir_p(ok);
    char *fm = sh_path_join(ok, "SKILL.md");
    sh_write_file(fm, "---\nname: ok\ndescription: d\n---\n", 38);

    chmod(denied, 0000);

    sh_config cfg;
    sh_config_defaults(&cfg, "/tmp");
    /* point the single scan root at the fixture */
    free(cfg.scan_roots[0]);
    cfg.scan_roots[0] = strdup(root);

    sh_scan_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.config = &cfg;
    opts.compute_hash = 0;

    sh_scan_result r;
    sh_scan_run(&opts, &r);

    CHECK(r.count == 1, "denied dir must be skipped, found %zu", r.count);
    CHECK(r.permissions_denied >= 1, "denial must be counted, got %zu",
          r.permissions_denied);
    CHECK(r.units[0].name && strcmp(r.units[0].name, "ok") == 0,
          "the readable skill is still found");

    sh_scan_result_free(&r);
    sh_config_free(&cfg);

    chmod(denied, 0755); /* restore so the cleanup can remove it */
    sh_remove_tree(root);
}
#endif


static void test_route(void) {
    sh_catalog cat;
    sh_route_hit *hits = NULL;
    size_t got;
    int kind = 0;
    size_t candidates = 0;

    memset(&cat, 0, sizeof(cat));
    cat.count = 3;
    cat.entries = calloc(3, sizeof(sh_catalog_entry));
    cat.entries[0].id = strdup("aaaa111122223333");
    cat.entries[0].name = strdup("godot-shaders");
    cat.entries[0].description = strdup("Write Godot shaders: vertex and fragment stages");
    cat.entries[0].in_store = 1;
    cat.entries[1].id = strdup("bbbb111122223333");
    cat.entries[1].name = strdup("godot-ui");
    cat.entries[1].description = strdup("Build Godot user interfaces with Control nodes");
    cat.entries[2].id = strdup("cccc111122223333");
    cat.entries[2].name = strdup("swiftui");
    cat.entries[2].description = strdup("Build SwiftUI views for macOS apps");
    cat.entries[2].in_store = 1;

    got = sh_route_search(&cat, "godot shader", 5, &hits);
    CHECK(got == 2, "expected 2 hits, got %zu", got);
    if (got == 2) {
        CHECK_STR(cat.entries[hits[0].index].name, "godot-shaders");
        CHECK(hits[0].score > hits[1].score, "shader match should outrank the plain godot hit");
    }
    sh_route_hits_free(hits);

    got = sh_route_search(&cat, "swiftui", 5, &hits);
    CHECK(got == 1 && cat.entries[hits[0].index].name &&
              strcmp(cat.entries[hits[0].index].name, "swiftui") == 0,
          "exact name should be the only hit");
    sh_route_hits_free(hits);

    got = sh_route_search(&cat, "", 5, &hits);
    CHECK(got == 0, "empty query must not match");
    sh_route_hits_free(hits);

    got = sh_route_search(&cat, "definitely-not-there", 5, &hits);
    CHECK(got == 0, "unrelated query must not match");
    sh_route_hits_free(hits);

    CHECK(sh_route_resolve(&cat, "godot-shaders", &kind, &candidates) == 0 && kind == 1,
          "exact name resolution failed");
    CHECK(sh_route_resolve(&cat, "godot-sh", &kind, &candidates) == 0 && kind == 3,
          "unique prefix resolution failed");
    CHECK(sh_route_resolve(&cat, "godot-", &kind, &candidates) == -1 && candidates == 2,
          "ambiguous prefix should report two candidates");
    CHECK(sh_route_resolve(&cat, "shaders", &kind, &candidates) == 0 && kind == 4,
          "unique substring resolution failed");
    CHECK(sh_route_resolve(&cat, "nope-nope", &kind, &candidates) == -1 && candidates == 0,
          "unknown needle should report no candidates");
    /* a duplicate name must be ambiguous, not silently first-match */
    free(cat.entries[1].name);
    cat.entries[1].name = strdup("godot-shaders");
    CHECK(sh_route_resolve(&cat, "godot-shaders", &kind, &candidates) == -1 && candidates == 2,
          "duplicate exact names should report two candidates");
    free(cat.entries[1].name);
    cat.entries[1].name = strdup("godot-ui");

    for (size_t i = 0; i < 3; i++) {
        free(cat.entries[i].id);
        free(cat.entries[i].name);
        free(cat.entries[i].description);
    }
    free(cat.entries);
}

int main(void) {
    test_paths();
    test_yaml_basic();
    test_yaml_quoted_and_crlf();
    test_yaml_block_scalar();
    test_yaml_literal_scalar();
    test_yaml_nested_metadata();
    test_yaml_no_frontmatter();
    test_yaml_key_order();
    test_hash();
    test_json();
    test_route();
    test_fs_basics();
    test_tree_hash_stability();

#ifndef _WIN32
    test_scan_skips_denied_dirs();
#endif

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
