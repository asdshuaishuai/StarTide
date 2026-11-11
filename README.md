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
