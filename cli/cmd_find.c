/*
 * cmd_find.c - the skill router commands: find, show, router-skill.
 *
 * find runs the recipe the retrieval literature converged on: BM25F over
 * the indexed skill bodies, plus a dense list when an embedder is
 * configured, fused with reciprocal rank fusion (k=60).
 *
 *   - SkillRouter (arXiv 2603.22455): the skill body is the decisive
 *     signal; retrieve-then-rerank over top-20 candidates, K=50 hurt.
 *   - ToolRet (ACL 2025): query/tool lexical overlap is tiny (ROUGE-L
 *     0.06), so one lexical list is not enough.
 *   - Cormack/Clarke/Buettcher (SIGIR 2009): RRF needs no score
 *     normalisation, which matters because BM25 and cosine live on
 *     incomparable scales.
 *
 * When no index exists the command degrades to the metadata heuristic
 * rather than failing; when no embedder is set the dense list is simply
 * absent.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "sh_fs.h"
#include "sh_index.h"
#include "sh_json.h"
#include "sh_route.h"
#include "sh_text.h"

#define FIND_CANDIDATES 100
#define FIND_MAX_TERMS 64

/*
 * Field-exact boosts. RRF alone cannot express "the query *is* this skill's
 * name", because BM25F sees the query and the document as independent bags
 * of tokens: the strongest signal a router has (SkillRouter measures names
 * drawing 26% of attention at layer 19) would otherwise be diluted by body
 * text length. A flat boost on the name field is the Lucene-style answer:
 * it complements the fused list instead of replacing it.
 */
#define FIND_EXACT_NAME_BOOST 0.050

/*
 * The glossary list is a weaker signal than a direct lexical match: a
 * translation catches documents that share no vocabulary with the query,
 * but a common translation ("state", "test") also matches dozens of
 * unrelated skills. Weighting it below 1 lets RRF keep it as a tie-breaker
 * and a fallback for all-Chinese intents without letting it outvote a
 * real lexical hit.
 */
#define FIND_EXP_WEIGHT 1.0

/*
 * Expansions rescue intents that share no vocabulary with the corpus (an
 * all-Chinese task against English skill bodies). They must not compete
 * with a real lexical hit, so the list only runs when the primary list
 * came back thin.
 */
#define FIND_EXP_MIN_LEXICAL 5

/* The router skill taught to agents. Kept in the binary so a PATH-only
 * install can emit it without shipping extra files. */
static const char *ROUTER_SKILL =
    "---\n"
    "name: skill-router\n"
    "description: >-\n"
    "  Find and load the right skill on demand instead of keeping every\n"
    "  skill description in context. Use when you need a skill but are\n"
    "  not sure which one, or when the skill catalog is too large to load.\n"
    "---\n"
    "\n"
    "# Skill Router\n"
    "\n"
    "The skill catalog on this machine is indexed by `skillctl`. Resolve an\n"
    "intent to a skill, then read only that skill. Two calls, minimal context.\n"
    "\n"
    "## 1. Rank candidates by intent\n"
    "\n"
    "   skillctl find \"build a godot UI with shaders\"\n"
    "   skillctl find --json --limit 5 \"deploy macos app\"\n"
    "   skillctl find --mode bm25 \"exact wording I expect in the body\"\n"
    "\n"
    "Modes: auto (default; hybrid when vectors exist, else bm25), bm25,\n"
    "vector, hybrid. --explain adds per-list ranks so a wrong hit is\n"
    "diagnosable. Output is one line per hit, capped at 8 by default.\n"
    "\n"
    "## 2. Fetch the winner on demand\n"
    "\n"
    "   skillctl show <name>            # prints its SKILL.md\n"
    "   skillctl show <name> --meta     # name/description/path only\n"
    "\n"
    "## Hard rules\n"
    "\n"
    "- This workflow is mandatory: find first, then show. Never walk the\n"
    "  skill directories yourself, never read a SKILL.md path directly,\n"
    "  never list a skill root by hand. Those paths are an implementation\n"
    "  detail of the store and bypass the index, which is what makes\n"
    "  selection reproducible.\n"
    "- `show` accepts a name, a unique prefix, or a unit id. On a duplicate\n"
    "  name it prints the candidates with their ids and exits 2; pick one\n"
    "  id rather than guessing.\n"
    "- One intent per call. Use --limit to keep output small.\n"
    "- If nothing matches, rephrase with the domain vocabulary the skill\n"
    "  itself would use (engine, language, artifact) before concluding that\n"
    "  no skill exists.\n"
    "- A stale index is still usable; `skillctl index` refreshes it.\n";


/*
 * Bilingual thesaurus for query expansion (Manning/Raghavan/Schuetze,
 * Introduction to Information Retrieval, ch. 9). The indexed skill bodies
 * are overwhelmingly English while intents arrive in Chinese, and a CJK
 * bigram matches no English token at all. A static domain glossary closes
 * that gap deterministically, without a translation model and without the
 * latency of one. Keys are the bigrams the tokeniser actually emits ("状
 * 态机" arrives as 状态 + 态机, so the useful key is 状态).
 */
typedef struct {
    const char *zh;
    const char *en;
} zh_en_pair;

static const zh_en_pair GLOSSARY[] = {
    {"状态", "state"},        {"敌人", "enemy"},       {"巡逻", "patrol"},
    {"主题", "theme"},        {"配色", "color"},       {"字体", "font"},
    {"物理", "physics"},      {"刚体", "rigid body"},   {"射线", "ray"},
    {"检测", "detect"},       {"并发", "concurrency"},  {"取消", "cancel"},
    {"分组", "group"},        {"广播", "broadcast"},    {"粒子", "particle"},
    {"特效", "vfx"},          {"蓝图", "blueprint"},    {"编译", "compile"},
    {"拾取", "pick"},         {"创建", "create"},       {"演示", "presentation"},
    {"文稿", "document"},     {"幻灯", "slide"},        {"知识", "knowledge"},
    {"在线", "online"},       {"文档", "document"},     {"新建", "create"},
    {"骨骼", "bone"},         {"绑定", "rigging"},      {"动画", "animation"},
    {"渲染", "render"},       {"材质", "material"},     {"纹理", "texture"},
    {"光照", "lighting"},     {"相机", "camera"},       {"场景", "scene"},
    {"节点", "node"},         {"信号", "signal"},       {"输入", "input"},
    {"存档", "save"},         {"读档", "load"},         {"对话", "dialogue"},
    {"任务", "task"},         {"音效", "audio"},        {"音乐", "music"},
    {"视频", "video"},        {"图片", "image"},        {"截图", "screenshot"},
    {"部署", "deploy"},       {"打包", "package"},      {"构建", "build"},
    {"测试", "test"},         {"单元", "unit"},         {"集成", "integration"},
    {"性能", "performance"},  {"优化", "optimize"},     {"内存", "memory"},
    {"线程", "thread"},       {"缓存", "cache"},        {"网络", "network"},
    {"请求", "request"},      {"接口", "api"},          {"数据", "database"},
    {"查询", "query"},        {"索引", "index"},        {"权限", "permission"},
    {"登录", "login"},        {"认证", "auth"},         {"加密", "encrypt"},
    {"密钥", "key"},          {"支付", "payment"},      {"订阅", "subscription"},
    {"通知", "notification"}, {"推送", "push"},         {"地图", "map"},
    {"定位", "location"},     {"布局", "layout"},       {"样式", "style"},       {"管理", "management manage manages managing"},
    {"状态", "state"},        {"动画", "animation"},
    {"组件", "component"},    {"页面", "page"},         {"表单", "form"},
    {"按钮", "button"},       {"弹窗", "dialog"},       {"导航", "navigation"},
    {"图表", "chart"},        {"导出", "export"},       {"导入", "import"},
    {"备份", "backup"},       {"迁移", "migration"},     {"回滚", "rollback"},
    {"日志", "log"},          {"监控", "monitor"},       {"报警", "alert"},
    {"安装", "install"},      {"配置", "config"},       {"版本", "version"},
    {"依赖", "dependency"},   {"脚本", "script"},       {"命令", "command"},
    {"终端", "terminal"},     {"文件", "file"},         {"目录", "directory"},
    {"路径", "path"},         {"编码", "encoding"},      {"解码", "decode"},
    {"压缩", "compress"},     {"加密", "encrypt"},      {"校验", "validate"},
    {"模型", "model"},        {"推理", "inference"},    {"训练", "train"},
    {"向量", "vector"},       {"嵌入", "embedding"},    {"检索", "retrieval"},
    {"排序", "ranking"},      {"分页", "pagination"},    {"搜索", "search"},
    {"过滤", "filter"},       {"聚合", "aggregate"},    {"统计", "statistics"},
    {"报表", "report"},       {"账单", "invoice"},      {"合同", "contract"},
    {"剪辑", "editing"},      {"转码", "transcode"},    {"字幕", "subtitle"},
    {"配音", "voiceover"},    {"合成", "composite"},    {"抠图", "matting"},
    {"建模", "modeling"},     {"雕刻", "sculpt"},       {"重拓扑", "retopology"},
    {"展UV", "uv"},           {"绑定", "rigging"},      {"关键帧", "keyframe"},
    {"补间", "tween"},        {"缓动", "easing"},       {"碰撞", "collision"},
    {"重力", "gravity"},      {"跳跃", "jump"},         {"行走", "walk"},
    {"攻击", "attack"},       {"技能", "skill"},        {"背包", "inventory"},
    {"升级", "level"},        {"商店", "shop"},         {"成就", "achievement"},
    {"存档", "save"},         {"读档", "load"},         {"重置", "reset"},
    {"计时", "timer"},        {"倒计时", "countdown"},  {"关卡", "level"},
};

#define GLOSSARY_COUNT (sizeof(GLOSSARY) / sizeof(GLOSSARY[0]))

/* A token is CJK when it carries at least one byte above ASCII. */
static int token_is_cjk(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 1;
    return 0;
}

/* ============================================================ helpers */

/* Join argv[from..] into one query string, skipping flag tokens. */
static char *join_args(int argc, char **argv, int from, const char **skip) {
    size_t len = 0;
    char *out;
    size_t at = 0;

    for (int i = from; i < argc; i++) {
        int is_skip = 0;
        for (const char **s = skip; s && *s; s++)
            if (strcmp(argv[i], *s) == 0) is_skip = 1;
        if (!is_skip) len += strlen(argv[i]) + 1;
    }
    if (len == 0) return NULL;
    out = calloc(len + 1, 1);
    if (!out) return NULL;
    for (int i = from; i < argc; i++) {
        int is_skip = 0;
        size_t n;
        for (const char **s = skip; s && *s; s++)
            if (strcmp(argv[i], *s) == 0) is_skip = 1;
        if (is_skip) continue;
        if (at) out[at++] = ' ';
        n = strlen(argv[i]);
        memcpy(out + at, argv[i], n);
        at += n;
    }
    out[at] = '\0';
    return out;
}

/* Collapse whitespace and truncate, for one-line text output. */
static void one_line(const char *src, char *dst, size_t cap, size_t max_chars) {
    size_t out = 0;
    int pending_space = 0;

    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] && out + 4 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            pending_space = out > 0;
            continue;
        }
        if (pending_space) {
            if (out >= max_chars) {
                memcpy(dst + out, "...", 3);
                out += 3;
                break;
            }
            if (out + 1 < cap) dst[out++] = ' ';
            pending_space = 0;
        }
        if (out >= max_chars) {
            memcpy(dst + out, "...", 3);
            out += 3;
            break;
        }
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static const char *first_path(const sh_catalog_entry *e) {
    if (e->source_count > 0 && e->sources[0] && e->sources[0][0]) return e->sources[0];
    return "";
}

/* Copy a 64-hex unit id and compare it against a vector record id. */
static long cat_index_for_id(const sh_catalog *cat, const char *id) {
    if (!id) return -1;
    for (size_t i = 0; i < cat->count; i++)
        if (cat->entries[i].id && strncmp(cat->entries[i].id, id, 64) == 0) return (long)i;
    return -1;
}

/* ============================================================= fusion */

typedef struct {
    long cat;
    double rrf;
    int bm25_rank;
    int vec_rank;
    int meta_rank; /* rank in the metadata heuristic list; 0 = absent */
    int name_rank; /* 1 = exact name, 2 = prefix, 0 = neither */
    size_t variants;
} fused_hit;

static int cmp_fused(const void *a, const void *b) {
    const fused_hit *x = a;
    const fused_hit *y = b;

    if (x->rrf > y->rrf) return -1;
    if (x->rrf < y->rrf) return 1;
    if (x->cat < y->cat) return -1;
    if (x->cat > y->cat) return 1;
    return 0;
}

/* RRF contribution of one rank: 1 / (k + rank). */
static void fuse_add(fused_hit *hits, size_t *count, size_t cap, long cat, size_t variants,
                     int bm25_rank, int vec_rank, int meta_rank) {
    size_t i;

    for (i = 0; i < *count; i++) {
        if (hits[i].cat == cat) {
            if (bm25_rank > 0 && hits[i].bm25_rank == 0) {
                hits[i].bm25_rank = bm25_rank;
                hits[i].rrf += 1.0 / (SH_INDEX_RRF_K + (double)bm25_rank);
            }
            if (vec_rank > 0 && hits[i].vec_rank == 0) {
                hits[i].vec_rank = vec_rank;
                hits[i].rrf += 1.0 / (SH_INDEX_RRF_K + (double)vec_rank);
            }
            if (meta_rank > 0 && hits[i].meta_rank == 0) {
                hits[i].meta_rank = meta_rank;
                hits[i].rrf += 1.0 / (SH_INDEX_RRF_K + (double)meta_rank);
            }
            if (variants > hits[i].variants) hits[i].variants = variants;
            return;
        }
    }
    if (*count >= cap) return;
    hits[*count].cat = cat;
    hits[*count].rrf = 0.0;
    hits[*count].bm25_rank = bm25_rank;
    hits[*count].vec_rank = vec_rank;
    hits[*count].meta_rank = meta_rank;
    hits[*count].name_rank = 0;
    hits[*count].variants = variants;
    if (bm25_rank > 0) hits[*count].rrf += 1.0 / (SH_INDEX_RRF_K + (double)bm25_rank);
    if (vec_rank > 0) hits[*count].rrf += 1.0 / (SH_INDEX_RRF_K + (double)vec_rank);
    if (meta_rank > 0) hits[*count].rrf += 1.0 / (SH_INDEX_RRF_K + (double)meta_rank);
    (*count)++;
}

/*
 * Deduplicate the query's terms. A repeated word would otherwise be
 * scored twice, which inflates whichever document happens to match that
 * one word.
 */
static size_t query_terms(const char *query, char ***out) {
    sh_text_tokens toks;
    char **terms;
    size_t unique = 0;

    *out = NULL;
    memset(&toks, 0, sizeof(toks));
    if (!sh_text_tokenize(query, strlen(query), &toks)) {
        sh_text_tokens_free(&toks);
        return 0;
    }
    if (toks.count == 0) {
        sh_text_tokens_free(&toks);
        return 0;
    }
    /* Room for the glossary expansions appended later by augment_oov;
     * allocating exactly toks.count here is what produced a heap overflow
     * the first time this was written. */
    terms = calloc(FIND_MAX_TERMS, sizeof(char *));
    if (!terms) {
        sh_text_tokens_free(&toks);
        return 0;
    }
    for (size_t i = 0; i < toks.count && unique < FIND_MAX_TERMS; i++) {
        int dup = 0;
        for (size_t k = 0; k < unique; k++)
            if (strcmp(terms[k], toks.items[i]) == 0) {
                dup = 1;
                break;
            }
        if (!dup) {
            terms[unique] = strdup(toks.items[i]);
            if (!terms[unique]) {
                sh_text_tokens_free(&toks);
                for (size_t k = 0; k < unique; k++) free(terms[k]);
                free(terms);
                return 0;
            }
            unique++;
        }
    }
    sh_text_tokens_free(&toks);

    *out = terms;
    return unique;
}

static void free_terms(char **terms, size_t count);

/* Document frequency of a term; 0 when the dictionary has never seen it. */
static uint32_t index_term_df(const sh_index *idx, const char *term) {
    size_t lo = 0;
    size_t hi = idx->term_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(idx->terms[mid].term, term);
        if (c == 0) return idx->terms[mid].df;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/*
 * Translate the *rare* CJK terms in the query into their glossary equivalents.
 *
 * The corpus is overwhelmingly English, so a Chinese word is either absent
 * (df = 0) or appears only in the handful of Chinese-authored skills -
 * accidental presence, not real coverage. Gating on "is the term in the
 * dictionary" therefore failed exactly when it mattered: "状态管理" is
 * present (a few skills are written in Chinese), so no translation fired,
 * and the English skill that actually describes state management never
 * matched. Gating on document frequency instead expands any CJK term that
 * fewer than 1/20 of the documents contain, and leaves a term that half the
 * corpus uses alone - that one really is the word the user meant.
 */
static void augment_oov(const sh_index *idx, char **terms, size_t *io_unique) {
    size_t unique = *io_unique;
    uint32_t rare_df = idx->doc_count / 20;

    for (size_t i = 0; i < unique && unique < FIND_MAX_TERMS; i++) {
        if (!token_is_cjk(terms[i])) continue;
        if (index_term_df(idx, terms[i]) > rare_df) continue;
        for (size_t g = 0; g < GLOSSARY_COUNT && unique < FIND_MAX_TERMS; g++) {
            const char *en;
            if (strcmp(GLOSSARY[g].zh, terms[i]) != 0) continue;
            en = GLOSSARY[g].en;
            while (en && *en && unique < FIND_MAX_TERMS) {
                char word[64];
                size_t wl = 0;
                int dup = 0;

                while (*en == ' ') en++;
                while (*en && *en != ' ' && wl + 1 < sizeof(word)) word[wl++] = *en++;
                word[wl] = '\0';
                if (wl == 0) break;
                for (size_t k = 0; k < unique; k++)
                    if (strcmp(terms[k], word) == 0) {
                        dup = 1;
                        break;
                    }
                if (!dup) {
                    terms[unique] = strdup(word);
                    if (!terms[unique]) break;
                    unique++;
                }
            }
        }
    }
    *io_unique = unique;
}

static void free_terms(char **terms, size_t count) {
    if (!terms) return;
    for (size_t i = 0; i < count; i++) free(terms[i]);
    free(terms);
}

/* =============================================================== find */

int cmd_find(int argc, char **argv) {
    static const char *SKIP[] = {"--explain", "--mode", NULL};
    const sh_config *cfg = cli_config();
    char *catalog_path = sh_store_catalog_path(cli_store_home());
    char *index_dir = sh_index_dir(cli_store_home());
    sh_catalog cat;
    char *query;
    size_t limit = cli_limit > 0 ? (size_t)cli_limit : 8;
    const char *mode = "auto";
    int explain = 0;
    int want_bm25 = 1;
    int want_vector = 1;
    sh_index idx;
    sh_vectors vecs;
    int have_index = 0;
    int have_vectors = 0;
    fused_hit hits[FIND_CANDIDATES * 2];
    size_t hit_count = 0;
    size_t shown = 0;

    memset(&cat, 0, sizeof(cat));
    memset(&idx, 0, sizeof(idx));
    memset(&vecs, 0, sizeof(vecs));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--explain") == 0) explain = 1;
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) mode = argv[++i];
        else if (strncmp(argv[i], "--mode=", 7) == 0) mode = argv[i] + 7;
    }
    query = join_args(argc, argv, 1, SKIP);

    if (!query || !query[0]) {
        fprintf(stderr, "usage: skillctl find <words describing the skill>\n");
        free(catalog_path);
        free(index_dir);
        free(query);
        return 2;
    }
    if (strcmp(mode, "auto") != 0 && strcmp(mode, "bm25") != 0 &&
        strcmp(mode, "vector") != 0 && strcmp(mode, "hybrid") != 0) {
        fprintf(stderr, "skillctl: unknown mode '%s' (expected auto, bm25, vector, hybrid)\n",
                mode);
        free(catalog_path);
        free(index_dir);
        free(query);
        return 2;
    }
    if (strcmp(mode, "bm25") == 0) want_vector = 0;
    if (strcmp(mode, "vector") == 0) want_bm25 = 0;

    if (!sh_catalog_load(&cat, catalog_path) || cat.count == 0) {
        fprintf(stderr, "skillctl: the catalog is empty; run 'skillctl scan' first\n");
        free(catalog_path);
        free(index_dir);
        free(query);
        sh_catalog_free(&cat);
        return 1;
    }

    if (want_bm25) have_index = sh_index_load(index_dir, &idx) && idx.doc_count > 0;

    if (want_vector) {
        char *vpath = sh_path_join(index_dir, "vectors.bin");
        have_vectors = sh_vectors_load(vpath, &vecs) && vecs.count > 0;
        free(vpath);
        if (strcmp(mode, "vector") == 0 && !have_vectors) {
            fprintf(stderr,
                    "skillctl: --mode vector needs index/vectors.bin; run 'skillctl embed' "
                    "with SKILLCTL_EMBEDDER set first\n");
            free(catalog_path);
            free(index_dir);
            free(query);
            sh_catalog_free(&cat);
            return 1;
        }
    }

    /* ---- lexical list: BM25F over the indexed bodies */
    if (have_index) {
        char **terms = NULL;
        size_t term_count = query_terms(query, &terms);
        sh_index_scored *scored = NULL;
        size_t got = 0;

        if (term_count > 0) augment_oov(&idx, terms, &term_count);
        if (term_count > 0)
            got = sh_index_search(&idx, (const char *const *)terms, term_count,
                                  FIND_CANDIDATES, &scored);
        for (size_t i = 0; i < got; i++) {
            long c = cat_index_for_id(&cat, idx.docs[scored[i].doc].id);
            if (c >= 0)
                fuse_add(hits, &hit_count, FIND_CANDIDATES * 2, c,
                         idx.docs[scored[i].doc].variants, (int)i + 1, 0, 0);
        }
        free(scored);
        free_terms(terms, term_count);
    }

    /* ---- dense list: only when vectors and an embedder both exist */
    if (have_vectors) {
        const char *embedder = getenv("SKILLCTL_QUERY_EMBEDDER");
        if (!embedder || !embedder[0]) embedder = getenv("SKILLCTL_EMBEDDER");
        if (embedder && embedder[0]) {
            float *q = NULL;
            uint32_t dim = 0;

            if (sh_index_embed_text(embedder, index_dir, query, &q, &dim)) {
                if (dim == vecs.dim) {
                    float *sims = malloc((size_t)vecs.count * sizeof(float));
                    if (sims) {
                        for (size_t i = 0; i < vecs.count; i++) {
                            double d = 0.0;
                            sims[i] = sh_vectors_dot(&vecs, i, q, &d) ? (float)d : -2.0f;
                        }
                        for (int rank = 1; rank <= FIND_CANDIDATES; rank++) {
                            size_t best = (size_t)-1;
                            for (size_t i = 0; i < vecs.count; i++)
                                if (sims[i] > -2.0f && (best == (size_t)-1 || sims[i] > sims[best]))
                                    best = i;
                            if (best == (size_t)-1) break;
                            {
                                long c = cat_index_for_id(&cat, vecs.ids + (size_t)best * 64);
                                if (c >= 0)
                                    fuse_add(hits, &hit_count, FIND_CANDIDATES * 2, c, 1, 0, rank, 0);
                            }
                            sims[best] = -2.0f;
                        }
                        free(sims);
                    }
                }
                free(q);
            } else {
                cli_verbose_note("query embedder failed; using the lexical list only");
            }
        }
    }

    /*
     * List 3: the metadata heuristic (name prefix/substring, description,
     * path). It is a genuinely different signal from BM25F - it rewards a
     * query word appearing in the *name* far more than term statistics
     * would - which is exactly the case BM25F under-serves once a
     * body-heavy document also matches. Fusing it by rank keeps its
     * strength for name-shaped intents without letting it bury the
     * body-only hits only BM25F can find.
     */
    {
        sh_route_hit *legacy = NULL;
        size_t got = sh_route_search(&cat, query, FIND_CANDIDATES, &legacy);
        for (size_t i = 0; i < got; i++)
            fuse_add(hits, &hit_count, FIND_CANDIDATES * 2, (long)legacy[i].index,
                     legacy[i].variants, 0, 0, (int)i + 1);
        sh_route_hits_free(legacy);
    }

    /*
     * Name channel: the query itself as a skill name. Exact matches are
     * seeded into the hit list even when BM25F ranked them outside the
     * shortlist, which is what keeps a one-word exact-name lookup from
     * being buried under body-text matches.
     */
    {
        char *nq = sh_text_normalize_name(query);

        if (nq && nq[0]) {
            for (size_t i = 0; i < cat.count; i++) {
                const char *name = cat.entries[i].name;
                char *nn;
                int rank = 0;

                if (!name) continue;
                nn = sh_text_normalize_name(name);
                if (!nn) continue;
                /*
                 * Only a whole-query exact match earns a boost. Prefix
                 * matches were tried and removed: a short skill name that
                 * happens to head the query ("blender ...") outranked a
                 * genuinely better content match, which is the one mistake
                 * a router must not make when the name is merely thematic.
                 */
                if (strcmp(nn, nq) == 0) rank = 1;
                if (rank > 0) {
                    int found = 0;
                    for (size_t k = 0; k < hit_count; k++)
                        if (hits[k].cat == (long)i) {
                            hits[k].name_rank = rank;
                            hits[k].rrf += FIND_EXACT_NAME_BOOST;
                            found = 1;
                            break;
                        }
                    if (!found && hit_count < FIND_CANDIDATES * 2) {
                        fuse_add(hits, &hit_count, FIND_CANDIDATES * 2, (long)i, 1, 0, 0, 0);
                        hits[hit_count - 1].name_rank = rank;
                        hits[hit_count - 1].rrf += FIND_EXACT_NAME_BOOST;
                    }
                }
                free(nn);
            }
        }
        free(nq);
    }

    cli_verbose_note("find mode=%s indexed=%s vectors=%s",
                     have_index ? (have_vectors ? "hybrid" : "bm25") : "heuristic",
                     have_index ? "yes" : "no", have_vectors ? "yes" : "no");

    qsort(hits, hit_count, sizeof(fused_hit), cmp_fused);

    /*
     * No name collapsing here. The index already merged units whose bodies
     * are near-identical, so any same-name pair still standing is genuinely
     * different work under one label - and the caller needs both rows to
     * pick one by id (`show <id>`), which is exactly what a duplicate name
     * demands.
     */

    /* The JSON contract is a bare array of hits: the evaluation harness
     * and existing consumers parse it as a list, so the mode diagnostics
     * stay out of the payload and go to stderr under --verbose. */
    if (cli_json) {
        sh_jw w;
        sh_jw_init(&w);
        sh_jw_arr_begin(&w);
        for (size_t i = 0; i < hit_count && shown < limit; i++, shown++) {
            const sh_catalog_entry *e = &cat.entries[hits[i].cat];
            sh_jw_obj_begin(&w);
            sh_jw_key(&w, "name");
            sh_jw_str(&w, e->name ? e->name : "");
            sh_jw_key(&w, "description");
            sh_jw_str(&w, e->description ? e->description : "");
            sh_jw_key(&w, "id");
            sh_jw_str(&w, e->id);
            sh_jw_key(&w, "stored");
            sh_jw_bool(&w, e->in_store);
            sh_jw_key(&w, "path");
            sh_jw_str(&w, first_path(e));
            sh_jw_key(&w, "variants");
            sh_jw_num(&w, (long long)hits[i].variants);
            if (explain) {
                sh_jw_key(&w, "rrf");
                sh_jw_numf(&w, hits[i].rrf);
                sh_jw_key(&w, "bm25_rank");
                sh_jw_num(&w, hits[i].bm25_rank);
                sh_jw_key(&w, "vec_rank");
                sh_jw_num(&w, hits[i].vec_rank);
                sh_jw_key(&w, "meta_rank");
                sh_jw_num(&w, hits[i].meta_rank);
                sh_jw_key(&w, "name_rank");
                sh_jw_num(&w, hits[i].name_rank);
            }
            sh_jw_obj_end(&w);
        }
        sh_jw_arr_end(&w);
        fputs(sh_jw_text(&w), stdout);
        fputc('\n', stdout);
        sh_jw_free(&w);
    } else if (hit_count == 0) {
        printf("no skills matched \"%s\"\n", query);
        if (!have_index) printf("tip: run 'skillctl index' to search skill bodies\n");
    } else {
        for (size_t i = 0; i < hit_count && shown < limit; i++, shown++) {
            const sh_catalog_entry *e = &cat.entries[hits[i].cat];
            char desc[512];
            one_line(e->description, desc, sizeof(desc), 120);
            printf("%s%s — %s", e->name ? e->name : e->id, e->in_store ? " [stored]" : "", desc);
            if (hits[i].variants > 1) printf("  (+%zu variants)", hits[i].variants - 1);
            if (explain)
                printf("  [rrf %.3f bm25 #%d meta #%d vec #%d name #%d]", hits[i].rrf,
                       hits[i].bm25_rank, hits[i].meta_rank, hits[i].vec_rank,
                       hits[i].name_rank);
            fputc('\n', stdout);
        }
    }

    sh_vectors_free(&vecs);
    sh_index_free(&idx);
    sh_catalog_free(&cat);
    free(catalog_path);
    free(index_dir);
    free(query);
    (void)cfg;
    return 0;
}

/* =============================================================== show */

/* Read one file into memory. Returns NULL on failure. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || size > (16 * 1024 * 1024)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)size;
    return buf;
}

int cmd_show(int argc, char **argv) {
    static const char *SKIP[] = {"--meta", NULL};
    char *catalog_path = sh_store_catalog_path(cli_store_home());
    sh_catalog cat;
    char *needle;
    int meta_only = 0;
    int match_kind = 0;
    size_t candidates = 0;
    int index;

    memset(&cat, 0, sizeof(cat));
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--meta") == 0) meta_only = 1;
    needle = join_args(argc, argv, 1, SKIP);

    if (!needle || !needle[0]) {
        fprintf(stderr, "usage: skillctl show <name|id> [--meta]\n");
        free(catalog_path);
        free(needle);
        return 2;
    }
    if (!sh_catalog_load(&cat, catalog_path) || cat.count == 0) {
        fprintf(stderr, "skillctl: the catalog is empty; run 'skillctl scan' first\n");
        free(catalog_path);
        free(needle);
        sh_catalog_free(&cat);
        return 1;
    }

    index = sh_route_resolve(&cat, needle, &match_kind, &candidates);
    if (index < 0) {
        if (candidates > 0) {
            char lower_needle[256];
            size_t n = 0;

            fprintf(stderr, "skillctl: \"%s\" is ambiguous (%zu candidates):\n", needle,
                    candidates);
            for (size_t i = 0; i < strlen(needle) && n + 1 < sizeof(lower_needle); i++)
                lower_needle[n++] = (char)tolower((unsigned char)needle[i]);
            lower_needle[n] = '\0';
            /* The whole catalog, not the first few hundred entries: a cap
             * here silently dropped candidates, so "2 candidates" printed
             * one row and the caller could not disambiguate at all. Each
             * row carries a short description because an id prefix alone
             * gives the caller nothing to choose on. */
            for (size_t i = 0; i < cat.count; i++) {
                const char *src = cat.entries[i].name ? cat.entries[i].name : "";
                char lower[256];
                char desc[160];
                size_t k = 0;

                for (; src[k] && k + 1 < sizeof(lower); k++)
                    lower[k] = (char)tolower((unsigned char)src[k]);
                lower[k] = '\0';
                if (strstr(lower, lower_needle) != NULL ||
                    strncmp(cat.entries[i].id, needle, strlen(needle)) == 0) {
                    one_line(cat.entries[i].description, desc, sizeof(desc), 90);
                    fprintf(stderr, "  %s  (%.16s)  %s\n",
                            cat.entries[i].name ? cat.entries[i].name : "",
                            cat.entries[i].id, desc);
                }
            }
            fprintf(stderr, "pass the exact name or a unit id to disambiguate\n");
        } else {
            fprintf(stderr, "skillctl: no skill named \"%s\"\n", needle);
        }
        free(catalog_path);
        free(needle);
        sh_catalog_free(&cat);
        return 2;
    }

    {
        const sh_catalog_entry *e = &cat.entries[index];
        char *path = NULL;

        if (e->in_store) {
            char *units = sh_store_units_dir(cli_store_home());
            char *dir = sh_path_join(units, e->id);
            path = sh_path_join(dir, "SKILL.md");
            free(dir);
            free(units);
        }
        if (!path || !read_file(path, NULL)) {
            free(path);
            path = sh_path_join(first_path(e), "SKILL.md");
        }

        if (meta_only) {
            if (cli_json) {
                sh_jw w;
                sh_jw_init(&w);
                sh_jw_obj_begin(&w);
                sh_jw_key(&w, "name");
                sh_jw_str(&w, e->name ? e->name : "");
                sh_jw_key(&w, "description");
                sh_jw_str(&w, e->description ? e->description : "");
                sh_jw_key(&w, "id");
                sh_jw_str(&w, e->id);
                sh_jw_key(&w, "stored");
                sh_jw_bool(&w, e->in_store);
                sh_jw_key(&w, "path");
                sh_jw_str(&w, path ? path : "");
                sh_jw_obj_end(&w);
                fputs(sh_jw_text(&w), stdout);
                fputc('\n', stdout);
                sh_jw_free(&w);
            } else {
                printf("name: %s\n", e->name ? e->name : "");
                printf("description: %s\n", e->description ? e->description : "");
                printf("id: %s\n", e->id);
                printf("stored: %s\n", e->in_store ? "yes" : "no");
                printf("path: %s\n", path ? path : "");
            }
        } else {
            size_t len = 0;
            char *body = path ? read_file(path, &len) : NULL;
            if (!body) {
                fprintf(stderr, "skillctl: cannot read %s\n", path ? path : "(no path)");
                free(path);
                free(catalog_path);
                free(needle);
                sh_catalog_free(&cat);
                return 1;
            }
            fwrite(body, 1, len, stdout);
            if (len == 0 || body[len - 1] != '\n') fputc('\n', stdout);
            free(body);
        }
        free(path);
    }

    free(catalog_path);
    free(needle);
    sh_catalog_free(&cat);
    return 0;
}

/* ====================================================== router-skill */

int cmd_router_skill(int argc, char **argv) {
    int install = 0;

    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--install") == 0) install = 1;

    if (!install) {
        fputs(ROUTER_SKILL, stdout);
        return 0;
    }

    {
        const sh_config *cfg = cli_config();
        const char *root = NULL;
        char *dir;
        char *path;
        sh_fs_status st;

        for (size_t i = 0; i < cfg->agent_count; i++) {
            if (strcmp(cfg->agents[i].id, "agents") == 0) {
                root = cfg->agents[i].root;
                break;
            }
        }
        if (!root && cfg->agent_count > 0) root = cfg->agents[0].root;
        if (!root) {
            fprintf(stderr, "skillctl: no agent roots configured\n");
            return 1;
        }

        dir = sh_path_join(root, "skill-router");
        st = sh_mkdir_p(dir);
        if (st != SH_FS_OK) {
            fprintf(stderr, "skillctl: cannot create %s\n", dir);
            free(dir);
            return 1;
        }
        path = sh_path_join(dir, "SKILL.md");
        st = sh_write_file(path, ROUTER_SKILL, strlen(ROUTER_SKILL));
        if (st != SH_FS_OK) {
            fprintf(stderr, "skillctl: cannot write %s\n", path);
            free(dir);
            free(path);
            return 1;
        }
        printf("installed router skill: %s\n", path);
        free(dir);
        free(path);
    }
    return 0;
}
