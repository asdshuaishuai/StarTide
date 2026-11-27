---
layout: default
wrap: doc
title: 架构
---

# 架构

## 一句话

C 引擎是唯一真相源；CLI 是它的全部对外接口；GUI 是调用 CLI 的外壳，不重写任何逻辑。

```
              ┌──────────────┐
  agents ────► │  core (C99)  │ ◄──── SKILL.md 树
  (~/.zcode/   │  扫描/哈希    │
   ~/.claude/  │  store/视图    │
   …)          │  索引/检索     │
              └──────┬───────┘
                     │ CLI（--json / --json-stream）
              ┌──────┴───────┐
              │  cli (C99)   │──── 人 / agent 直接使用
              └──────┬───────┘
                     │ spawn + JSON
              ┌──────┴───────┐
              │ gui (Rust)   │  gpui-kit 原生窗口
              └──────────────┘
```

## 模块

| 文件 | 职责 |
|---|---|
| `core/sh_fs_posix.c` / `sh_fs_win32.c` | 平台抽象：遍历、软链/junction、原子 rename、树复制 |
| `core/sh_hash.c` | SHA-256（自实现），内容寻址的基础 |
| `core/sh_yaml.c` | frontmatter 子集解析（block scalar、引号、CRLF、嵌套 metadata） |
| `core/sh_json.c` | JSON 读写（`sh_json` 只读解析 + `sh_jw` 流式写出） |
| `core/sh_scan.c` | 扫描器：不跟随软链、排除表、权限边界、可达性规则 |
| `core/sh_store.c` | 内容寻址 store、catalog、view 投影与根替换 |
| `core/sh_config.c` | 配置加载与 agent 探测（ACP 白名单） |
| `core/sh_text.c` | 分词（ASCII/标识符切分/CJK bigram/停用词）、确定性 profile 富化、5-gram shingle 近重复 |
| `core/sh_index.c` | 倒排索引（五字段 postings）、BM25F 打分、索引期去重、向量文件读写 |
| `core/sh_route.c` | 名字/id 解析（精确名→id 前缀→名前缀→唯一子串），以及无索引时的元数据启发式 |
| `core/sh_wordpiece.c` | BERT WordPiece 分词（读 `vocab.txt`） |
| `core/sh_unigram.c` | SentencePiece Unigram 分词（读 `tokenizer.json`，Viterbi + Metaspace） |
| `core/sh_onnx.c` | ONNX Runtime 会话封装（CLS / mean 池化） |
| `core/sh_llama.c` | llama.cpp 会话封装（GGUF，last-token 池化） |
| `cli/cmd_*.c` | 每个命令一个文件，只做参数解析和输出格式化 |
| `cli/cli_common.c` | 全局 flag、store home、link 计划构造 |
| `cli/cli_embedder.c` | 模型发现与"分词→编码"的唯一入口（两个后端共用） |

## 三条数据流

**发现与统一**

```
scan ──► scan_result（单元 + 可达根 + 别名，全部只读）
     ──► import：按内容哈希复制进 units/，写 catalog.jsonl
     ──► link：每个 agent 根 rename 进 trash，根换成指向 views/<agent> 的软链
```

**检索**

```
find "<意图>"
  ├─ 分词（同一套分词器，查询与文档一致）
  ├─ 稀有 CJK 词按文档频率触发中英术语扩展
  ├─ 词面路：BM25F（name 4.0 / desc 2.0 / profile 1.5 / body 1.0 / path 0.5，k1=1.2）
  ├─ 元数据路：名字精确/子串 + 描述 + 路径（名字信号是 BM25F 表达不出来的）
  ├─ 稠密路（opt-in）：GGUF/ONNX 向量，余弦 top-100
  ├─ RRF 融合（k=60），整串精确名 boost
  └─ 重排（opt-in）：top-20 用字段词频重排
```

**按需加载**

```
show <name|id> ──► store units/<id>/SKILL.md（或回退到首个 source）
                ──► 同名多条时列出候选 + 退出码 2
```

## 不变量

这些是设计约束，改动时不应破坏：

1. **只读优先**：`scan` 不改动任何磁盘内容；`link`/`unlink` 的破坏性动作只在显式命令下发生，且原目录永远 `rename` 进 trash，绝不 `rm`。
2. **权限边界**：扫描与读写只在用户权限可达范围内；遇到 EACCES/EPERM 就跳过并计数，绝不提权重试。
3. **内容寻址**：单元身份 = 目录树哈希（跳过噪声文件），因此导入是幂等的、去重是天然的。
4. **软链不展开**：扫描不跟随软链（44% 的条目是软链，展开会拖进数百 MB）。
5. **索引期去重**：正文 5-gram shingle Jaccard ≥ 0.8 的变体合并为一个文档，记 `variants`；`find` 不按名字再合并——剩下的同名是真正不同的内容。
6. **模型可替换但不可混用**：`vectors.bin` 带模型指纹，换模型必须重跑 `embed`。
7. **降级而非失败**：无索引→元数据启发式；无模型→跳过稠密路；无 ONNX/llama.cpp→编译期空实现。`find` 永远可用。

## 测试

| 层 | 位置 | 覆盖 |
|---|---|---|
| 单元 | `tests/test_core.c`（130 项） | 路径、YAML（block scalar/引号/CRLF）、SHA-256 向量、JSON、扫描权限、树哈希稳定性、路由排序与同名歧义、分词/去重/profile、BM25F 手算值、向量文件往返与 v1 兼容、WordPiece |
| 端到端 | `tests/e2e.sh`（44 项） | 在临时 home/store 里跑完整流程：扫描→导入→链接→回滚→GC→同步，**以及降级契约**（无模型/无向量时各模式的行为） |
| 指标 | `tests/router_eval.sh` | 80 条分层查询：Hit@1 / MRR@10 / Recall@3 / @10 / nDCG@10；`ROUTER_MODE` 可切模式对照 |
| 内存 | `make asan-smoke` | ASAN+UBSAN 下的 doctor/status 冒烟；手工跑过 embed/find |

基线数字见 `tests/router_baseline.txt`，最新结果见 README「评测」表。

## 设计依据

检索部分的取舍都来自论文而不是直觉，逐条对应关系见 README「算法与依据」表
（SkillRouter 2603.22455、Enrich-Retrieve-Rank 2608.22695、ToolRet ACL 2025、
Lost in Implicit Routing 2610.18522、REALM 2026 2607.17598）。

## 相关文档

| 文档 | 讲什么 |
|---|---|
| [README.md](https://github.com/asdshuaishuai/StarTide#readme) | 项目主页：定位、安装、算法与依据、评测 |
| [USAGE.md](https://asdshuaishuai.github.io/StarTide/USAGE/) | 使用手册：上手步骤、日常维护、路由工作流、排错 |
| [CLI.md](https://asdshuaishuai.github.io/StarTide/CLI/) | 命令、选项、环境变量、`--json` 形状、退出码 |
| [THEMES.md](https://asdshuaishuai.github.io/StarTide/THEMES/) | 四套设计语言的 token、约束与控件命名姿态 |
| [DESIGN.md](https://asdshuaishuai.github.io/StarTide/DESIGN/) | 最初的引擎设计：扫描 / store / 链接的取舍与调研结论 |
