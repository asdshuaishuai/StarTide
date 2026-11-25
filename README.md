# StarTide

StarTide 是这套工具的桌面应用；命令行引擎叫 `skillctl`。两者在同一个仓库里，
但 CLI 可以单独安装使用——它不依赖图形界面。

把散落在机器各处的 agent skill 统一收进一个内容寻址的仓库，然后让所有 agent
通过软链接访问同一份内容。

## 单独安装 CLI

```
make
install -m 755 build/skillctl /usr/local/bin/skillctl
```

Windows / Linux 均可从源码构建；GUI 目前是 macOS（GPUI）。

## 它解决什么问题

一台开发机上通常同时存在多个 AI agent（ZCode、Claude Code、Codex、Qwen、
Qoder……），每个都有自己的 skills 目录，此外插件缓存、项目内目录、第三方
商店还散落着大量副本。同名 skill 可能同时存在多个不同版本，同一个 skill
可能被复制了三四份。skillctl 把这一切收敛为：

```
~/.skillctl/
├── units/<sha256 前 16 位>/   真实内容，按内容去重
├── views/<agent>/             每个 agent 一个目录，里面全是软链接
├── trash/<agent>-<时间>/       被替换掉的原目录，随时可回滚
└── catalog.jsonl              已知 skill 的索引
```

各 agent 的 skills 目录被整体替换成一个指向对应 view 的软链接：

```
~/.zcode/skills  ->  ../.skillctl/views/zcode
~/.claude/skills ->  ../.skillctl/views/claude
```

agent 的加载器跟随软链接（已在 ZCode 的加载器源码中验证），所以对它们来
说一切照旧。

## 构建

```
make            # 产物在 build/skillctl
make test       # 核心单元测试
./tests/e2e.sh  # 端到端测试（在临时目录里跑完整流程）
make asan       # AddressSanitizer 构建二进制 build/skillctl_asan
make gui        # 编译 GUI（Rust / gpui-kit，产物 gui/target/release/startide）
make app        # 在仓库根目录组装 StarTide.app（二进制 + Info.plist + 图标）
make cross-win  # 交叉编译 Windows 引擎 build-win/skillctl.exe（需要 mingw-w64）
```

纯 C99，零外部依赖。Linux/macOS 直接可编；Windows 需要 `sh_fs_win32.c`
（见「跨平台」一节）。

## 使用

```
skillctl scan              # 只读：找出磁盘上所有 skill，写入 catalog
skillctl catalog           # 列出已知的 skill
skillctl import            # 把发现的 skill 复制进 store（内容去重）
skillctl link              # 建 view，并把各 agent 的 skills 目录替换成软链
skillctl unlink            # 从 trash 恢复原目录，删掉软链
skillctl sync              # 增量：吸收新出现的 skill，刷新各 view
skillctl status            # store 与各 agent 的状态
skillctl doctor            # 环境自检
skillctl gc --dry-run      # 预览将清理的无引用 store 单元
```

所有命令都支持 `--json`（机器可读输出，GUI 就是这么消费 CLI 的）；
`link`/`unlink`/`sync`/`gc`/`import` 支持 `--dry-run`；`scan` 额外支持
`--stream`（逐行 JSON 进度事件）。

第一次真实使用建议按顺序：

```
skillctl scan
skillctl import
skillctl link --dry-run    # 检查将要发生什么
skillctl link              # 真正替换
```

后悔了就 `skillctl unlink`，原目录从 trash 原样恢复。

### 作用域

默认 scope 是 `own`：每个 agent 的 view 只包含它自己家目录下的 skill
（含插件缓存）加上共享的 `~/.agents/skills`。`--scope all` 让每个 agent
看到磁盘上的全部 skill。同名不同内容的 skill 在同一个 view 里会同时存在，
主版本占用原名，其余加 `__<hash8>` 后缀。

### 安全性

- **权限边界**：skillctl 只在用户权限可访问的范围内工作，绝不尝试突破权限——
  - 默认扫描根只有用户主目录（`$HOME` / `%USERPROFILE%`），不碰系统路径
  - 遇到不可读的目录或文件（EACCES / EPERM / `ERROR_ACCESS_DENIED`）：扫描时静默跳过并计数，导入/复制时直接报错中断；绝不重试、绝不提权
  - 不使用任何提权手段（无 sudo、无管理员请求；Windows 上链接降级为 junction 正是因为 junction 不需要特权）
  - macOS 的 TCC 保护目录（邮件、照片、信息等）在默认排除表中
- 原目录永远是 `rename` 进 trash，绝不删除；`gc` 只删无引用的 store 单元，
  不加 `--dry-run` 会真的删除——执行前先跑 `skillctl gc --dry-run` 预览
- 插件缓存、应用自带 skill 包一律只读——内容被复制出来，原位不动
- 被别的工具管理的 store（如 `~/.cc-switch`）在保护名单里，不会被接管
- go module 缓存、site-packages、Flutter SDK 等上游副本目录默认不进扫描

### 配置

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


## Skill 路由（按需取用，省上下文）

把"所有 skill 的 description 都塞进 agent 上下文"换成"提问 → 命中 → 只读一个"：

```
skillctl index                                   # 建全文索引（body 参与检索）
skillctl find "build a godot UI with shaders"    # 意图 → 排序后的候选
skillctl find --json --limit 5 "deploy macos app"
skillctl find --explain "godot 状态机 敌人 巡逻"  # 每一路各自的排名
skillctl show godot-ui                           # 按需取回该 skill 的 SKILL.md
skillctl show godot-ui --meta                    # 只要元信息，不取正文
```

### 算法与依据

检索不是关键词匹配，而是一条按论文结论搭起来的流水线：

| 组件 | 做法 | 依据 |
|---|---|---|
| **正文进索引** | 索引五个字段：name / description / profile / body / path。`profile` 是从正文**确定性抽取**的（H2/H3 标题、含 "when to use/适用" 的行、代码围栏语言、references 文件名），不调用任何模型 | SkillRouter（arXiv 2603.22455）：去掉 skill 正文，Hit@1 掉 29–44pp；描述级富化只能补回一部分，LLM 富化在元数据够用时中性甚至负收益（Enrich-Retrieve-Rank 2608.22695） |
| **BM25F** | 字段权重 name 4.0 / desc 2.0 / profile 1.5 / body 1.0 / path 0.5；k1=1.2；每字段独立长度归一（b = 0.3/0.5/0.6/0.75/0.3）；IDF = ln(1+(N−df+0.5)/(df+0.5)) | Robertson & Zaragoza 2009；BM25F: Robertson/Zaragoza/Taylor, CIKM 2004 |
| **中文 bigram** | CJK 连续段发重叠 bigram，ASCII 按 `_ - . / :` 切分，中英停用词表；查询与文档用同一套分词 | 无分词器 CJK 检索的经典做法 |
| **OOV 术语扩展** | 只对**索引里不存在的**中文词做中英对照翻译后追加；索引里已有的词一律原样使用 | Manning/Raghavan/Schuetze, IIR ch.9 thesaurus expansion；实测无条件扩展会让常见译词（state/test）压过真实词面命中，故只补 OOV |
| **索引期去重** | 建索引时按归一化名字分组，正文 5-gram shingle Jaccard ≥ 0.8 的变体合并为一个文档并记 `variants` 数 | SkillRouter 的三层假负例过滤（同名/trigram Jaccard/向量余弦）单独贡献 +4.0pp |
| **多路召回 + RRF** | 三路各取 top-100：① BM25F 正文 ② 元数据启发式（名字精确/子串、描述、路径）③ 稠密向量（配置了 embedder 才有）；用 RRF k=60 融合，因为 BM25 与余弦分数不可比 | ToolRet（ACL 2025）：query/tool 词面重叠仅 ROUGE-L 0.06，单路不够；Cormack/Clarke/Buettcher, SIGIR 2009（RRF 无需分数归一）；Lost in Implicit Routing（2610.18522）：1000 个干扰技能让已加载技能召回从 73.6% 掉到 27.4%，hybrid 预选在大规模最强 |
| **名字精确 boost** | 仅当**整条查询**归一化后等于某个技能名时加分。前后缀 boost 试过并已移除：`blender ...` 这类查询会被同名技能顶到第一，压过内容更匹配的结果 | 论文观测到名字在浅层注意力里占 26%，是早期对齐信号；但只应作用于真正的精确匹配 |
| **精排（预留）** | top-20 交给可插拔 listwise 精排（默认关闭） | SkillRouter：K=50 反而 −2.0pp；pointwise 重排崩到 0.433，必须 listwise |

规模上的意义：in-context 路由的拐点约在 N≈500（Enrich-Retrieve-Rank 实测 0.85→0.12），这台机器 766 个技能已经越过拐点；Progressive Disclosure 在 Skill-Use 上正确激活率只有 0.11%（Gavel, 2609.15982）。一层渐进披露足够，第二层更深的路由从不帮忙（REALM 2026, 2607.17598）——所以这里只有 `find` → `show` 两步。

### 向量路（进程内，无 Python）

两条后端，都是 C 进程内、无子进程、无 Python：

| 后端 | 运行时 | 分词器 | 适合 |
|---|---|---|---|
| **GGUF** | llama.cpp（C 库） | 模型自带（byte-level BPE） | **decoder 系嵌入模型**：Qwen3-Embedding / F2LLM-v2 |
| ONNX | ONNX Runtime（C 库） | `vocab.txt`→WordPiece；`tokenizer.json`→Unigram | BERT 系（bge、XLM-R、E5） |

```
# 当前随装的模型（GGUF，llama.cpp 后端）
ls ~/.skillctl/models/f2llm-80m-gguf/      # F2LLM-v2-80M.Q8_0.gguf  87MB
skillctl embed                              # 生成 index/vectors.bin
skillctl find --mode hybrid "flutter 状态管理"
SKILLCTL_GGUF_MODEL=/path/to/model.gguf     # 换任意 GGUF 嵌入模型
SKILLCTL_ONNX_MODEL=/path/to/dir            # 换 ONNX 模型（含 vocab.txt 或 tokenizer.json）
```

模型完全可替换：GGUF 由 llama.cpp 加载，ONNX 由形状推断，分词器按目录里存在
`vocab.txt`（WordPiece）还是 `tokenizer.json`（Unigram）自动选择。

**当前模型：F2LLM-v2-80M（Q8_0 GGUF，87MB）**

- Qwen3 架构、80M 参数、320 维、**last-token 池化**、200+ 语言、无投影层
- 量化后 87MB，满足"<100MB + 真多语言"这两个此前互斥的条件
- 实测编码 320 维向量，跨语言判别正常：`flutter 状态管理` ↔ 英文
  `Manages application and ephemeral state…` 余弦 **0.629**，与不相关英文 **0.168**

**为什么是 decoder 模型而不是 BERT**：先试过 BERT 系 GGUF（multilingual-e5-small
的多个转换版），llama.cpp 1.0.0 下要么拒绝加载（缺 `bert.token_type_count`），要么
加载成功但产出退化向量——不相关文本对余弦 0.953 反而高于相关对 0.934。逐 token
embedding 也拿不到（全零）。Qwen3 这类 decoder 的 last-token 池化才是 llama.cpp
验证充分的路径，换成 F2LLM 后判别力立即正常。**这条经验写在这里，避免以后再走一遍。**

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

**已实现但未启用**：`--mode vector` 单路、`--explain` 显示 `vec #n` 排名、
`SKILLCTL_NO_ONNX` / `SKILLCTL_NO_LLAMA` 编译期降级（缺库时整条路消失，`find` 不受影响）。

### 行为约定

- `find` 默认输出前 8 条、每条一行；`--mode` 可选 `auto|bm25|vector|hybrid`（auto 默认只用词面+元数据两路；稠密路需显式 hybrid/vector，理由见上节实测）；`--explain` 打印每一路的排名（`[rrf … bm25 #n meta #n vec #n name #n]`）
- `show` 接受名字、唯一前缀或 unit id；**同名多条会列候选并以退出码 2 报歧义**（同名不同内容在这类机器上很常见，静默取第一条是错误行为）
- `find` **不按名字合并**结果：索引期已经合并了正文近重复的变体，剩下的同名条目是真正不同的内容，调用方需要两条并列才能按 id 选一个
- `find`/`show` 只读，不触碰磁盘上的 skill；`index` 只写 store 下的 `index/`

### 评测

```
bash tests/router_eval.sh                 # 80 条查询，输出 Hit@1/MRR@10/Recall@3/@10/nDCG@10
bash tests/router_eval.sh --bin <binary>  # 对比别的版本
```

查询集分六层（精确名 / 中文意图 / 英文自然语言 / 中英混合 / **仅正文才有的信号** / 同名消歧），
期望值支持 `name@hash8` 形式以指定同名中的具体变体。当前结果：

| 指标 | 改造前（启发式） | 改造后 |
|---|---|---|
| Hit@1 | 0.775 | **0.900** |
| MRR@10 | 0.792 | **0.927** |
| Recall@3 | 0.812 | **0.950** |
| Recall@10 | 0.825 | **0.988** |
| nDCG@10 | 0.800 | **0.942** |
| 仅正文信号层 Hit@1 | 0.000 | **0.833** |

配合同仓库的 `skills/skill-router/SKILL.md`（也可 `skillctl router-skill` 直接打印）：

```
skillctl router-skill --install   # 写进共享的 ~/.agents/skills/skill-router/
```

这个 skill 里写死了强制规则：**必须先 `find` 再 `show`，禁止自己遍历 skill 目录或直接读 SKILL.md 路径**——那些路径是 store 的实现细节，绕过索引会让选择不可复现。

## 设计

见 [docs/DESIGN.md](docs/DESIGN.md)，包括扫描可达性规则、内容寻址、
软链投影与回滚的完整说明。

## 跨平台

核心以 `sh_fs.h` 为边界做了平台抽象：POSIX 后端（`sh_fs_posix.c`）已实现；
Windows 后端（`FindFirstFileW` / `CreateSymbolicLinkW` / `MoveFileExW`，
目录链接失败时降级为 junction）是同一个接口的另一份实现，放进 `core/`
即可参与构建。

## GUI

`gui/` 下是一个 GPUI（Rust）壳：它不重新实现任何逻辑，只 spawn CLI 并解析
其 `--json` 输出（CLI 的 `--stream` 进度事件 GUI 目前未使用）。

界面是三栏布局：左侧 agent 树（每个 agent 一个首字母图标章 + skill 计数），
中间是卡片墙或紧凑列表（工具栏两个图标钮切换视图），右侧是选中 skill 的
详情面板。详情面板包含描述、元信息（大小 / 哈希 / 副本数）、可见 agent、
来源位置、仓库位置，以及「文件清单」一节：展示该 skill 目录两层（深度
≤2）的内容，最多 50 条，等宽字体，目录名带 `/` 后缀，二级项缩进。

卡片视图会为每张卡尝试加载封面图：在该 skill 目录内按 `cover.*` >
`preview.*` > `icon.*` 的优先级寻找（跳过点目录与点文件，单个文件超过
2MB 的不用），找到则显示圆角小图；找不到或加载失败时回退到首字母章。
列表视图不加载封面。

键盘导航：

| 按键 | 作用 |
|------|------|
| `↑` / `↓`（或 `j` / `k`） | 移动选择 |
| `↩` | 打开或关闭详情面板 |
| `/` 或 `⌘K`（Ctrl+K） | 聚焦过滤框 |
| `esc` | 清空过滤，或取消选中 |

「还原」按钮是两态确认：第一次点击只进入确认态（按钮变红并提示"再点一
次"），再点一次才真正执行 `skillctl unlink` 恢复所有 agent 的原始目录。

构建与运行：

```
make gui        # 产物 gui/target/release/startide，直接运行
make app        # 在仓库根目录组装 StarTide.app，双击启动
```

GUI 启动时按 `SKILLCTL_BIN` 环境变量 → 从可执行文件位置向上最多 8 级目录
寻找 `build/skillctl`（Windows 为 `build-win/skillctl.exe`）的顺序定位
CLI 引擎；找不到会提示先运行 `make` 构建，或用 `SKILLCTL_BIN` 指定路径。
