---
layout: default
wrap: doc
title: CLI 参考
---

# CLI 参考

`skillctl` 是 StarTide 的引擎与命令行。GUI 不重写任何逻辑，它调用同一套 CLI。

任务导向的手册见 [USAGE.md](https://asdshuaishuai.github.io/StarTide/USAGE/)；这份文档只讲**命令、选项、输出形状**。

```
skillctl <command> [options]
```

## 全局选项

| 选项 | 作用 |
|---|---|
| `--json` | 机器可读输出（各命令的形状见下文） |
| `--dry-run` | 只演练，不改动磁盘（`import`/`link`/`unlink`/`sync`/`gc`） |
| `--verbose` / `-v` | 更多细节，诊断用 |
| `--agent <id>` | 只作用于一个 agent |
| `--filter <text>` | 按名字/描述过滤 catalog 输出 |
| `--limit <n>` | 限制输出条数 |
| `--root <path>` | 临时增加扫描根 |
| `--scope <own\|all>` | view 内容：仅该 agent 自己的技能，或全部技能 |
| `--stream` | `scan` 专用：逐行 JSON 进度事件 |
| `--no-hash` | `scan` 专用：跳过内容哈希（更快，去重更粗） |
| `--version` / `--help` | 版本 / 帮助 |

## 环境变量

| 变量 | 作用 | 缺省 |
|---|---|---|
| `SKILLCTL_HOME` | store 位置 | `~/.skillctl` |
| `SKILLCTL_BIN` | GUI/子进程定位引擎二进制 | 由 `find_cli()` 推断 |
| `SKILLCTL_GGUF_MODEL` | GGUF 模型文件或所在目录 | `<store>/models/f2llm-80m-gguf` 下第一个 `*.gguf` |
| `SKILLCTL_ONNX_MODEL` | ONNX 模型目录（含 `model*.onnx` + `vocab.txt` 或 `tokenizer.json`） | `<store>/models/multilingual-e5-small-onnx`，再退到 `bge-small-zh-v1.5` |
| `SKILLCTL_EMBEDDER` | 外部嵌入命令（读 stdin 文本、输出 JSON 数组），仅在**没有**本地模型时使用 | 无 |
| `SKILLCTL_QUERY_EMBEDDER` | 查询侧外部命令，缺省回退到 `SKILLCTL_EMBEDDER` | 无 |

## 命令

### 发现与索引

| 命令 | 说明 | 关键选项 | 退出码 |
|---|---|---|---|
| `scan` | 只读扫描主目录，找出所有含 `SKILL.md` 的目录 | `--stream`、`--no-hash`、`--root` | 0；目录不可读时计数并继续 |
| `catalog` / `list` | 列出已知技能（名字、描述、大小、来源数、可达 agent） | `--filter`、`--limit`、`--json` | 0 |
| `import` | 把发现的技能按内容哈希复制进 store（去重） | `--dry-run` | 1 = 有单元复制失败 |
| `index` | 为每个单元建 BM25F 倒排索引（正文、确定性 profile、索引期去重） | `--rebuild`、`--if-stale` | 1 = 建索引失败或 catalog 为空 |
| `embed` | 为每个单元生成向量（进程内 ONNX/GGUF） | — | 1 = 无模型 / 嵌入失败（已有向量保持不动） |

### 链接与回滚

| 命令 | 说明 | 关键选项 |
|---|---|---|
| `link` | 把每个 agent 的 skills 目录整体替换成指向 view 的软链；原目录 `rename` 进 trash | `--dry-run`、`--agent`、`--scope` |
| `unlink` | 从 trash 恢复原目录、删除 view | `--agent`、`--dry-run` |
| `sync` | 增量：吸收新出现的技能，刷新各 view | `--agent`、`--dry-run` |
| `gc` | 删除无引用的 store 单元与 trash | `--dry-run`（默认演练） |
| `status` | store 容量、单元数、各 agent 链接状态、漂移与孤儿 | `--json` |
| `doctor` | 环境自检：store 可写、软链支持、agent 探测、**向量路就绪度** | `--json` |

### 技能路由

| 命令 | 说明 | 关键选项 | 退出码 |
|---|---|---|---|
| `find`（别名 `route`） | 按意图排序技能（BM25F 正文 + 元数据启发式 + 可选稠密，RRF 融合）。默认输出前 8 条 | `--mode auto\|bm25\|vector\|hybrid`、`--explain`、`--rerank`、`--limit`、`--json` | 0；`--mode vector` 无向量时 1 |
| `show`（别名 `open`） | 取回某个技能的 `SKILL.md`（按需加载）。接受名字、唯一前缀或 unit id | `--meta`（只给元信息）、`--json` | 2 = 名字歧义；1 = 读不到文件 |
| `router-skill` | 打印（或 `--install` 写入 `~/.agents/skills/skill-router/`）路由技能本身 | `--install` | 1 = 写不进去 |

## `--json` 输出形状

**`find`** —— 裸数组（下游按列表解析）：

```json
[{"name":"godot-ui","description":"…","id":"<sha256>","stored":true,
  "path":"/Users/…/godot-ui","variants":1}]
```

`--explain` 时每条多出每一路的排名：

```json
[{"name":"godot-audio","…":"…","rrf":0.024397,"bm25_rank":54,
  "meta_rank":4,"vec_rank":0,"name_rank":0,"rerank":0.0}]
```

`vec_rank` 恒为 0 表示稠密路没参与（默认 `auto` 模式，或没有向量）。

**`show --meta --json`**：

```json
{"name":"…","description":"…","id":"…","stored":true,"path":"…/SKILL.md"}
```

**`index --json`**：

```json
{"ok":true,"docs":714,"terms":56708,"postings":300393,"merged":52,
 "seconds":0.46,"catalog":766}
```

`--if-stale` 且索引已是最新时：

```json
{"up_to_date":true,"docs":0}
```

**`embed --json`**：

```json
{"ok":true,"vectors":766,"dim":320}
```

**`doctor --json`** 额外包含向量路字段：

```json
{"vector_backend":"gguf","vector_model_sig":"feae1d87cb9982fe",
 "vectors_count":766,"vectors_dim":320,"vectors_match_encoder":true}
```

## store 布局

```
~/.skillctl/
├── config.json          # agents、扫描根、排除规则
├── units/<id>/          # 真实内容（id = 目录树 sha256 前 16 位）
├── catalog.jsonl        # 索引：每行一个单元
├── views/<agent>/       # 软链投影，每条指向 ../../units/<id>
├── trash/<agent>-<ts>/  # 被替换前的原件（可回滚，绝不 rm）
├── index/               # meta.json / terms.bin / postings.bin / vectors.bin
├── models/              # 本地向量模型（可选）
├── gui-theme.json       # GUI 主题偏好
└── logs/
```

## 退出码约定

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 运行失败（I/O、缺模型、catalog 为空） |
| 2 | 用法错误，或 `show` 遇到同名歧义（需改用 id） |
