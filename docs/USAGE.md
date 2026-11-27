---
layout: default
wrap: doc
title: 使用手册
---

# 使用手册

这份文档讲**怎么用**。它假设你已经读过 [README](https://github.com/asdshuaishuai/StarTide#readme)（项目主页）和
[CLI 参考](https://asdshuaishuai.github.io/StarTide/CLI/)（命令与选项的完整列表）。

- [1. 安装](#1-安装)
- [2. 第一次使用](#2-第一次使用)
- [3. 日常维护](#3-日常维护)
- [4. Skill 路由工作流](#4-skill-路由工作流)
- [5. 让 agent 用起来](#5-让-agent-用起来)
- [6. 桌面应用](#6-桌面应用)
- [7. 配置](#7-配置)
- [8. 排错](#8-排错)
- [9. 脚本化](#9-脚本化)
- [10. 命令速查](#10-命令速查)

---

## 1. 安装

### 1.1 下载预编译产物（最快）

每次推送都会在 Actions 上构建产物，打 `v*` 标签会发布成 Release：
<https://github.com/asdshuaishuai/StarTide/releases>

| 产物 | 平台 |
|---|---|
| `StarTide-<版本>-macos-arm64.zip` / `-macos-x86_64.zip` | 桌面客户端（**包里带引擎**，解压即用） |
| `skillctl-<版本>-macos-{arm64,x86_64}.tar.gz` | macOS 命令行 |
| `skillctl-<版本>-linux-{x86_64,aarch64}.tar.gz` | Linux 命令行 |
| `skillctl-<版本>-windows-x86_64.zip` | Windows 命令行 |

客户端只有 macOS 版：GUI 工具链（GPUI）还没有 Windows / Linux 后端。

### 1.2 从源码构建 CLI

```bash
git clone https://github.com/asdshuaishuai/StarTide.git
cd StarTide
make                       # 产物：build/skillctl
install -m 755 build/skillctl /usr/local/bin/skillctl
```

`make` 需要 `cc` 和 `make`。**引擎本身零外部依赖**——ONNX Runtime 与 llama.cpp
都是可选的，缺了只影响向量路（见 [README](https://github.com/asdshuaishuai/StarTide#readme#可选依赖只影响向量路)）。

Windows：需要 MSVC 或 mingw-w64；源码用 `sh_fs_win32.c` 走 Win32 API。

版本号在构建时打进去：`make VERSION=0.2.0`。

### 1.3 构建桌面应用（可选，macOS）

```bash
make app                   # 组装 ./StarTide.app（引擎随包）
open StarTide.app
# 或者一步装好：
make install               # → /Applications/StarTide.app + PATH 上的 skillctl
```

桌面应用是 CLI 的壳：它 spawn `skillctl` 并解析 `--json`。找引擎的顺序是
**随包的 `Contents/MacOS/skillctl`** → `SKILLCTL_BIN` → 从可执行文件位置向上
最多 8 级目录找 `build/skillctl`。所以下载来的 `.app` 解压即用，不依赖系统里
有没有装 CLI；找不到引擎时它会明确提示。

### 1.4 装一个向量模型（可选）

```bash
mkdir -p ~/.skillctl/models/f2llm-80m-gguf
curl -L -o ~/.skillctl/models/f2llm-80m-gguf/F2LLM-v2-80M.Q8_0.gguf \
  https://hf-mirror.com/mradermacher/F2LLM-v2-80M-GGUF/resolve/main/F2LLM-v2-80M.Q8_0.gguf

skillctl doctor            # 应显示：Vector path  gguf (model …)
skillctl embed             # 生成 index/vectors.bin
```

> 向量路**默认不参与** `find`（实测在本机语料上它让 Hit@1 从 0.912 降到 0.825，
> 原因见 README）。装模型是为了 `skillctl embed` 和 `--mode hybrid` 可用，
> 不是为了让日常路由变好。

---

## 2. 第一次使用

**先扫、再导、再链**。每一步都可以先 `--dry-run` 预览。

### Step 0 — 自检

```bash
skillctl doctor
```

```
Store          /Users/you/.skillctl
Symlinks       supported
Agents         10 detected, 10 with an existing skills directory
Vector path    gguf (model feae1d87cb9982fe)
Vectors        766 x 320, match this model
  ZCode            present  /Users/you/.zcode/skills
  ...
Everything looks fine.
```

读法：

| 输出 | 含义 |
|---|---|
| `Symlinks supported` | 这台机器可以用软链方案。不支持就说明平台/文件系统不允许 |
| `Agents N detected` | 自动发现了几个 agent 的 skills 根。少一个就检查 `~/.xxx/skills` 是否存在 |
| `Vector path` | 向量路是否就绪。`none` 表示没模型或没编译进后端，**不影响词面路由** |
| `Vectors … match this model` | 已有向量与当前模型指纹一致。不一致说明换过模型，需要重跑 `embed` |

### Step 1 — 扫描（只读）

```bash
skillctl scan
```

```
Scanning 1 root(s)...
  /Users/you

  3145 skill directories  (986 unique by content)
  416 skill roots, 338 symlinked aliases
  1465 read-only sources, 59746 directories visited

Skill roots:
  /Users/you/.zcode/skills                                      134
  /Users/you/.claude/skills                                      79
  ...
Run 'skillctl catalog' for the full list, or 'skillctl link' to unify.
```

三个数字的区别值得记住：

| 数字 | 含义 |
|---|---|
| skill directories | 磁盘上找到的 `SKILL.md` 目录总数（含副本） |
| unique by content | 按内容哈希去重后的**真实** skill 数 |
| symlinked aliases | 指向别处的软链条目（不展开，避免拖进几百 MB） |

`scan` **只读**：不复制、不移动、不删除任何 skill。它只把发现结果写进
`catalog.jsonl`（store 内）。

想扫某个额外目录而不改配置：

```bash
skillctl scan --root /path/to/project/skills
```

### Step 2 — 导入（复制进 store）

```bash
skillctl import --dry-run     # 先看会复制多少
skillctl import
```

```
Scanning for skills...

Would import 538 skills (2607 already in the store, 0 skipped), 109.5 MB
Store: /Users/you/.skillctl
```

导入按**内容哈希**去重：同一份内容出现在五个地方，只存一份。所以导入后的
单元数通常明显少于扫描数（上面 3145 个目录 → 986 个唯一内容）。

> `import` 会**复制**源目录到 `~/.skillctl/units/<id>/`。源目录原地不动，
> 插件缓存也永远只读。

### Step 3 — 链接（先预览）

```bash
skillctl link --dry-run
skillctl link --dry-run --json | head -c 400
```

```json
{"command":"link","dry_run":true,"imported":538,"skills":986,
 "agents":[{"id":"zcode","root":"/Users/you/.zcode/skills",
            "view":"/Users/you/.skillctl/views/zcode",
            "action":"linked","links":15,"backup":""}]}
```

`action` 有三种：

| action | 含义 |
|---|---|
| `linked` | 这次会（已）建立 view 并把根替换成软链 |
| `skipped` | 已经是我们建的软链，或不需要动 |
| `failed` | 失败，错误在 stderr 里 |

### Step 4 — 真正链接

```bash
skillctl link
```

发生的事（每一步都可回滚）：

1. 根目录若是真实目录 → **重命名**进 `~/.skillctl/trash/<agent>-<时间戳>/`
2. 建 `~/.skillctl/views/<agent>/`，里面每条是指向 `units/<id>` 的软链
3. 根目录本身变成指向 view 的软链

```
~/.zcode/skills  ->  ../.skillctl/views/zcode
```

**原目录永远不删，只 `rename` 进 trash。**

### Step 5 — 验证

```bash
skillctl status
```

```
Store   /Users/you/.skillctl
  766 units, 988.6 MB, catalog holds 986 entries (766 stored)

Agents
  ZCode            directory /Users/you/.zcode/skills                          401 links
  ...
```

三个数分别看：**units** 是 store 里真实存着的单元数，**catalog holds N entries**
是扫描发现过的技能数（986），**N stored** 是其中已经复制进 store 的（766）——
差值就是"磁盘上有、但还没入库"的部分，跑一次 `import` 或 `sync` 就会补齐。

`directory` 而不是 `symlink`？说明链接这一步还没做（或被别的工具改回去了）。

### 回滚

```bash
skillctl unlink --dry-run     # 打印将要恢复什么
skillctl unlink
```

```
  zcode        would restore /Users/you/.zcode/skills
  claude       would restore /Users/you/.claude/skills
```

原目录从 trash 原样移回，软链与 view 删掉。**这一步是原子的**——中途失败
不会留下半截状态。

### 一次跑完

```bash
skillctl scan && skillctl import && skillctl link
```

---

## 3. 日常维护

| 场景 | 命令 | 说明 |
|---|---|---|
| 装了新 skill | `skillctl sync` | 扫描 + 导入新出现的 + 刷新已链接 agent 的 view |
| 某个第三方工具往 view 里写了目录 | `skillctl sync` | 同样的流程会把它收编成软链 |
| 想看机器上还剩多少孤儿 | `skillctl gc --dry-run` | 预览要清掉的无引用单元 |
| 真的要清 | `skillctl gc` | 删无引用 store 单元（**会真删**） |
| 看看现状 | `skillctl status` | 容量、单元数、各 agent 链接状态、漂移、孤儿 |
| 列出技能 | `skillctl catalog --limit 50` | 名字、描述、大小、来源数、可见 agent |
| 按名字过滤 | `skillctl catalog --filter godot` | |
| 环境体检 | `skillctl doctor` | |

`sync` 与 `import` 的区别：**`sync` 只动已经链接过的 agent**（没链接的会跳过），
`import` 只入库不碰 view。日常用 `sync` 就够。

`gc` 之前务必先 `--dry-run`：

```
Would remove 184 unreferenced store unit(s), 932.9 MB
```

---

## 4. Skill 路由工作流

核心只有两步：**先 `find` 选，再 `show` 读**。

```bash
skillctl find "build a godot UI with shaders"
```

```
godot-ui [stored] — Use when building user interfaces — Control nodes, themes, anchors…
godot-ui-designer [stored] — Design Godot UI layouts…
...
```

每行：`名字 [是否已入库] — 描述摘要`。默认给前 8 条。

```bash
skillctl find --json --limit 5 "deploy macos app"
```

```json
[{"name":"app-store-changelog","description":"…","id":"…","stored":true,
  "path":"/Users/you/.zcode/skills/app-store-changelog","variants":1}]
```

然后按需取回：

```bash
skillctl show godot-ui               # 打印 SKILL.md 全文
skillctl show godot-ui --meta        # 只要 name/description/path，不取正文
skillctl show godot-ui --json        # 机器可读
```

`show` 接受**名字、唯一前缀、或 unit id**。

### 同名怎么办

机器上同名不同内容的 skill 很常见。`show` **不会**静默取第一条：

```bash
skillctl show export-pipeline
```

```
skillctl: "export-pipeline" is ambiguous (2 candidates):
  export-pipeline  (b5cf5b905e106bb1)  Use when exporting and distributing Godot games — export presets, platform settings, CI/...
  export-pipeline  (6dae4f79526cf5f6)  Production export from Blender to FBX, GLTF, OBJ, USD, and Alembic with scale, pivot, rota...
pass the exact name or a unit id to disambiguate
```

退出码是 **2**。按 id 取：

```bash
skillctl show 6dae4f79
```

`find` **也不会**按名字合并结果——剩下的同名条目是真正不同的内容，需要并列着
选。

### 模式

| 模式 | 组成 | 何时用 |
|---|---|---|
| `auto`（默认） | BM25F 正文 + 元数据启发式，RRF 融合 | **日常就用这个** |
| `bm25` | 只走词面 | 想排除元数据信号的干扰时 |
| `hybrid` | 加上稠密向量 | 装了模型，且想在自己的语料上试 |
| `vector` | 只走稠密向量 | 调试向量路 |

```bash
skillctl find --mode bm25 "exact wording I expect in the body"
skillctl find --mode hybrid "flutter 状态管理"
```

**先量再开**。换机器或换语料后，用评测脚本对照：

```bash
bash tests/router_eval.sh                    # 默认模式
ROUTER_MODE=hybrid bash tests/router_eval.sh # 对照组
```

### 命中不对时

```bash
skillctl find --explain "godot 状态机 敌人 巡逻"
```

```
state-machine [stored] — …  [rrf 0.030 rr 0.000 bm25 #1 meta #13 vec #0 name #0]
```

`bm25 #n` / `meta #n` / `vec #n` / `name #n` 是**每一路各自的排名**。哪一路
把它排到后面，哪一路就是拖累项——`meta` 低说明描述里没写清楚，`bm25` 低说明
正文里没这个词。

### 索引

索引是**过期也能用**的：catalog 变了之后 `find` 依然给出结果，只是可能漏掉
最新的 skill。刷新：

```bash
skillctl index              # 增量
skillctl index --rebuild    # 全量重建
skillctl index --if-stale   # 只有 catalog 变过才重建
```

`index` 只写 `~/.skillctl/index/`，**不碰任何 skill 目录**。

---

## 5. 让 agent 用起来

给 agent 装上路由 skill，它就知道该用 `find` 而不是把描述全读一遍：

```bash
skillctl router-skill --install   # 写入 ~/.agents/skills/skill-router/
```

或者先看一眼内容：

```bash
skillctl router-skill
```

这个 skill 里的强制规则是：

> 必须先 `find` 再 `show`。**禁止**自己遍历 skill 目录、禁止直接读某个
> `SKILL.md` 路径、禁止手工列 skill 根目录。那些路径是 store 的实现细节，
> 绕过索引会让选择不可复现。

装完之后，agent 的调用长这样：

```
$ skillctl find "godot 状态机 敌人 巡逻"
$ skillctl show state-machine
```

`--install` 优先写进**共享的** `~/.agents/skills/skill-router/`（所有以它为根的
agent 都能看到）；没有这个 agent 根时退回到配置里的第一个 agent 根。写入后
会打印完整路径：

```
installed router skill: /Users/you/.agents/skills/skill-router/SKILL.md
```

---

## 6. 桌面应用

```bash
make app && open StarTide.app
```

### 界面

三栏：

- **左**：agent 列表（首字母图标章 + skill 计数 + 归属点阵）
- **中**：卡片墙 / 紧凑列表（工具栏两个图标钮切换），上面是搜索框和
  「全部 / 已入库 / 仅目录」分段控件
- **右**：Inspector——选中 skill 的描述、元信息、可见 agent、来源位置、
  仓库位置、文件清单（深度 ≤2，最多 50 条）

### 标题栏动作

| 图标 | 动作 |
|---|---|
| ☀☾ | 深色 / 浅色 |
| 🎨 | 强调色（青 / 蓝 / 紫 / 琥珀） |
| Layers | 设计语言：Studio → 极简 → 粗野 → 拟物 |
| 硬盘 | 扫描本机 |
| 刷新 | 同步 |
| 网络 | 链接（**预览**，不真的执行） |
| 垃圾桶 | 清理（**预览**） |
| ↩ | 还原（两态确认，再点一次才真的 `unlink`） |

链接和清理在 GUI 里**只预览不执行**——真正改动目录结构的事留给 CLI，那里有
`--dry-run` 的默认预览和 trash 兜底。

### 快捷键

| 按键 | 作用 |
|------|------|
| `↑` / `↓`（或 `j` / `k`） | 移动选择 |
| `↩` | 打开 / 关闭详情 |
| `/` 或 `⌘K`（Ctrl+K） | 聚焦搜索框 |
| `esc` | 清空过滤 / 取消选中 |

### 设计语言

四套风格共用同一份渲染代码，差别只是 `Palette` 里的姿态 token。切换写入
`~/.skillctl/gui-theme.json`：

```json
{"dark":true,"accent":0,"style":"Minimal"}
```

`style` 缺省是 `Studio`（老配置文件没有这个字段时按默认处理）。每套风格的
约束与反漂移说明见 [THEMES.md](https://asdshuaishuai.github.io/StarTide/THEMES/)。

---

## 7. 配置

`~/.skillctl/config.json`：

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

没有配置文件时用内置默认值，agent 根按 `~/.<名字>/skills` 自动发现。

### 作用域

默认 `own`：每个 agent 的 view 只含它自己家目录下的 skill（含插件缓存）加上
共享的 `~/.agents/skills`。`--scope all` 让每个 agent 看到磁盘上全部 skill。

```bash
skillctl link --scope all
```

同名不同内容的 skill 在同一个 view 里同时存在，主版本占原名，其余加
`__<hash8>` 后缀。

### 环境变量

| 变量 | 作用 |
|---|---|
| `SKILLCTL_HOME` | store 位置，默认 `~/.skillctl` |
| `SKILLCTL_BIN` | GUI/脚本定位引擎二进制 |
| `SKILLCTL_GGUF_MODEL` | GGUF 模型文件或目录 |
| `SKILLCTL_ONNX_MODEL` | ONNX 模型目录（含 `vocab.txt` 或 `tokenizer.json`） |
| `SKILLCTL_EMBEDDER` | 外部嵌入命令（读 stdin、输出 JSON 数组） |

---

## 8. 排错

| 现象 | 原因 | 处理 |
|---|---|---|
| `find` 说没有匹配 | 索引太旧，或查询词和正文用词不同 | `skillctl index`；用领域词重述（引擎名、语言、产物名） |
| `find` 命中但 `show` 说读不到 | 该 skill 只在磁盘上、没入库 | `skillctl import` 或 `skillctl sync` |
| `show` 退出码 2 | 同名歧义 | 按提示用 id：`skillctl show 6dae4f79` |
| `doctor` 说 `Vectors … do not match` | 换过模型 | `skillctl embed` 重跑 |
| `doctor` 说 `Vector path none` | 没模型，或编译时缺 llama.cpp / ONNX Runtime | 装模型 + 装依赖；词面路由不受影响 |
| `find` 报 `vectors were built by … run 'skillctl embed'` | 同上 | `skillctl embed` |
| 某个 agent 不在 `doctor` 列表 | 没有 `~/.xxx/skills` 目录 | 装那个 agent，或在 `config.json` 里手写 `agents` |
| `link` 报权限错误 | Windows 上目录软链需要特权 | 自动降级为 junction；仍失败就只给该 agent 用 `own` 视图 |
| GUI 提示找不到 CLI | 二进制不在预期位置 | `SKILLCTL_BIN=/abs/path/skillctl` 启动，或 `make` |
| 磁盘占用涨得快 | store 里堆了不再被引用的单元 | `skillctl gc --dry-run` 看一眼，再 `skillctl gc` |
| `scan` 报权限跳过 | 目录不可读 | 正常行为，跳过并计数；不会提权重试 |

诊断细节：

```bash
skillctl doctor --json
skillctl status --json
skillctl find --explain "你的查询"
```

---

## 9. 脚本化

所有命令支持 `--json`。GUI 就是这么消费 CLI 的。

```bash
# 找出所有 godot 相关 skill 的名字
skillctl find --json --limit 20 "godot" | jq -r '.[].name'

# 拿到某个 skill 的正文（幂等）
skillctl show godot-ui --json | jq -r '.body // .content // empty'
```

退出码约定：

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 运行失败（I/O、缺模型、catalog 为空） |
| 2 | 用法错误，或 `show` 遇到同名歧义 |

`--json` 的各命令输出形状见 [CLI.md](https://asdshuaishuai.github.io/StarTide/CLI/#--json-输出形状)。

---

## 10. 命令速查

```
scan       只读扫描，找出所有 SKILL.md 并记进 catalog
catalog    列出已知技能
import     把发现的技能复制进 store（内容去重）
index      建 BM25F 倒排索引（路由用）
embed      建向量索引（可选路）
link       建 view + 把 agent 根换成软链
unlink     恢复 agent 原始目录
sync       增量：吸收新 skill + 刷新已链接的 view
status     store 与各 agent 的状态
doctor     环境自检
gc         清理无引用的 store 单元（默认先预览）
find       按意图排序技能（路由）
show       取回某个技能的 SKILL.md（按需加载）
router-skill  打印 / 安装给 agent 用的路由 skill
```

破坏性命令（`import` `link` `unlink` `sync` `gc`）都支持 `--dry-run`。
`scan` 额外支持 `--stream`（逐行 JSON 进度事件）。
