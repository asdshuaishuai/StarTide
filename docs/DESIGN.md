# skillctl 设计

## 目标与约束

一个工具做一件事：扫 home → 找到所有 skill → 复制进统一 store → 各 agent
的 skills 根目录整体替换成软链。不做商店、不做按 agent 的开关管理。

约束来自对现有系统的逆向：

| 事实 | 来源 |
|---|---|
| ZCode 加载器跟随软链（目录条目 `isDirectory() || isSymbolicLink()`，根目录 `stat()`） | `zcode.cjs` 中 `followSymbolicLinks: t.source !== "plugin"` |
| 插件缓存每次安装/更新会整目录原子替换、卸载即删 | 同上，`Sht`/`wwn` |
| ZCode 的 skill 根只有 `~/.zcode/skills` 与 `~/.agents/skills`（项目内还有 `<dir>/.zcode/skills`） | 同上常量 `Fra`/`jra`/`Ppo` |
| skill 身份是 frontmatter `name:`，不是目录名 | 950 样本中 24 个目录名与 name 不符 |
| `description` 37% 是 block scalar 或带引号 | 单行正则会截断 |
| 软链条目占样本 44%，最大的软链指向 952MB | 扫描绝不跟随软链 |

## 数据模型

**Skill Unit** = 含 `SKILL.md` 且不嵌套在其他 unit 内的极大目录。

```
unit { id = sha256(tree)[0:16], name, description, size, files, sources[] }
```

- `id` 是内容身份：对树内全部非点文件按相对路径排序，`hash(relpath ⊕ content)`。
  点文件、`.git`、构建噪声不参与，第三方工具写锁文件不会改变身份。
- 目录名不是身份的一部分：同一份内容出现在 5 个 agent 目录下只算 1 个 unit。

## 扫描

从 `~` 出发，`lstat` 遍历，不跟随软链，跳过排除表（目录名表 + 路径片段表）。

- 目录含 `SKILL.md`（regular file）→ 记为 unit，**不再下降**
- 名为 `skills`/`skill` 的目录记为候选根
- 软链条目记为 alias，不展开

可达性复刻 loader 语义：unit U 对根 R 可达 ⟺ R→U 路径上没有中间目录自身
是 unit。`reverse-skill`（内含 80 个子 skill）对 loader 只暴露它自己。

### view 的真相源

`link`/`sync` 重建 view 时的输入是 **扫描结果 ∪ catalog**，不是单纯的扫描
结果。原因：第一次 link 之后各 skills 根都成了软链，扫描器不再深入它们，
单靠扫描会得到一个近空的列表，`sh_view_sync` 会把已有链接全删掉。catalog
中 `stored=1` 的条目被合成为伪 unit（root 取其首个来源路径，供 scope-own
可达性判定），保证 view 永远是 store 的完整投影。

## Store

```
~/.skillctl/units/<id>/   内容；导入走 .incoming-<pid>-<id> 暂存 + rename 原子落位
~/.skillctl/views/<agent>/   条目全是相对软链 → ../../units/<id>
~/.skillctl/trash/<agent>-<时间戳>/   link 时被替换的原目录
~/.skillctl/catalog.jsonl   每 unit 一行 JSON
```

同名不同内容都入库；view 内主版本占原名，其余 `<name>__<hash8>`。主版本
判定：MANAGED 优于 EXTERNAL（插件缓存），mtime 新者优于旧者。命名冲突的
取舍在**每个 view 内**做，不在全局做——scope-own 下两个 agent 各自保留
自己的 `alpha`。

## link 的原子性

对每个 agent 根：

1. 根已是软链 → 指向我们的 view 则跳过，指向别处则报错不动
2. 根是真实目录 → 先 scan+import，然后 `rename` 进 trash，再建软链
   `skills -> ../.skillctl/views/<agent>`（相对路径，随 home 迁移有效；
   两侧都取 realpath，规避 macOS `/tmp → /private/tmp` 一类父级软链）
3. 失败任一步即中止；`unlink` 从 trash 恢复

## 跨平台

- `sh_fs.h` 是唯一的系统调用边界；内部一律 UTF-8 + `/`
- POSIX 后端已实现；Win32 后端实现同一接口（`FindFirstFileW`、
  `CreateSymbolicLinkW`、`MoveFileExW`、UTF-8↔UTF-16 边界转换，
  目录软链无权限时降级 junction）
- GUI（GPUI）只 spawn CLI，逻辑零重复；CLI 的 `--json`/`--stream` 是
  两层之间唯一契约

## 测试

- `tests/test_core.c`：路径、YAML（block scalar/CRLF/引号/嵌套）、SHA-256
  标准向量、软链、树哈希稳定性，65 项
- `tests/e2e.sh`：夹具 home（嵌套 skill、同名异版、CRLF、插件缓存布局、
  共享 .agents）上跑 scan→import→link→scope 切换→status→unlink→gc→sync
  全流程，27 项
- `make asan` + 全命令冒烟：AddressSanitizer 零报告
