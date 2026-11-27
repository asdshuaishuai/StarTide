# StarTide

**把散落在机器各处的 agent skill，收进一个仓库；需要哪一个，才取哪一个。**

StarTide 是一套跨平台的 skill 管理工具，由三部分组成：

| 组成 | 是什么 | 能不能单独用 |
|---|---|---|
| **core** | 纯 C99 引擎：扫描、内容寻址入库、索引、检索。零外部依赖 | —— |
| **skillctl** | 引擎的命令行。人、脚本、agent 都用它 | ✅ 跨平台 |
| **StarTide.app** | macOS 桌面应用（Rust / GPUI）。不重写任何逻辑，只调用 CLI | macOS |

仓库地址：<https://github.com/asdshuaishuai/StarTide>
官网：<https://asdshuaishuai.github.io/StarTide/>

---

## 它解决什么问题

一台开发机上通常同时存在多个 AI agent（ZCode、Claude Code、Codex、Qwen、
Qoder……），每个都有自己的 skills 目录，此外插件缓存、项目内目录、第三方
商店还散落着大量副本。同名 skill 可能同时存在多个不同版本，同一个 skill
可能被复制了三四份。

更贵的是**上下文**：agent 的标准做法是把每个 skill 的 `name` + `description`
全塞进系统提示。机器上 766 个 skill 塞进去既慢又挤，而且 agent 真正需要
时往往还是猜错了那个。

StarTide 把这两件事分开处理：

```
~/.skillctl/
├── units/<sha256 前 16 位>/   真实内容，按内容去重
├── views/<agent>/             每个 agent 一个目录，里面全是软链接
├── trash/<agent>-<时间>/       被替换掉的原目录，随时可回滚
├── index/                     BM25F 倒排索引 + 向量
└── catalog.jsonl              已知 skill 的索引
```

各 agent 的 skills 目录被整体替换成一个指向对应 view 的软链接：

```
~/.zcode/skills  ->  ../.skillctl/views/zcode
~/.claude/skills ->  ../.skillctl/views/claude
```

agent 的加载器跟随软链接（已在 ZCode 的加载器源码中验证），所以对它们来
说一切照旧。

而**选择**这件事交给了检索：

```
skillctl find "build a godot UI with shaders"   # 意图 → 排序后的候选
skillctl show godot-ui                          # 按需取回那一个 SKILL.md
```

两步，只读一个 skill 的正文。

---

## 五分钟上手

```bash
make                                   # 产出 build/skillctl
install -m 755 build/skillctl /usr/local/bin/skillctl

skillctl scan                          # 只读：找出磁盘上所有 skill
skillctl import                        # 复制进 store（按内容去重）
skillctl link --dry-run                # 看看将要发生什么
skillctl link                          # 真正替换成软链
skillctl index                         # 建全文索引（路由需要）

skillctl find "godot 状态机 敌人 巡逻"   # 路由
skillctl show state-machine             # 取回正文
```

后悔了随时 `skillctl unlink`，原目录从 trash 原样恢复。

安装桌面应用：

```bash
make app && open StarTide.app          # 或 make install 直接装进 /Applications
```

---

## 完整安装

### 直接下载

每次推送都会在 Actions 上构建产物；打标签会发布成 Release。当前是**阿尔法测试版，
没有正式版本号**：

**→ [StarTide · alpha 2026-11-28](https://github.com/asdshuaishuai/StarTide/releases/tag/alpha-2026-11-28)（预发布）**

| 产物 | 平台 | 说明 |
|---|---|---|
| `StarTide-alpha-*-macos-arm64.zip` | macOS Apple Silicon | 桌面客户端，**包里带引擎** |
| `StarTide-alpha-*-macos-x86_64.zip` | macOS Intel | 桌面客户端，**包里带引擎** |
| `StarTide-alpha-*-windows-x86_64.zip` | Windows x86_64 | 桌面客户端 + 引擎 |
| `skillctl-alpha-*-macos-arm64.tar.gz` | macOS Apple Silicon | 命令行 |
| `skillctl-alpha-*-macos-x86_64.tar.gz` | macOS Intel | 命令行 |
| `skillctl-alpha-*-linux-x86_64.tar.gz` | Linux x86_64 | 命令行 |
| `skillctl-alpha-*-linux-aarch64.tar.gz` | Linux ARM64 | 命令行 |
| `skillctl-alpha-*-windows-x86_64.zip` | Windows x86_64 | 命令行 |

下载页：<https://github.com/asdshuaishuai/StarTide/releases>

> 发布版是**纯词面**构建（没有链接 llama.cpp / ONNX Runtime），所以
> `skillctl embed` 与向量路不可用——但 `find` 走的就是词面 + 元数据那条路，
> 评测上它本来就是最好的一条（Hit@1 0.912）。要向量路请自行编译引擎。

### 从源码构建

```
make            # 引擎：产物在 build/skillctl
make test       # 核心单元测试（130 项）
./tests/e2e.sh  # 端到端测试（44 项，在临时目录里跑完整流程）
make asan       # AddressSanitizer 构建 build/skillctl_asan
make gui        # 编译 GUI（Rust / gpui-kit，产物 gui/target/release/startide）
make app        # 在仓库根目录组装 StarTide.app（引擎随包）
make install    # 装进 /Applications 并把 CLI 放上 PATH
make cross-win  # 交叉编译 Windows 引擎（需要 mingw-w64）
```

版本号在构建时打进去：`make VERSION=0.2.0`，或
`cmake -B build -DSKILLCTL_VERSION=0.2.0`。

**引擎与 CLI 是纯 C99，源码层面零外部依赖。** Linux / macOS 直接 `make` 即可；
Windows 需要 `sh_fs_win32.c`（见「跨平台」一节）。

### 可选依赖（只影响向量路）

| 依赖 | 用在哪 | 缺了会怎样 |
|---|---|---|
| [ONNX Runtime](https://onnxruntime.ai)（`brew install onnxruntime`） | ONNX 后端（BERT 系模型） | 编译期自动降级（`SKILLCTL_NO_ONNX`），`find` 不受影响 |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) + `ggml`（`brew install llama.cpp`） | GGUF 后端（decoder 系模型） | 同上（`SKILLCTL_NO_LLAMA`） |

两条后端都是**进程内**调用，不需要 Python、不需要服务进程。缺库时对应代码编译成
空实现，词面检索照常工作——`doctor` 会如实报告当前哪条路可用。

### 装一个向量模型（可选）

默认模型是 F2LLM-v2-80M 的 Q8_0 GGUF（**87 MB**，200+ 语言，320 维）：

```bash
mkdir -p ~/.skillctl/models/f2llm-80m-gguf
curl -L -o ~/.skillctl/models/f2llm-80m-gguf/F2LLM-v2-80M.Q8_0.gguf \
  https://hf-mirror.com/mradermacher/F2LLM-v2-80M-GGUF/resolve/main/F2LLM-v2-80M.Q8_0.gguf
skillctl doctor        # 应显示 Vector path gguf (model …)
skillctl embed         # 生成 index/vectors.bin（带模型指纹）
```

换模型两种方式：`SKILLCTL_GGUF_MODEL=/path/to/model.gguf`（任意 GGUF 嵌入
模型），或 `SKILLCTL_ONNX_MODEL=/path/to/dir`（任意含 `vocab.txt` 或
`tokenizer.json` 的 ONNX 模型）。换完必须重跑 `skillctl embed`——`find` 会
比对向量里的模型指纹，不匹配就拒绝使用稠密路并提示你重建。

---

## Skill 路由（按需取用，省上下文）

这是引擎的核心能力：**意图 → 命中 → 只读一个**。

```bash
skillctl index                                   # 建全文索引（body 参与检索）
skillctl find "build a godot UI with shaders"    # 意图 → 排序后的候选
skillctl find --json --limit 5 "deploy macos app"
skillctl find --explain "godot 状态机 敌人 巡逻"  # 每一路各自的排名
skillctl show godot-ui                           # 按需取回该 skill 的 SKILL.md
skillctl show godot-ui --meta                    # 只要元信息，不取正文
```

### 它不是关键词匹配

检索是一条按论文结论搭起来的流水线，而不是对 description 做字符串匹配：

| 组件 | 做法 | 依据 |
|---|---|---|
| **正文进索引** | 索引五个字段：name / description / profile / body / path。`profile` 是从正文**确定性抽取**的（H2/H3 标题、含 "when to use/适用" 的行、代码围栏语言、references 文件名），不调用任何模型 | SkillRouter（arXiv 2603.22455）：去掉 skill 正文，Hit@1 掉 29–44pp；描述级富化只能补回一部分，LLM 富化在元数据够用时中性甚至负收益（Enrich-Retrieve-Rank 2608.22695） |
| **BM25F** | 字段权重 name 4.0 / desc 2.0 / profile 1.5 / body 1.0 / path 0.5；k1=1.2；每字段独立长度归一（b = 0.3/0.5/0.6/0.75/0.3）；IDF = ln(1+(N−df+0.5)/(df+0.5)) | Robertson & Zaragoza 2009；BM25F: Robertson/Zaragoza/Taylor, CIKM 2004 |
| **中文 bigram** | CJK 连续段发重叠 bigram，ASCII 按 `_ - . / :` 切分，中英停用词表；查询与文档用同一套分词 | 无分词器 CJK 检索的经典做法 |
| **OOV 术语扩展** | 只对**索引里不存在的**中文词做中英对照翻译后追加；索引里已有的词一律原样使用 | Manning/Raghavan/Schuetze, IIR ch.9 thesaurus expansion；实测无条件扩展会让常见译词（state/test）压过真实词面命中，故只补 OOV |
| **索引期去重** | 建索引时按归一化名字分组，正文 5-gram shingle Jaccard ≥ 0.8 的变体合并为一个文档并记 `variants` 数 | SkillRouter 的三层假负例过滤（同名/trigram Jaccard/向量余弦）单独贡献 +4.0pp |
| **多路召回 + RRF** | 三路各取 top-100：① BM25F 正文 ② 元数据启发式（名字精确/子串、描述、路径）③ 稠密向量（配置了 embedder 才有）；用 RRF k=60 融合，因为 BM25 与余弦分数不可比 | ToolRet（ACL 2025）：query/tool 词面重叠仅 ROUGE-L 0.06，单路不够；Cormack/Clarke/Buettcher, SIGIR 2009（RRF 无需分数归一）；Lost in Implicit Routing（2610.18522）：1000 个干扰技能让已加载技能召回从 73.6% 掉到 27.4%，hybrid 预选在大规模最强 |
| **名字精确 boost** | 仅当**整条查询**归一化后等于某个技能名时加分。前后缀 boost 试过并已移除：`blender ...` 这类查询会被同名技能顶到第一，压过内容更匹配的结果 | 论文观测到名字在浅层注意力里占 26%，是早期对齐信号；但只应作用于真正的精确匹配 |
| **重排阶段（默认关）** | `--rerank` 对融合后的 top-20 用**索引里的字段词频**重排（IDF 加权覆盖 + 名字/富化覆盖），无需模型；实测本语料 0.850 / 名字权重版 0.812，均低于不重排的 0.912，故默认关 | SkillRouter：K=50 反而 −2.0pp，pointwise 重排崩到 0.433，必须 listwise；机制已就位，等可用的 listwise 模型 |

规模上的意义：in-context 路由的拐点约在 N≈500（Enrich-Retrieve-Rank 实测
0.85→0.12），这台机器 766 个技能已经越过拐点；Progressive Disclosure 在
Skill-Use 上正确激活率只有 0.11%（Gavel, 2609.15982）。一层渐进披露足够，
第二层更深的路由从不帮忙（REALM 2026, 2607.17598）——所以这里只有
`find` → `show` 两步。

### 评测

80 条分层查询（精确名 / 中文意图 / 英文自然语言 / 中英混合 / 仅正文才有的
信号 / 同名消歧），`bash tests/router_eval.sh` 可复现：

| 指标 | 改造前（启发式） | 改造后 |
|---|---|---|
| Hit@1 | 0.775 | **0.912** |
| MRR@10 | 0.792 | **0.940** |
| Recall@3 | 0.812 | **0.963** |
| Recall@10 | 0.825 | **0.988** |
| nDCG@10 | 0.800 | **0.952** |
| 仅正文信号层 Hit@1 | 0.000 | **0.833** |

最后一栏是分水岭：那一层的查询信号只存在于 `SKILL.md` 正文里，名字和描述
里一个词都没有。改造前的实现对它们**完全无能为力**（0.000），现在能命中
83.3%。

### 向量路（进程内，无 Python）

两条后端，都是 C 进程内、无子进程、无 Python：

| 后端 | 运行时 | 分词器 | 适合 |
|---|---|---|---|
| **GGUF** | llama.cpp（C 库） | 模型自带（byte-level BPE） | **decoder 系嵌入模型**：Qwen3-Embedding / F2LLM-v2 |
| ONNX | ONNX Runtime（C 库） | `vocab.txt`→WordPiece；`tokenizer.json`→Unigram | BERT 系（bge、XLM-R、E5） |

当前随装 **F2LLM-v2-80M（Q8_0 GGUF，87MB）**：Qwen3 架构、80M 参数、320 维、
**last-token 池化**、200+ 语言、无投影层。实测跨语言判别正常：
`flutter 状态管理` ↔ 英文 `Manages application and ephemeral state…` 余弦
**0.629**，与不相关英文 **0.168**。

> **为什么是 decoder 模型而不是 BERT**：先试过 BERT 系 GGUF（multilingual-e5-small
> 的多个转换版），llama.cpp 1.0.0 下要么拒绝加载（缺 `bert.token_type_count`），
> 要么加载成功但产出退化向量——不相关文本对余弦 0.953 反而高于相关对 0.934。
> 逐 token embedding 也拿不到（全零）。Qwen3 这类 decoder 的 last-token 池化
> 才是 llama.cpp 验证充分的路径，换成 F2LLM 后判别力立即正常。**这条经验写在这里，
> 避免以后再走一遍。**

**稠密路为什么默认关闭（实测）**

80 条查询集上，向量只编码 `name + description + profile`（正文留给 BM25F，
避免"向量读不到正文深处却和正文检索竞争"），稠密路的 RRF 权重扫描：

| 稠密路权重 | Hit@1 | MRR@10 |
|---|---|---|
| 1.0（RRF 标准） | 0.825 | 0.887 |
| 0.5 | 0.762 | 0.858 |
| 0.25 | 0.775 | 0.865 |
| **不启用（`auto` 默认）** | **0.912** | **0.940** |

逐层看，稠密路让每一层都略降（A 1.000→0.944、C 1.000→0.933、E 0.833→0.500）。
原因是这个语料的技术标识符（`is_instance_valid`、`bpy.ops export_scene`）只有
词面检索能精确命中，而 320 维语义摘要无法表达它们；等权 RRF 会把这类精确命中挤到
第二位。

所以：**权重保持 RRF 的公布值 1.0，稠密路改为显式 `--mode hybrid` / `--mode vector`
才启用**。要在某台机器/某个语料上启用它，先用 `tests/router_eval.sh` 对照
`ROUTER_MODE=hybrid` 与默认值，别凭感觉开。

### 行为约定

- `find` 默认输出前 8 条、每条一行；`--rerank` 可显式开启 top-20 重排（默认关，
  理由见上表）；`--mode` 可选 `auto|bm25|vector|hybrid`；`--explain` 打印每一路的
  排名（`[rrf … bm25 #n meta #n vec #n name #n]`）
- `show` 接受名字、唯一前缀或 unit id；**同名多条会列候选并以退出码 2 报歧义**
  （同名不同内容在这类机器上很常见，静默取第一条是错误行为）
- `find` **不按名字合并**结果：索引期已经合并了正文近重复的变体，剩下的同名条目是
  真正不同的内容，调用方需要两条并列才能按 id 选一个
- `find`/`show` 只读，不触碰磁盘上的 skill；`index` 只写 store 下的 `index/`

### 给 agent 用

配合同仓库的 `skills/skill-router/SKILL.md`（也可 `skillctl router-skill` 直接
打印）：

```bash
skillctl router-skill --install   # 写进共享的 ~/.agents/skills/skill-router/
```

这个 skill 里写死了强制规则：**必须先 `find` 再 `show`，禁止自己遍历 skill 目录
或直接读 SKILL.md 路径**——那些路径是 store 的实现细节，绕过索引会让选择不可复现。

---

## GUI

`gui/` 下是一个 GPUI（Rust）壳：它不重新实现任何逻辑，只 spawn CLI 并解析
其 `--json` 输出。

界面是三栏布局：左侧 agent 树（每个 agent 一个首字母图标章 + skill 计数），
中间是卡片墙或紧凑列表，右侧是选中 skill 的详情面板（描述、元信息、可见 agent、
来源位置、仓库位置，以及深度 ≤2 的文件清单）。

标题栏的动作（全部可见，不藏进菜单）：

| 动作 | 行为 |
|---|---|
| 深色 / 浅色 | 切换配色 |
| 强调色 | 青 / 蓝 / 紫 / 琥珀 |
| 设计语言（Layers 图标） | Studio → 极简 → 粗野 → 拟物，循环 |
| 扫描本机 | `skillctl scan` |
| 同步 | `skillctl sync` |
| 链接（预览） | `skillctl link --dry-run` |
| 清理（预览） | `skillctl gc --dry-run` |
| 还原 | 两态确认，第二次点击才真的 `skillctl unlink` |

键盘导航：

| 按键 | 作用 |
|------|------|
| `↑` / `↓`（或 `j` / `k`） | 移动选择 |
| `↩` | 打开或关闭详情面板 |
| `/` 或 `⌘K`（Ctrl+K） | 聚焦过滤框 |
| `esc` | 清空过滤，或取消选中 |

GUI 启动时按 `SKILLCTL_BIN` 环境变量 → 从可执行文件位置向上最多 8 级目录
寻找 `build/skillctl`（Windows 为 `build-win/skillctl.exe`）的顺序定位
CLI 引擎；找不到会提示先运行 `make` 构建。

### 四套设计语言

客户端有四套可切换的设计语言，共用同一份渲染代码，差别来自 `Palette` 的姿态
token（圆角、描边、内距、标题字号、等宽、全大写、横线、投影、内阴影、页面渐变）：

| 风格 | 立场 |
|---|---|
| **Studio** | 默认：半透明面板叠在毛玻璃上，克制的中性色 + 品牌强调色 |
| **极简 Minimal** | 元素最少、内距 20px、近单色 + 单一强调色、30px 超大标题；装饰为零 |
| **粗野 Brutal** | 浏览器默认材料（等宽全大写、蓝链接）、1px 实线暴露结构、0 圆角零装饰、密度优先 |
| **拟物 Skeuo** | 可命名材质（纸面/桌面/石墨金属）+ 统一自上而下光照（凸起投影、凹陷内阴影） |

选择写入 `~/.skillctl/gui-theme.json`；深/浅与强调色仍独立可调。三套新风格
各自的约束、反漂移说明（极简 ≠ flat design、粗野 ≠ neobrutalism、拟物 ≠
neumorphism）与控件命名姿态见 **[docs/THEMES.md](docs/THEMES.md)**。

---

## 安全性

- **权限边界**：skillctl 只在用户权限可访问的范围内工作，绝不尝试突破权限
  - 默认扫描根只有用户主目录（`$HOME` / `%USERPROFILE%`），不碰系统路径
  - 遇到不可读的目录或文件（EACCES / EPERM / `ERROR_ACCESS_DENIED`）：扫描时静默跳过并计数，导入/复制时直接报错中断；绝不重试、绝不提权
  - 不使用任何提权手段（无 sudo、无管理员请求；Windows 上链接降级为 junction 正是因为 junction 不需要特权）
  - macOS 的 TCC 保护目录（邮件、照片、信息等）在默认排除表中
- 原目录永远是 `rename` 进 trash，绝不删除；`gc` 只删无引用的 store 单元，
  不加 `--dry-run` 会真的删除——执行前先跑 `skillctl gc --dry-run` 预览
- 插件缓存、应用自带 skill 包一律只读——内容被复制出来，原位不动
- 被别的工具管理的 store（如 `~/.cc-switch`）在保护名单里，不会被接管
- go module 缓存、site-packages、Flutter SDK 等上游副本目录默认不进扫描
- `find` / `show` 只读，永不写盘

### 作用域

默认 scope 是 `own`：每个 agent 的 view 只包含它自己家目录下的 skill（含插件
缓存）加上共享的 `~/.agents/skills`。`--scope all` 让每个 agent 看到磁盘上的
全部 skill。同名不同内容的 skill 在同一个 view 里会同时存在，主版本占用原名，
其余加 `__<hash8>` 后缀。

---

## 配置

`~/.skillctl/config.json`（环境变量 `SKILLCTL_HOME` 可改 store 位置）：

```json
{
  "version": 1,
  "scan": {
    "roots": ["~/"],
    "exclude": ["node_modules", ".git", "..."],
    "exclude_paths": ["/pkg/mod/", "/site-packages/", "..."],
    "max_depth": 16
  },
  "view": { "scope": "own" },
  "protected_agents": ["cc-switch"],
  "agents": [
    { "id": "zcode", "root": "~/.zcode/skills", "label": "ZCode" }
  ]
}
```

没有配置文件时使用内置默认值，agent 根目录按 `~/.<名字>/skills` 自动发现。

---

## 跨平台

核心以 `sh_fs.h` 为边界做了平台抽象：POSIX 后端（`sh_fs_posix.c`）已实现；
Windows 后端（`FindFirstFileW` / `CreateSymbolicLinkW` / `MoveFileExW`，
目录链接失败时降级为 junction）是同一个接口的另一份实现，放进 `core/`
即可参与构建。CI 在 Linux / macOS / Windows 三平台编译并跑测试。

---

## 文档

| 文档 | 内容 |
|---|---|
| **[docs/USAGE.md](docs/USAGE.md)** | **使用手册**：安装、上手、日常操作、路由工作流、GUI、排错 |
| [docs/CLI.md](docs/CLI.md) | 命令、全局选项、环境变量、`--json` 形状、退出码、store 布局 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 模块职责、三条数据流、七条不变量、测试分层 |
| [docs/THEMES.md](docs/THEMES.md) | 四套设计语言的 token、约束与控件命名姿态 |
| [docs/DESIGN.md](docs/DESIGN.md) | 最初的引擎设计（扫描 / store / 链接的取舍与调研结论） |

`docs/` 目录同时是 GitHub Pages 的站点源：`main` 分支的 `/docs` 直接发布成
官网 <https://asdshuaishuai.github.io/StarTide/>，上面的文档同时是站点页面。

> **发布官网**：GitHub Pages 免费版不支持私有仓库，所以要先让仓库可见，
> 再打开 Pages：
>
> ```bash
> gh repo edit asdshuaishuai/StarTide --visibility public
> gh api --method POST repos/asdshuaishuai/StarTide/pages \
>   --input - <<< '{"source":{"branch":"main","path":"/docs"}}'
> ```
>
> 之后每次 push 都会自动重新构建站点。

## 许可

源码见仓库。第三方资产各自遵循其许可：模型遵循发布页（CC-BY / Apache-2.0
等），检索算法实现依据见上表论文。
