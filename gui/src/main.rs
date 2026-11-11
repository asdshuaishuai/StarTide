/*
 * StarTide - a calm home for your AI agent skills.
 *
 * A thin native shell around the skillctl engine, styled after the VS
 * Code extension panel: an agent tree on the left, skills as a card wall
 * or a compact list in the middle, and a detail pane that appears when a
 * skill is picked.
 *
 * The C command line is the only source of truth; this window spawns it
 * and renders its JSON.
 *
 * Keyboard map:
 *   up/down (or j/k)  move the selection
 *   / or ⌘K (Ctrl+K)  focus the filter field
 *   enter             open or close the detail pane
 *   esc               clear the filter, or drop the selection
 */
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::rc::Rc;

use gpui_kit::assets::Assets;
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::TitleBar;
use gpui_kit::component::scroll::ScrollableElement;
use gpui_kit::component::{Disableable, Icon, IconName, Sizable, Theme, ThemeMode};
use gpui_kit::prelude::*;
use gpui_kit::*;
use serde::Deserialize;

/* ------------------------------------------------------ VS Code palette */

/* gpui 的 rgb() 不是 const fn，手动构建 Rgba。 */
const fn rgba_hex(hex: u32, alpha: f32) -> Rgba {
    Rgba {
        r: ((hex >> 16) & 0xff) as f32 / 255.0,
        g: ((hex >> 8) & 0xff) as f32 / 255.0,
        b: (hex & 0xff) as f32 / 255.0,
        a: alpha,
    }
}

/* macOS 毛玻璃：面板半透明，模糊材质从窗口后透出。 */
const TRANSPARENT: Rgba = rgba_hex(0x000000, 0.0);

/* 可切换主题：深/浅 × 强调色。所有界面颜色都从 Palette 取。 */
#[derive(Clone, Copy)]
struct Palette {
    side: Rgba,
    input: Rgba,
    card: Rgba,
    hover: Rgba,
    sel: Rgba,
    line: Rgba,
    fg: Rgba,
    fg_bright: Rgba,
    fg_dim: Rgba,
    fg_faint: Rgba,
    focus: Rgba,
    link: Rgba,
    green: Rgba,
    amber: Rgba,
    row_border: Rgba,
    card_hover_border: Rgba,
    panel: Rgba,
    panel_border: Rgba,
    dot_off: Rgba,
    badge_bg: Rgba,
    statusbar_bg: Rgba,
    accent: Rgba,
}

impl gpui::Global for Palette {}

/* 全局取色：任何持有 cx 的地方都能用。 */
fn pal(cx: &App) -> &Palette {
    cx.global::<Palette>()
}

/* 在颜色上叠一层透明度。 */
fn with_alpha(c: Rgba, a: f32) -> Rgba {
    Rgba { r: c.r, g: c.g, b: c.b, a }
}

/* 深浅两套底色，强调色动态注入（sel/focus/accent 跟随）。 */
fn build_palette(dark: bool, accent: Rgba) -> Palette {
    if dark {
        Palette {
            side: rgba_hex(0x202023, 0.66),
            input: rgba_hex(0x2c2c30, 0.85),
            card: rgba_hex(0x2a2a2e, 0.62),
            hover: rgba_hex(0x36363a, 0.55),
            sel: with_alpha(accent, 0.42),
            line: rgba_hex(0x333333, 1.0),
            fg: rgba_hex(0xcccccc, 1.0),
            fg_bright: rgba_hex(0xe7e7e7, 1.0),
            fg_dim: rgba_hex(0x858585, 1.0),
            fg_faint: rgba_hex(0x6a6a6a, 1.0),
            focus: accent,
            link: rgba_hex(0x3794ff, 1.0),
            green: rgba_hex(0x89d185, 1.0),
            amber: rgba_hex(0xcca700, 1.0),
            row_border: rgba_hex(0x2b2b2b, 1.0),
            card_hover_border: rgba_hex(0x4a4a4a, 1.0),
            panel: rgba_hex(0xffffff, 0.03),
            panel_border: rgba_hex(0xffffff, 0.05),
            dot_off: rgba_hex(0x3a3a3a, 1.0),
            badge_bg: rgba_hex(0x2d2d2f, 1.0),
            statusbar_bg: rgba_hex(0x18181b, 0.85),
            accent,
        }
    } else {
        Palette {
            side: rgba_hex(0xf6f6f7, 0.72),
            input: rgba_hex(0xffffff, 0.75),
            card: rgba_hex(0xffffff, 0.6),
            hover: rgba_hex(0xe4e4e7, 0.6),
            sel: with_alpha(accent, 0.16),
            line: rgba_hex(0xd8d8dc, 1.0),
            fg: rgba_hex(0x52525b, 1.0),
            fg_bright: rgba_hex(0x18181b, 1.0),
            fg_dim: rgba_hex(0x71717a, 1.0),
            fg_faint: rgba_hex(0xa1a1aa, 1.0),
            focus: accent,
            link: rgba_hex(0x2563eb, 1.0),
            green: rgba_hex(0x16a34a, 1.0),
            amber: rgba_hex(0xb45309, 1.0),
            row_border: rgba_hex(0xe4e4e7, 1.0),
            card_hover_border: rgba_hex(0xc8c8cc, 1.0),
            panel: rgba_hex(0x000000, 0.03),
            panel_border: rgba_hex(0x000000, 0.06),
            dot_off: rgba_hex(0xd4d4d8, 1.0),
            badge_bg: rgba_hex(0xeeeeef, 1.0),
            statusbar_bg: rgba_hex(0xfafafa, 0.85),
            accent,
        }
    }
}

/* 强调色选项。 */
const ACCENTS: [(&str, Rgba); 4] = [
    ("青", rgba_hex(0x2dd4bf, 1.0)),
    ("蓝", rgba_hex(0x5b9cf6, 1.0)),
    ("紫", rgba_hex(0xa78bfa, 1.0)),
    ("琥珀", rgba_hex(0xe0a93c, 1.0)),
];

/* 主题偏好（持久化到 store 目录的 gui-theme.json）。 */
#[derive(Clone, Copy, PartialEq)]
struct ThemePref {
    dark: bool,
    accent: usize,
}

#[derive(serde::Serialize, serde::Deserialize)]
struct ThemePrefJson {
    dark: bool,
    accent: usize,
}

fn theme_path() -> Option<std::path::PathBuf> {
    let home = std::env::var("SKILLCTL_HOME")
        .ok()
        .filter(|s| !s.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var("HOME").ok().map(|h| std::path::PathBuf::from(h).join(".skillctl")))?;
    Some(home.join("gui-theme.json"))
}

fn load_pref() -> ThemePref {
    let default = ThemePref { dark: true, accent: 0 };
    let Some(path) = theme_path() else { return default };
    std::fs::read_to_string(path)
        .ok()
        .and_then(|s| serde_json::from_str::<ThemePrefJson>(&s).ok())
        .map(|j| ThemePref { dark: j.dark, accent: j.accent })
        .unwrap_or(default)
}

fn save_pref(pref: ThemePref) {
    if let Some(path) = theme_path() {
        let json = ThemePrefJson { dark: pref.dark, accent: pref.accent };
        if let Ok(s) = serde_json::to_string(&json) {
            let _ = std::fs::write(path, s);
        }
    }
}

/* 应用主题：写全局调色板 + 同步 kit 主题与品牌色。 */
fn apply_theme(pref: ThemePref, cx: &mut App) {
    let accent = ACCENTS[pref.accent % ACCENTS.len()].1;
    cx.set_global(build_palette(pref.dark, accent));
    Theme::change(
        if pref.dark { ThemeMode::Dark } else { ThemeMode::Light },
        None,
        cx,
    );
    let theme = Theme::global_mut(cx);
    theme.primary = accent.into();
    theme.accent = accent.into();
}

/* 一个 agent 一个色相。 */
const AGENT_COLORS: [Rgba; 10] = [
    rgba_hex(0x2f7dd1, 1.0),
    rgba_hex(0x3d9a5f, 1.0),
    rgba_hex(0xc98a2e, 1.0),
    rgba_hex(0xc25b7c, 1.0),
    rgba_hex(0x2f9a8f, 1.0),
    rgba_hex(0xd0713f, 1.0),
    rgba_hex(0x4a6fa5, 1.0),
    rgba_hex(0x7c8fa6, 1.0),
    rgba_hex(0xb05a5a, 1.0),
    rgba_hex(0x2f8f8a, 1.0),
];

const DETAIL_WIDTH: f32 = 312.0;

/* 文件清单：展示深度 ≤2（由 read_file_listing 的两层循环保证）、最多 50 条。 */
const LISTING_MAX_ENTRIES: usize = 50;

/* 封面扫描：单文件 ≤2MB。 */
const COVER_MAX_FILE_BYTES: u64 = 2 * 1024 * 1024;

/* 只读展示缓存（目录清单 / 卡片封面）的条目上限，超过即整体清空，防止无界增长。 */
const FS_CACHE_LIMIT: usize = 1024;

/* --------------------------------------------------------- cli output */

#[derive(Clone, Deserialize)]
struct CatalogLine {
    id: String,
    name: String,
    #[serde(default)]
    description: String,
    #[serde(default)]
    size: f64,
    #[serde(default)]
    stored: bool,
    #[serde(default)]
    agents: Vec<String>,
    #[serde(default)]
    sources: Vec<String>,
}

#[derive(Deserialize)]
struct StatusJson {
    #[serde(default)]
    store: StoreJson,
    #[serde(default)]
    agents: Vec<AgentJson>,
}

#[derive(Deserialize, Default)]
struct StoreJson {
    #[serde(default)]
    units: i64,
    #[serde(default)]
    bytes: f64,
    #[serde(default)]
    home: String,
}

#[allow(dead_code)]
#[derive(Deserialize)]
struct AgentJson {
    id: String,
    #[serde(default)]
    label: String,
    #[serde(default)]
    state: String,
    #[serde(default)]
    root: String,
    #[serde(default)]
    links: i64,
    #[serde(default)]
    include: Vec<String>,
}

/* -------------------------------------------------------------- model */

#[derive(Clone, Copy, PartialEq)]
enum StatusTab {
    All,
    Stored,
    Known,
}

#[derive(Clone, Copy, PartialEq)]
enum SkillView {
    Cards,
    List,
}

#[derive(Clone, Copy, PartialEq)]
enum MessageKind {
    Idle,
    Busy,
    Ok,
    Err,
}

/* 文件清单的一行；目录在展示时补「/」后缀，depth 1 是根目录项，2 是二级项。 */
#[derive(Clone)]
struct ListingEntry {
    name: String,
    is_dir: bool,
    depth: usize,
}

struct AppState {
    cli: PathBuf,
    cli_ok: bool,
    skills: Vec<CatalogLine>,
    agents: Vec<AgentJson>,
    store_home: String,
    store_units: i64,
    store_bytes: f64,

    query: String,
    status_tab: StatusTab,
    agent_filter: Option<String>,
    skill_view: SkillView,
    selected: Option<usize>,
    show_detail: bool,
    theme_pref: ThemePref,
    unlink_armed: bool,

    busy: bool,
    msg_kind: MessageKind,
    msg: SharedString,

    search_state: Option<Entity<InputState>>,

    /* 只读展示缓存（GUI 直接读文件系统；一切写操作仍只走 CLI）：
     * 目录清单与封面都按 skill_base_dir 的目录路径缓存。 */
    file_listings: HashMap<String, Option<Vec<ListingEntry>>>,
    card_thumbs: HashMap<String, Option<PathBuf>>,
}

fn format_bytes(bytes: f64) -> String {
    let units = ["B", "KB", "MB", "GB", "TB"];
    let mut value = bytes;
    let mut unit = 0;
    while value >= 1024.0 && unit < units.len() - 1 {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{} B", bytes as u64)
    } else {
        format!("{:.1} {}", value, units[unit])
    }
}

/* 三平台 UI 字体；中文回退系统字体（PingFang / Microsoft YaHei）。 */
fn ui_font() -> &'static str {
    if cfg!(target_os = "macos") {
        "Avenir Next"
    } else if cfg!(target_os = "windows") {
        "Segoe UI"
    } else {
        "Noto Sans"
    }
}

/* 三平台“在文件管理器中显示”。 */
fn reveal_in_file_manager(path: &str) {
    if cfg!(target_os = "macos") {
        let _ = Command::new("open").arg("-R").arg(path).spawn();
    } else if cfg!(target_os = "windows") {
        let _ = Command::new("explorer")
            .arg(format!("/select,\"{}\"", path))
            .spawn();
    } else {
        let parent = Path::new(path)
            .parent()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_else(|| path.to_string());
        let _ = Command::new("xdg-open").arg(parent).spawn();
    }
}

fn modifier_label() -> &'static str {
    if cfg!(target_os = "macos") {
        "⌘"
    } else {
        "Ctrl"
    }
}

/* ---------------------------------------------------------------- fs read */

/*
 * 以下均为只读展示用途：GUI 直接读文件系统，不做任何写入；
 * 一切写操作仍只经 CLI（既定设计决策）。
 */

/* 读取一个目录并排序：目录优先，同级按名称。失败返回 None。 */
fn read_dir_entries(dir: &Path) -> Option<Vec<(String, bool, PathBuf)>> {
    let rd = std::fs::read_dir(dir).ok()?;
    let mut entries: Vec<(String, bool, PathBuf)> = rd
        .filter_map(|e| e.ok())
        .map(|e| {
            let name = e.file_name().to_string_lossy().into_owned();
            let is_dir = e.file_type().map(|t| t.is_dir()).unwrap_or(false);
            (name, is_dir, e.path())
        })
        .collect();
    entries.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    Some(entries)
}

/* 详情「文件清单」：深度 ≤2、最多 50 条。根目录读不到整体返回 None；
 * 二级子目录读不到则跳过该子目录，不影响其余条目。 */
fn read_file_listing(root: &Path) -> Option<Vec<ListingEntry>> {
    let mut out: Vec<ListingEntry> = Vec::new();
    for (name, is_dir, path) in read_dir_entries(root)? {
        if out.len() >= LISTING_MAX_ENTRIES {
            break;
        }
        out.push(ListingEntry { name, is_dir, depth: 1 });
        if is_dir {
            for (name, is_dir, _) in read_dir_entries(&path).unwrap_or_default() {
                if out.len() >= LISTING_MAX_ENTRIES {
                    break;
                }
                out.push(ListingEntry { name, is_dir, depth: 2 });
            }
        }
    }
    Some(out)
}

/* 卡片代表图：cover.* / preview.* / icon.* 依次优先，
 * 否则取首个 *.png|jpg|jpeg|webp。扫描深度 ≤2、跳过点目录、单文件 ≤2MB。 */
fn find_cover_image(root: &Path) -> Option<PathBuf> {
    /* 具名封面允许位图以外的常见图片格式（icon.svg / icon.ico 很常见）。
     * 不收 gif：gpui 会把多帧 GIF 逐帧动画，每个布局 pass 都请求重绘，
     * 活动期间整棵 UI 每帧重建、CPU 常驻占用，封面场景不值得。 */
    const NAMED_EXTS: [&str; 6] = ["png", "jpg", "jpeg", "webp", "svg", "ico"];
    const FALLBACK_EXTS: [&str; 4] = ["png", "jpg", "jpeg", "webp"];
    const STEM_PRIORITY: [&str; 3] = ["cover", "preview", "icon"];

    /* (路径, 小写主干, 小写扩展名, 深度)；先一层后二层、层内已按名排序。 */
    let mut candidates: Vec<(PathBuf, String, String)> = Vec::new();
    collect_image_files(root, &NAMED_EXTS, &mut candidates);
    for (name, is_dir, path) in read_dir_entries(root).unwrap_or_default() {
        if is_dir && !name.starts_with('.') {
            collect_image_files(&path, &NAMED_EXTS, &mut candidates);
        }
    }
    if candidates.is_empty() {
        return None;
    }

    for stem in STEM_PRIORITY {
        if let Some((path, _, _)) = candidates.iter().find(|(_, s, _)| s == stem) {
            return Some(path.clone());
        }
    }
    candidates
        .into_iter()
        .find(|(_, _, ext)| FALLBACK_EXTS.contains(&ext.as_str()))
        .map(|(path, _, _)| path)
}

/* 把目录下（不递归）符合条件的图片文件追加进 candidates。 */
fn collect_image_files(dir: &Path, exts: &[&str], candidates: &mut Vec<(PathBuf, String, String)>) {
    for (name, is_dir, path) in read_dir_entries(dir).unwrap_or_default() {
        if is_dir || name.starts_with('.') {
            continue;
        }
        let (stem, ext) = match name.rsplit_once('.') {
            Some((s, e)) => (s.to_lowercase(), e.to_lowercase()),
            None => continue,
        };
        if exts.contains(&ext.as_str())
            && std::fs::metadata(&path)
                .map(|m| m.len() <= COVER_MAX_FILE_BYTES)
                .unwrap_or(false)
        {
            candidates.push((path, stem, ext));
        }
    }
}

/* 首字母章：卡片无封面时的占位；封面加载失败也回退到它。 */
fn initial_badge(letter: String, color: Rgba, badge_bg: Rgba) -> gpui::Div {
    div()
        .w(px(26.0))
        .h(px(26.0))
        .flex_none()
        .flex()
        .items_center()
        .justify_center()
        .rounded(px(5.0))
        .font_family("Menlo")
        .text_size(px(12.0))
        .font_weight(FontWeight::BOLD)
        .text_color(color)
        .bg(badge_bg)
        .child(SharedString::from(letter))
}

/* 定位 skillctl 引擎：SKILLCTL_BIN、项目树或 PATH。 */
fn find_cli() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("SKILLCTL_BIN") {
        if !p.is_empty() {
            let pb = PathBuf::from(p);
            if pb.exists() {
                return Some(pb);
            }
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        let mut dir = exe.parent().map(|p| p.to_path_buf());
        for _ in 0..8 {
            match dir {
                Some(d) => {
                    for name in ["build/skillctl", "build-win/skillctl.exe"] {
                        let cand = d.join(name);
                        if cand.exists() {
                            return Some(cand);
                        }
                    }
                    dir = d.parent().map(|p| p.to_path_buf());
                }
                None => break,
            }
        }
    }
    None
}

impl AppState {
    fn new() -> Self {
        let cli = find_cli().unwrap_or_default();
        let cli_ok = cli.exists();
        AppState {
            cli_ok,
            cli,
            skills: Vec::new(),
            agents: Vec::new(),
            store_home: String::new(),
            store_units: 0,
            store_bytes: 0.0,
            query: String::new(),
            status_tab: StatusTab::All,
            agent_filter: None,
            skill_view: SkillView::Cards,
            selected: None,
            show_detail: false,
            theme_pref: load_pref(),
            unlink_armed: false,
            busy: false,
            msg_kind: MessageKind::Idle,
            msg: "启动中".into(),
            search_state: None,
            file_listings: HashMap::new(),
            card_thumbs: HashMap::new(),
        }
    }

    fn set_message(&mut self, kind: MessageKind, text: impl Into<SharedString>) {
        self.msg_kind = kind;
        self.msg = text.into();
    }

    fn agent_label(&self, id: &str) -> String {
        self.agents
            .iter()
            .find(|a| a.id == id)
            .map(|a| {
                if a.label.is_empty() {
                    a.id.clone()
                } else {
                    a.label.clone()
                }
            })
            .unwrap_or_else(|| id.to_string())
    }

    fn agent_color(&self, id: &str) -> Rgba {
        let i = self.agents.iter().position(|a| a.id == id).unwrap_or(0);
        AGENT_COLORS[i % AGENT_COLORS.len()]
    }

    /* 只读展示的基准目录：优先仓库 units 目录，否则第一个来源路径。 */
    fn skill_base_dir(&self, entry: &CatalogLine) -> Option<String> {
        if entry.stored && !self.store_home.is_empty() {
            return Some(format!("{}/units/{}", self.store_home, entry.id));
        }
        entry
            .sources
            .first()
            .filter(|s| !s.is_empty())
            .cloned()
    }

    /* 选中变化时读一次目录：缓存命中则绝不重读。 */
    fn ensure_file_listing(&mut self, entry: &CatalogLine) {
        let Some(dir) = self.skill_base_dir(entry) else {
            return;
        };
        if self.file_listings.contains_key(&dir) {
            return;
        }
        let listing = read_file_listing(Path::new(&dir));
        if self.file_listings.len() >= FS_CACHE_LIMIT {
            self.file_listings.clear();
        }
        self.file_listings.insert(dir, listing);
    }

    /* 卡片惰性封面：render 只查缓存；未命中先占位 None（防止重复派发），
     * 再把 find_cover_image（纯 fs 读）放到后台执行，完成后写回缓存并通知重绘。
     * 目录口径与文件清单一致，都用 skill_base_dir。 */
    fn ensure_card_thumb(&mut self, entry: &CatalogLine, cx: &Context<Self>) {
        let Some(dir) = self.skill_base_dir(entry) else {
            return;
        };
        if self.card_thumbs.contains_key(&dir) {
            return;
        }
        if self.card_thumbs.len() >= FS_CACHE_LIMIT {
            self.card_thumbs.clear();
        }
        self.card_thumbs.insert(dir.clone(), None);

        let dir_for_scan = dir.clone();
        cx.spawn(async move |this, cx| {
            let cover = cx
                .background_executor()
                .spawn(async move { find_cover_image(Path::new(&dir_for_scan)) })
                .await;
            let _ = this.update(cx, |state, cx| {
                state.card_thumbs.insert(dir, cover);
                cx.notify();
            });
        })
        .detach();
    }

    /* ------------------------------------------------------- cli access */

    fn refresh(&mut self, cx: &mut Context<Self>) {
        /* CLI 刚可能写过磁盘：清空只读展示缓存，下次选中再读。 */
        self.file_listings.clear();
        self.card_thumbs.clear();
        if let Some(lines) = self.read_lines(&["catalog", "--json"]) {
            let mut lines = lines;
            lines.sort_by(|a, b| a.name.cmp(&b.name));
            self.skills = lines;
        }
        if let Some(status) = self.read_json::<StatusJson>(&["status", "--json"]) {
            self.store_units = status.store.units;
            self.store_bytes = status.store.bytes;
            self.store_home = status.store.home;
            self.agents = status.agents;
        }
        if matches!(self.msg_kind, MessageKind::Idle | MessageKind::Busy) {
            self.set_message(
                MessageKind::Idle,
                format!(
                    "{} 个技能 · {} 个 agent · 共享 {}",
                    self.skills.len(),
                    self.agents.len(),
                    format_bytes(self.store_bytes)
                ),
            );
        }
        cx.notify();
    }

    fn run(
        &mut self,
        command: &'static str,
        args: Vec<&'static str>,
        then_refresh: bool,
        cx: &mut Context<Self>,
    ) {
        if self.busy || !self.cli_ok {
            return;
        }
        self.busy = true;
        self.unlink_armed = false;
        self.set_message(MessageKind::Busy, format!("正在执行 {}…", command));
        cx.notify();

        let cli = self.cli.clone();
        cx.spawn(async move |this, cx| {
            let output = cx
                .background_executor()
                .spawn(async move { Command::new(&cli).args(&args).output() })
                .await;

            let result = match output {
                Ok(out) if out.status.success() => {
                    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
                }
                Ok(out) => {
                    let err = String::from_utf8_lossy(&out.stderr);
                    Err(if err.is_empty() {
                        format!("{} 退出码 {}", command, out.status)
                    } else {
                        err.lines().next().unwrap_or("失败").to_string()
                    })
                }
                Err(e) => Err(format!("无法运行 {}: {}", command, e)),
            };

            let _ = this.update(cx, |state, cx| {
                state.busy = false;
                match result {
                    Ok(text) => {
                        if !state.apply(command, &text) {
                            state.set_message(MessageKind::Ok, format!("{} 完成", command));
                        }
                    }
                    Err(err) => state.set_message(MessageKind::Err, err),
                }
                if then_refresh {
                    state.refresh(cx);
                } else {
                    cx.notify();
                }
            });
        })
        .detach();
    }

    fn read_lines(&self, args: &[&str]) -> Option<Vec<CatalogLine>> {
        let out = Command::new(&self.cli).args(args).output().ok()?;
        if !out.status.success() {
            return None;
        }
        let text = String::from_utf8_lossy(&out.stdout);
        let mut lines = Vec::new();
        for line in text.lines() {
            if let Ok(entry) = serde_json::from_str::<CatalogLine>(line) {
                lines.push(entry);
            }
        }
        Some(lines)
    }

    fn read_json<T: serde::de::DeserializeOwned>(&self, args: &[&str]) -> Option<T> {
        let out = Command::new(&self.cli).args(args).output().ok()?;
        if !out.status.success() {
            return None;
        }
        serde_json::from_slice(&out.stdout).ok()
    }

    fn apply(&mut self, command: &str, stdout: &str) -> bool {
        let v: serde_json::Value = match serde_json::from_str(stdout) {
            Ok(v) => v,
            Err(_) => return false,
        };
        match command {
            "scan" => {
                let units = v["stats"]["units"].as_i64().unwrap_or(0);
                let unique = v["stats"]["unique"].as_i64().unwrap_or(0);
                self.set_message(
                    MessageKind::Ok,
                    format!("扫描完成：{} 个技能目录，{} 个内容唯一", units, unique),
                );
                true
            }
            "link" => {
                let skills = v["skills"].as_i64().unwrap_or(0);
                self.set_message(
                    MessageKind::Ok,
                    format!("链接预览：将链接 {} 个技能", skills),
                );
                true
            }
            "sync" => {
                let imported = v["imported"].as_i64().unwrap_or(0);
                self.set_message(MessageKind::Ok, format!("同步完成：新导入 {} 个", imported));
                true
            }
            "gc" => {
                let removed = v["removed"].as_i64().unwrap_or(0);
                self.set_message(
                    MessageKind::Ok,
                    format!("清理预览：{} 个仓库单元未被引用", removed),
                );
                true
            }
            "unlink" => {
                let restored = v["restored"].as_i64().unwrap_or(0);
                self.set_message(
                    MessageKind::Ok,
                    format!("已还原 {} 个 agent 的原始目录", restored),
                );
                true
            }
            _ => false,
        }
    }

    /* -------------------------------------------------------- selection */

    fn visible_indices(&self) -> Vec<usize> {
        let query = self.query.to_lowercase();
        let agent_filter = self.agent_filter.clone();

        self.skills
            .iter()
            .enumerate()
            .filter(|(_, e)| {
                if !query.is_empty()
                    && !e.name.to_lowercase().contains(&query)
                    && !e.description.to_lowercase().contains(&query)
                {
                    return false;
                }
                match self.status_tab {
                    StatusTab::All => true,
                    StatusTab::Stored => e.stored,
                    StatusTab::Known => !e.stored,
                }
            })
            .filter(|(_, e)| match &agent_filter {
                Some(id) => e.agents.iter().any(|a| a == id),
                None => true,
            })
            .map(|(i, _)| i)
            .collect()
    }

    fn select(&mut self, ix: Option<usize>, cx: &mut Context<Self>) {
        self.selected = ix;
        self.show_detail = ix.is_some();
        cx.notify();
    }

    fn move_selection(&mut self, delta: isize, cx: &mut Context<Self>) {
        let count = self.visible_indices().len();
        if count == 0 {
            return;
        }
        let next = match self.selected {
            None => {
                if delta < 0 {
                    count - 1
                } else {
                    0
                }
            }
            Some(current) => (current as isize + delta).clamp(0, count as isize - 1) as usize,
        };
        self.select(Some(next), cx);
    }

    /* ------------------------------------------------------ key handling */

    fn on_root_key(&mut self, event: &KeyDownEvent, window: &mut Window, cx: &mut Context<Self>) {
        /* kit Input 持有过滤文本；每次按键同步。 */
        if let Some(state) = &self.search_state {
            self.query = state.read(cx).value().to_string();
        }
        let key = event.keystroke.key.as_str();
        let search_focused = self
            .search_state
            .as_ref()
            .is_some_and(|s| s.read(cx).focus_handle(cx).is_focused(window));

        match key {
            "k" if event.keystroke.modifiers.platform => {
                if let Some(s) = &self.search_state {
                    s.read(cx).focus_handle(cx).clone().focus(window, cx);
                }
                cx.notify();
            }
            "/" if !search_focused => {
                if let Some(s) = &self.search_state {
                    s.read(cx).focus_handle(cx).clone().focus(window, cx);
                }
                cx.notify();
            }
            "up" => self.move_selection(-1, cx),
            "down" => self.move_selection(1, cx),
            "j" if !search_focused => self.move_selection(1, cx),
            "k" if !search_focused => self.move_selection(-1, cx),
            "home" => self.select(if self.visible_indices().is_empty() { None } else { Some(0) }, cx),
            "end" => {
                let count = self.visible_indices().len();
                self.select(if count == 0 { None } else { Some(count - 1) }, cx);
            }
            "enter" => {
                let count = self.visible_indices().len();
                if self.selected.filter(|i| *i < count).is_some() {
                    self.show_detail = !self.show_detail;
                    cx.notify();
                }
            }
            "escape" => {
                if search_focused {
                    if let Some(s) = &self.search_state {
                        s.update(cx, |s, cx| s.set_value("", window, cx));
                    }
                    cx.notify();
                } else if self.selected.is_some() {
                    self.selected = None;
                    cx.notify();
                }
            }
            _ => {}
        }
    }
}

/* -------------------------------------------------------------- view */

/* 在品牌色上叠一层透明度，做成柔和底色。 */
fn tint(c: Rgba) -> Rgba {
    Rgba { r: c.r, g: c.g, b: c.b, a: 0.16 }
}

impl AppState {
    /* 视图标题栏的 ghost 图标按钮。 */
    fn icon_action<I: gpui_kit::component::IconNamed + Clone + 'static>(
        &self,
        id: &'static str,
        icon: I,
        tooltip: &'static str,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> Button {
        let view = cx.entity();
        Button::new(id)
            .ghost()
            .icon(icon)
            .tooltip(tooltip)
            .disabled(self.busy || !self.cli_ok)
            .on_click(move |_e, _window, cx| {
                let _ = view.update(cx, |state, cx| on_click(state, cx));
            })
    }

    /* 两态确认按钮（还原）：与其他工具钮一致，仅图标 + 悬浮提示。 */
    fn confirm_button(&self, cx: &Context<Self>) -> Button {
        let view = cx.entity();
        let armed = self.unlink_armed;
        let tooltip: SharedString = if armed {
            "确认：还原所有 agent 的原始目录".into()
        } else {
            "还原所有 agent 的原始目录".into()
        };
        let mut b = Button::new("unlink")
            .icon(IconName::Undo)
            .tooltip(tooltip)
            .disabled(self.busy || !self.cli_ok);
        if armed {
            b = b.danger();
        }
        b.on_click(move |_e, _window, cx| {
            view.update(cx, |state, cx| {
                if state.unlink_armed {
                    state.run("unlink", vec!["unlink", "--json"], true, cx);
                } else {
                    state.unlink_armed = true;
                    state.set_message(
                        MessageKind::Err,
                        "再点一次「还原」以恢复所有 agent 的原始技能目录",
                    );
                    cx.notify();
                }
            });
        })
    }

    /* 状态过滤胶囊。 */
    fn tab_chip(
        &self,
        id: String,
        label: &'static str,
        count: usize,
        active: bool,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::Stateful<gpui::Div> {
        let p = *pal(cx);
        div()
            .id(SharedString::from(id))
            .h(px(22.0))
            .px(px(8.0))
            .flex()
            .items_center()
            .gap(px(5.0))
            .rounded(px(3.0))
            .text_size(px(11.5))
            .bg(if active { p.sel } else { TRANSPARENT })
            .text_color(if active { p.fg_bright } else { p.fg_dim })
            .when(!active, |c| c.hover(|s| s.bg(p.hover).text_color(p.fg)))
            .on_click(cx.listener(move |state, _e, _w, cx| on_click(state, cx)))
            .child(label)
            .child(
                div()
                    .font_family("Menlo")
                    .text_size(px(9.5))
                    .text_color(if active { p.fg_bright } else { p.fg_faint })
                    .child(SharedString::from(count.to_string())),
            )
    }

    /* 视图切换小图标钮。 */
    fn view_toggle(
        &self,
        id: &'static str,
        icon: IconName,
        active: bool,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::Stateful<gpui::Div> {
        let p = *pal(cx);
        div()
            .id(SharedString::from(id))
            .w(px(24.0))
            .h(px(22.0))
            .flex()
            .items_center()
            .justify_center()
            .rounded(px(3.0))
            .bg(if active { p.sel } else { TRANSPARENT })
            .text_color(if active { p.fg_bright } else { p.fg_dim })
            .when(!active, |c| c.hover(|s| s.bg(p.hover).text_color(p.fg)))
            .on_click(cx.listener(move |state, _e, _w, cx| on_click(state, cx)))
            .child(Icon::new(icon).with_size(px(13.0)))
    }

    /* 智能体树行。 */
    fn agent_row(
        &self,
        id: String,
        label: String,
        initial: String,
        color: Rgba,
        count: usize,
        active: bool,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::Stateful<gpui::Div> {
        let p = *pal(cx);
        div()
            .id(SharedString::from(id))
            .h(px(30.0))
            .pl(px(10.0))
            .pr(px(10.0))
            .flex()
            .items_center()
            .gap(px(8.0))
            .rounded(px(6.0))
            .bg(if active { p.sel } else { TRANSPARENT })
            .when(!active, |r| r.hover(|s| s.bg(p.hover)))
            .on_click(cx.listener(move |state, _e, _w, cx| on_click(state, cx)))
            .child(
                div()
                    .w(px(20.0))
                    .h(px(20.0))
                    .flex_none()
                    .flex()
                    .items_center()
                    .justify_center()
                    .rounded(px(5.0))
                    .font_family("Menlo")
                    .text_size(px(10.0))
                    .font_weight(FontWeight::BOLD)
                    .text_color(if active {
                        rgba_hex(0xffffff, 1.0)
                    } else {
                        color
                    })
                    .bg(if active { color } else { tint(color) })
                    .child(SharedString::from(initial)),
            )
            .child(
                div()
                    .min_w_0()
                    .flex_1()
                    .truncate()
                    .text_size(px(12.5))
                    .text_color(if active { p.fg_bright } else { p.fg })
                    .child(SharedString::from(label)),
            )
            .child(
                div()
                    .font_family("Menlo")
                    .flex_none()
                    .text_size(px(10.0))
                    .text_color(if active { p.fg_bright } else { p.fg_faint })
                    .child(SharedString::from(count.to_string())),
            )
    }

    /* 详情分区面板：淡淡的凸起底，避免整栏松散。 */
    fn section_panel(&self, label: &'static str, content: gpui::AnyElement, cx: &App) -> gpui::Div {
        let p = *pal(cx);
        div()
            .flex()
            .flex_col()
            .gap_1p5()
            .p(px(10.0))
            .rounded(px(6.0))
            .bg(p.panel)
            .border_1()
            .border_color(p.panel_border)
            .child(self.section_label(label, cx))
            .child(content)
    }

    fn section_label(&self, text: &'static str, cx: &App) -> gpui::Div {
        let p = *pal(cx);
        div()
            .text_size(px(10.0))
            .font_weight(FontWeight::BOLD)
            .text_color(p.fg_faint)
            .child(text)
    }

    fn meta_line(&self, label: &'static str, value: String, cx: &App) -> gpui::Div {
        let p = *pal(cx);
        div()
            .flex()
            .items_center()
            .justify_between()
            .gap_3()
            .py(px(3.0))
            .child(
                div()
                    .text_size(px(11.5))
                    .text_color(p.fg_dim)
                    .child(label),
            )
            .child(
                div()
                    .font_family("Menlo")
                    .min_w_0()
                    .truncate()
                    .text_size(px(10.5))
                    .text_color(p.fg)
                    .child(SharedString::from(value)),
            )
    }

    /* 路径行：复制 / 显示。 */
    fn path_row(
        &self,
        id_seed: String,
        path: &str,
        empty_text: &str,
        show_in_finder: bool,
        cx: &Context<Self>,
    ) -> gpui::Div {
        let pal_p = *pal(cx);
        let p_fg_dim = pal_p.fg_dim;
        let p_fg_faint = pal_p.fg_faint;
        let p_link = pal_p.link;
        let p_fg_bright = pal_p.fg_bright;
        let has_path = !path.is_empty();
        div()
            .flex()
            .items_center()
            .gap_2()
            .py(px(5.0))
            .child(
                div()
                    .min_w_0()
                    .flex_1()
                    .truncate()
                    .font_family("Menlo")
                    .text_size(px(10.5))
                    .text_color(if has_path { p_fg_dim } else { p_fg_faint })
                    .child(SharedString::from(if has_path {
                        path.to_string()
                    } else {
                        empty_text.to_string()
                    })),
            )
            .when(has_path, |row| {
                row.child(
                    div()
                        .id(SharedString::from(format!("{}-copy", id_seed)))
                        .font_family("Menlo")
                        .text_size(px(9.5))
                        .text_color(p_link)
                        .hover(|s| s.text_color(p_fg_bright))
                        .on_click({
                            let copied = path.to_string();
                            cx.listener(move |state, _e, _w, cx| {
                                cx.write_to_clipboard(gpui::ClipboardItem::new_string(copied.clone()));
                                state.set_message(MessageKind::Ok, "已复制路径");
                                cx.notify();
                            })
                        })
                        .child("复制"),
                )
            })
            .when(has_path && show_in_finder, |row| {
                row.child(
                    div()
                        .id(SharedString::from(format!("{}-show", id_seed)))
                        .font_family("Menlo")
                        .text_size(px(9.5))
                        .text_color(p_link)
                        .hover(|s| s.text_color(p_fg_bright))
                        .on_click({
                            let opened = path.to_string();
                            cx.listener(move |state, _e, _w, cx| {
                                reveal_in_file_manager(&opened);
                                state.set_message(MessageKind::Ok, "已在文件管理器中显示");
                                cx.notify();
                            })
                        })
                        .child("显示"),
                )
            })
    }

    /* 技能详情面板。 */
    fn render_detail(&self, entry: &CatalogLine, cx: &Context<Self>) -> gpui::AnyElement {
        let p = *pal(cx);
        let unit_path = if entry.stored && !self.store_home.is_empty() {
            format!("{}/units/{}", self.store_home, entry.id)
        } else {
            String::new()
        };

        let pills: gpui::AnyElement = if entry.agents.is_empty() {
            div()
                .text_size(px(11.0))
                .text_color(p.fg_faint)
                .child("仅范围 all 时可见")
                .into_any_element()
        } else {
            div()
                .flex()
                .flex_wrap()
                .gap_1p5()
                .children(entry.agents.iter().map(|id| {
                    div()
                        .flex()
                        .items_center()
                        .gap_1p5()
                        .px_2()
                        .py(px(3.0))
                        .rounded(px(3.0))
                        .bg(p.input)
                        .child(div().size(px(6.0)).rounded_full().bg(self.agent_color(id)))
                        .child(
                            div()
                                .text_size(px(11.0))
                                .text_color(p.fg)
                                .child(SharedString::from(self.agent_label(id))),
                        )
                }))
                .into_any_element()
        };

        let mut sources_block = div().flex().flex_col().gap_0p5();
        for (i, source) in entry.sources.iter().take(4).enumerate() {
            sources_block = sources_block.child(self.path_row(
                format!("src-{}", i),
                source,
                "未记录",
                i == 0,
                cx,
            ));
        }
        if entry.sources.len() > 4 {
            sources_block = sources_block.child(
                div()
                    .pt_1()
                    .font_family("Menlo")
                    .text_size(px(9.0))
                    .text_color(p.fg_faint)
                    .child(SharedString::from(format!(
                        "+{} 个位置",
                        entry.sources.len() - 4
                    ))),
            );
        }

        div()
            .id("detail")
            .w(px(DETAIL_WIDTH))
            .flex_none()
            .h_full()
            .bg(p.side)
            .border_l_1()
            .border_color(p.line)
            .px(px(18.0))
            .pt(px(14.0))
            .pb(px(20.0))
            .overflow_y_scrollbar()
            .flex()
            .flex_col()
            .gap_3()
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap_2()
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap_2()
                            .child(
                                div()
                                    .px_2()
                                    .py(px(2.0))
                                    .rounded(px(3.0))
                                    .text_size(px(10.0))
                                    .font_weight(FontWeight::MEDIUM)
                                    .text_color(if entry.stored { p.green } else { p.amber })
                                    .bg(if entry.stored {
                                        with_alpha(p.green, 0.12)
                                    } else {
                                        with_alpha(p.amber, 0.12)
                                    })
                                    .child(if entry.stored { "已入库" } else { "仅目录" }),
                            )
                            .child(div().flex_1())
                            .child(
                                div()
                                    .font_family("Menlo")
                                    .text_size(px(9.5))
                                    .text_color(p.fg_faint)
                                    .child(SharedString::from(
                                        entry.id.chars().take(12).collect::<String>(),
                                    )),
                            )
                            .child(
                                div()
                                    .id("detail-close")
                                    .w(px(20.0))
                                    .h(px(20.0))
                                    .flex()
                                    .items_center()
                                    .justify_center()
                                    .rounded(px(3.0))
                                    .text_color(p.fg_dim)
                                    .hover(|s| s.bg(p.hover).text_color(p.fg_bright))
                                    .on_click(cx.listener(move |state, _e, _w, cx| {
                                        state.selected = None;
                                        cx.notify();
                                    }))
                                    .child(Icon::new(IconName::Close).with_size(px(12.0))),
                            ),
                    )
                    .child(
                        div()
                            .text_size(px(19.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .line_height(px(26.0))
                            .text_color(p.fg_bright)
                            .child(SharedString::from(entry.name.clone())),
                    )
                    .child(
                        div()
                            .text_size(px(12.0))
                            .line_height(px(19.0))
                            .text_color(p.fg_dim)
                            .child(SharedString::from(if entry.description.is_empty() {
                                "暂无描述".to_string()
                            } else {
                                entry.description.clone()
                            })),
                    ),
            )
            .child(self.section_panel(
                "元信息",
                div()
                    .flex()
                    .flex_col()
                    .child(self.meta_line("大小", format_bytes(entry.size), cx))
                    .child(self.meta_line(
                        "哈希",
                        entry.id.chars().take(16).collect::<String>(),
                        cx,
                    ))
                    .child(self.meta_line("副本数", entry.sources.len().to_string(), cx))
                    .into_any_element(),
                cx,
            ))
            .child(self.section_panel("可见 agent", pills, cx))
            .child(self.section_panel("来源位置", sources_block.into_any_element(), cx))
            .child(self.section_panel(
                "仓库位置",
                self.path_row("store".into(), &unit_path, "尚未复制进仓库", true, cx)
                    .into_any_element(),
                cx,
            ))
            .child(self.section_panel("文件清单", self.render_file_listing(entry, cx), cx))
            .into_any_element()
    }

    /* 「文件清单」内容：等宽字体，目录带「/」后缀，二级缩进。缓存由
     * render() 里的 ensure_file_listing 保证只读一次。 */
    fn render_file_listing(&self, entry: &CatalogLine, cx: &App) -> gpui::AnyElement {
        let p = *pal(cx);
        let Some(dir) = self.skill_base_dir(entry) else {
            return div()
                .text_size(px(11.0))
                .text_color(p.fg_faint)
                .child("无法读取目录")
                .into_any_element();
        };
        match self.file_listings.get(&dir) {
            Some(Some(entries)) if !entries.is_empty() => {
                let mut rows = div().flex().flex_col().gap(px(1.0));
                for e in entries {
                    let label = if e.is_dir {
                        format!("{}/", e.name)
                    } else {
                        e.name.clone()
                    };
                    rows = rows.child(
                        div()
                            .flex()
                            .items_center()
                            .pl(if e.depth == 2 { px(14.0) } else { px(0.0) })
                            .font_family("Menlo")
                            .text_size(px(10.5))
                            .text_color(if e.is_dir { p.fg_dim } else { p.fg_faint })
                            .truncate()
                            .child(SharedString::from(label)),
                    );
                }
                rows.into_any_element()
            }
            Some(Some(_)) => div()
                .text_size(px(11.0))
                .text_color(p.fg_faint)
                .child("空目录")
                .into_any_element(),
            _ => div()
                .text_size(px(11.0))
                .text_color(p.fg_faint)
                .child("无法读取目录")
                .into_any_element(),
        }
    }

    /* 卡片视图单卡。 */
    fn card(
        &self,
        ix: usize,
        skill_ix: usize,
        entry: &CatalogLine,
        selected: bool,
        cx: &mut Context<Self>,
    ) -> gpui::AnyElement {
        let p = *pal(cx);
        let mut dots = div().flex().items_center().gap_1().flex_none();
        let max_dots = 6usize;
        for agent in self.agents.iter().take(max_dots) {
            let lit = entry.agents.iter().any(|a| a == &agent.id);
            dots = dots.child(
                div()
                    .size(px(6.0))
                    .rounded_full()
                    .bg(if lit {
                        self.agent_color(&agent.id)
                    } else {
                        p.dot_off
                    }),
            );
        }
        if self.agents.len() > max_dots {
            dots = dots.child(
                div()
                    .text_size(px(9.0))
                    .text_color(p.fg_faint)
                    .child(SharedString::from(format!(
                        "+{}",
                        self.agents.len() - max_dots
                    ))),
            );
        }

        /* 卡片章：有封面图则用圆角小预览替换首字母；加载失败回退到首字母章。 */
        let badge_color = self.agent_color(
            entry.agents.first().map(|s| s.as_str()).unwrap_or(""),
        );
        let letter = entry.name.chars().next().unwrap_or('?').to_string();
        let fb_bg = p.badge_bg;
        let thumb = self
            .skill_base_dir(entry)
            .and_then(|dir| self.card_thumbs.get(&dir))
            .cloned()
            .flatten();
        let badge: gpui::AnyElement = if let Some(path) = thumb {
            img(path)
                .size(px(26.0))
                .rounded(px(5.0))
                .object_fit(ObjectFit::Cover)
                .with_fallback({
                    let letter = letter.clone();
                    move || initial_badge(letter.clone(), badge_color, fb_bg).into_any_element()
                })
                .into_any_element()
        } else {
            initial_badge(letter, badge_color, p.badge_bg).into_any_element()
        };

        div()
            .id(SharedString::from(format!("card-{}", skill_ix)))
            .flex_grow(1.0)
            .min_w(px(192.0))
            .max_w(px(264.0))
            .flex()
            .flex_col()
            .gap_2()
            .p(px(12.0))
            .rounded(px(6.0))
            .border_1()
            .border_color(if selected { p.focus } else { p.line })
            .bg(if selected { p.sel } else { p.card })
            .when(!selected, |c| {
                c.hover(|s| s.bg(p.hover).border_color(p.card_hover_border))
            })
            .on_click(cx.listener(move |state, _e, _w, cx| {
                state.select(Some(ix), cx);
            }))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .child(badge)
                    .child(
                        div()
                            .min_w_0()
                            .flex_1()
                            .truncate()
                            .text_size(px(12.5))
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(p.fg)
                            .child(SharedString::from(entry.name.clone())),
                    )
                    .child(
                        div()
                            .flex_none()
                            .px(px(5.0))
                            .rounded(px(3.0))
                            .font_family("Menlo")
                            .text_size(px(9.0))
                            .text_color(if entry.stored { p.green } else { p.amber })
                            .bg(if entry.stored {
                                with_alpha(p.green, 0.12)
                            } else {
                                with_alpha(p.amber, 0.12)
                            })
                            .child(if entry.stored { "已入库" } else { "仅目录" }),
                    ),
            )
            .child(
                div()
                    .truncate()
                    .text_size(px(11.0))
                    .text_color(p.fg_dim)
                    .child(SharedString::from(entry.description.clone())),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .child(dots)
                    .child(div().flex_1())
                    .child(
                        div()
                            .font_family("Menlo")
                            .flex_none()
                            .text_size(px(9.5))
                            .text_color(p.fg_faint)
                            .child(SharedString::from(format_bytes(entry.size))),
                    ),
            )
            .into_any_element()
    }

    /* 列表视图单行。 */
    fn list_row(
        &self,
        ix: usize,
        skill_ix: usize,
        entry: &CatalogLine,
        selected: bool,
        cx: &mut Context<Self>,
    ) -> gpui::AnyElement {
        let p = *pal(cx);
        let tile_color = self.agent_color(
            entry.agents.first().map(|s| s.as_str()).unwrap_or(""),
        );
        let mut dots = div().flex().items_center().gap_1().flex_none();
        let max_dots = 6usize;
        for agent in self.agents.iter().take(max_dots) {
            let lit = entry.agents.iter().any(|a| a == &agent.id);
            dots = dots.child(
                div()
                    .size(px(6.0))
                    .rounded_full()
                    .bg(if lit {
                        self.agent_color(&agent.id)
                    } else {
                        p.dot_off
                    }),
            );
        }
        if self.agents.len() > max_dots {
            dots = dots.child(
                div()
                    .text_size(px(9.0))
                    .text_color(p.fg_faint)
                    .child(SharedString::from(format!(
                        "+{}",
                        self.agents.len() - max_dots
                    ))),
            );
        }

        div()
            .id(SharedString::from(format!("row-{}", skill_ix)))
            .h(px(40.0))
            .w_full()
            .flex()
            .items_center()
            .gap_3()
            .px(px(12.0))
            .border_b_1()
            .border_color(p.row_border)
            .bg(if selected { p.sel } else { TRANSPARENT })
            .when(!selected, |r| r.hover(|s| s.bg(p.hover)))
            .on_click(cx.listener(move |state, _e, _w, cx| {
                state.select(Some(ix), cx);
            }))
            .child(
                div()
                    .w(px(2.0))
                    .h(px(20.0))
                    .flex_none()
                    .rounded_full()
                    .bg(if selected { tile_color } else { TRANSPARENT }),
            )
            .child(
                div()
                    .w(px(20.0))
                    .h(px(20.0))
                    .flex_none()
                    .flex()
                    .items_center()
                    .justify_center()
                    .rounded(px(5.0))
                    .font_family("Menlo")
                    .text_size(px(10.0))
                    .font_weight(FontWeight::BOLD)
                    .text_color(tile_color)
                    .bg(tint(tile_color))
                    .child(SharedString::from(
                        entry.name.chars().next().unwrap_or('?').to_string(),
                    )),
            )
            .child(
                div()
                    .min_w_0()
                    .flex_1()
                    .flex()
                    .flex_col()
                    .child(
                        div()
                            .truncate()
                            .text_size(px(12.5))
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(p.fg)
                            .child(SharedString::from(entry.name.clone())),
                    )
                    .child(
                        div()
                            .truncate()
                            .text_size(px(10.5))
                            .text_color(p.fg_dim)
                            .child(SharedString::from(entry.description.clone())),
                    ),
            )
            .child(dots)
            .child(
                div()
                    .w(px(50.0))
                    .flex_none()
                    .font_family("Menlo")
                    .text_size(px(9.5))
                    .text_color(p.fg_faint)
                    .child(SharedString::from(format_bytes(entry.size))),
            )
            .child(
                div()
                    .w(px(52.0))
                    .flex_none()
                    .text_size(px(10.0))
                    .text_color(if entry.stored { p.green } else { p.amber })
                    .child(if entry.stored { "已入库" } else { "仅目录" }),
            )
            .into_any_element()
    }
}

impl gpui::Render for AppState {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl gpui::IntoElement {
        let p = *pal(cx);
        /* kit Input 持有过滤文本；每次渲染同步。 */
        if let Some(state) = &self.search_state {
            self.query = state.read(cx).value().to_string();
        }

        let filtered = Rc::new(self.visible_indices());
        let effective = self.selected.filter(|i| *i < filtered.len());

        /* ============================================ title bar */
        let titlebar = TitleBar::new().child(
            div()
                .w_full()
                .flex()
                .items_center()
                .gap_3()
                .h_full()
                .pl(if cfg!(target_os = "macos") { px(60.0) } else { px(12.0) })
                .pr(px(10.0))
                .child(Icon::new(IconName::Star).with_size(px(16.0)).text_color(rgba_hex(0x4fc3f7, 1.0)))
                .child(
                    div()
                        .text_size(px(12.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(p.fg_bright)
                        .child("StarTide — 技能管理"),
                )
                .child(div().flex_1())
                .child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(2.0))
                        .child(self.icon_action(
                            "t-mode", gpui_kit::assets::IconName::SunMoon, "深色 / 浅色主题切换", cx,
                            |state, cx| {
                                state.theme_pref.dark = !state.theme_pref.dark;
                                apply_theme(state.theme_pref, cx);
                                save_pref(state.theme_pref);
                                let tip = if state.theme_pref.dark { "已切换到深色主题" } else { "已切换到浅色主题" };
                                state.set_message(MessageKind::Ok, tip);
                                cx.notify();
                            },
                        ))
                        .child(self.icon_action(
                            "t-accent", gpui_kit::assets::IconName::Palette, "强调色切换（青 / 蓝 / 紫 / 琥珀）", cx,
                            |state, cx| {
                                state.theme_pref.accent = (state.theme_pref.accent + 1) % ACCENTS.len();
                                apply_theme(state.theme_pref, cx);
                                save_pref(state.theme_pref);
                                let name = ACCENTS[state.theme_pref.accent].0;
                                state.set_message(MessageKind::Ok, format!("强调色：{}", name));
                                cx.notify();
                            },
                        ))
                        .child(self.icon_action(
                            "t-scan", IconName::HardDrive, "扫描本机", cx,
                            |state, cx| state.run("scan", vec!["scan", "--json"], true, cx),
                        ))
                        .child(self.icon_action(
                            "t-sync", IconName::RefreshCw, "同步", cx,
                            |state, cx| state.run("sync", vec!["sync", "--json"], true, cx),
                        ))
                        .child(self.icon_action(
                            "t-link", IconName::Network, "链接（预览）", cx,
                            |state, cx| {
                                let scope = if state.skill_view == SkillView::Cards { "own" } else { "all" };
                                state.run(
                                    "link",
                                    vec!["link", "--dry-run", "--json", "--scope", scope],
                                    true,
                                    cx,
                                );
                            },
                        ))
                        .child(self.icon_action(
                            "t-gc", IconName::Delete, "清理（预览）", cx,
                            |state, cx| state.run("gc", vec!["gc", "--dry-run", "--json"], false, cx),
                        ))
                        .child(self.confirm_button(cx)),
                ),
        );

        /* ============================================ search row */
        let stored_count = self.skills.iter().filter(|e| e.stored).count();
        let known_count = self.skills.len().saturating_sub(stored_count);

        let search_row = div()
            .flex()
            .items_center()
            .gap_4()
            .px(px(16.0))
            .h(px(44.0))
            .border_b_1()
            .border_color(p.line)
            .child(
                div()
                    .id("searchbox")
                    .flex_1()
                    .max_w(px(460.0))
                    .h(px(28.0))
                    .px(px(8.0))
                    .flex()
                    .items_center()
                    .gap(px(6.0))
                    .rounded(px(3.0))
                    .bg(p.input)
                    .on_mouse_down(gpui::MouseButton::Left, {
                        let st = self.search_state.clone();
                        move |_e, window, cx| {
                            if let Some(s) = &st {
                                s.read(cx).focus_handle(cx).clone().focus(window, cx);
                            }
                        }
                    })
                    .child(Icon::new(IconName::Search).with_size(px(13.0)).text_color(p.fg_faint))
                    .children(self.search_state.iter().map(|state| {
                        Input::new(state).appearance(false).h_full().flex_1()
                    })),
            )
            .child(
                div()
                    .text_size(px(11.0))
                    .text_color(p.fg_faint)
                    .child(SharedString::from(format!(
                        "{}/{}",
                        filtered.len(),
                        self.skills.len()
                    ))),
            )
            .child(div().flex_1())
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(6.0))
                    .child(self.tab_chip(
                        "tab-all".into(), "全部", self.skills.len(),
                        self.status_tab == StatusTab::All, cx,
                        |state, cx| { state.status_tab = StatusTab::All; state.selected = None; cx.notify(); },
                    ))
                    .child(self.tab_chip(
                        "tab-in".into(), "已入库", stored_count,
                        self.status_tab == StatusTab::Stored, cx,
                        |state, cx| { state.status_tab = StatusTab::Stored; state.selected = None; cx.notify(); },
                    ))
                    .child(self.tab_chip(
                        "tab-out".into(), "仅目录", known_count,
                        self.status_tab == StatusTab::Known, cx,
                        |state, cx| { state.status_tab = StatusTab::Known; state.selected = None; cx.notify(); },
                    ))
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(2.0))
                            .ml(px(6.0))
                            .child(self.view_toggle(
                                "v-cards", IconName::LayoutDashboard, self.skill_view == SkillView::Cards, cx,
                                |state, _cx| state.skill_view = SkillView::Cards,
                            ))
                            .child(self.view_toggle(
                                "v-list", IconName::Menu, self.skill_view == SkillView::List, cx,
                                |state, _cx| state.skill_view = SkillView::List,
                            )),
                    ),
            );

        /* ================================================ agent tree */
        let mut tree = div()
            .id("agent-tree")
            .w(px(208.0))
            .flex_none()
            .h_full()
            .bg(p.side)
            .border_r_1()
            .border_color(p.line)
            .overflow_y_scrollbar()
            .child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .h(px(24.0))
                    .px(px(10.0))
                    .child(
                        div()
                            .text_size(px(10.5))
                            .font_weight(FontWeight::BOLD)
                            .text_color(p.fg_bright)
                            .child("智能体"),
                    )
                    .child(
                        div()
                            .font_family("Menlo")
                            .text_size(px(9.0))
                            .text_color(p.fg_faint)
                            .child(SharedString::from(self.agents.len().to_string())),
                    ),
            );
        tree = tree.child(self.agent_row(
            "agent-all".into(),
            "全部".into(),
            "✦".into(),
            p.accent,
            filtered.len(),
            self.agent_filter.is_none(),
            cx,
            |state, cx| {
                state.agent_filter = None;
                state.selected = None;
                cx.notify();
            },
        ));
        /* 共享目录 agents 置顶在全部下面，其余按名称排序。 */
        let mut ordered: Vec<&AgentJson> = Vec::new();
        if let Some(shared) = self.agents.iter().find(|a| a.id == "agents") {
            ordered.push(shared);
        }
        let mut rest: Vec<&AgentJson> = self
            .agents
            .iter()
            .filter(|a| a.id != "agents")
            .collect();
        rest.sort_by(|a, b| a.label.cmp(&b.label));
        ordered.extend(rest);

        for agent in ordered {
            let n = filtered
                .iter()
                .filter(|i| self.skills[**i].agents.iter().any(|a| a == &agent.id))
                .count();
            let label = if agent.label.is_empty() { agent.id.clone() } else { agent.label.clone() };
            let initial = label.chars().next().unwrap_or('•').to_string();
            let color = self.agent_color(&agent.id);
            let is_active = self.agent_filter.as_deref() == Some(agent.id.as_str());
            tree = tree.child(self.agent_row(
                format!("agent-{}", agent.id),
                label,
                initial,
                color,
                n,
                is_active,
                cx,
                {
                    let id = agent.id.clone();
                    move |state, cx| {
                        state.agent_filter =
                            if state.agent_filter.as_deref() == Some(id.as_str()) {
                                None
                            } else {
                                Some(id.clone())
                            };
                        state.selected = None;
                        cx.notify();
                    }
                },
            ));
        }

        /* ================================================== centre */
        let centre: gpui::AnyElement = if filtered.is_empty() {
            div()
                .flex_1()
                .flex()
                .flex_col()
                .items_center()
                .justify_center()
                .gap_2()
                .child(Icon::new(IconName::Inbox).with_size(px(30.0)).text_color(p.fg_faint))
                .child(
                    div()
                        .text_size(px(13.0))
                        .text_color(p.fg_dim)
                        .child(if self.skills.is_empty() {
                            "目录还是空的"
                        } else {
                            "没有匹配的技能"
                        }),
                )
                .into_any_element()
        } else if self.skill_view == SkillView::Cards {
            let mut cards = div().flex().flex_wrap().gap(px(10.0));
            for (ix, i) in filtered.iter().enumerate() {
                /* 先克隆条目：ensure_* 需要可变借用自身（更新缓存）。 */
                if let Some(entry) = self.skills.get(*i).cloned() {
                    self.ensure_card_thumb(&entry, cx);
                    cards = cards.child(self.card(ix, *i, &entry, effective == Some(ix), cx));
                }
            }
            div()
                .id("cards-scroll")
                .p(px(12.0))
                .overflow_y_scrollbar()
                .child(cards)
                .into_any_element()
        } else {
            let mut rows = div().flex().flex_col();
            for (ix, i) in filtered.iter().enumerate() {
                if let Some(entry) = self.skills.get(*i) {
                    rows = rows.child(self.list_row(ix, *i, entry, effective == Some(ix), cx));
                }
            }
            div().id("rows-scroll").overflow_y_scrollbar().child(rows).into_any_element()
        };

        let centre = div()
            .flex_1()
            .min_w_0()
            .flex()
            .flex_col()
            .bg(TRANSPARENT)
            .border_l_1()
            .border_color(p.line)
            .overflow_hidden()
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .h(px(30.0))
                    .px(px(12.0))
                    .border_b_1()
                    .border_color(p.line)
                    .child(
                        div()
                            .text_size(px(11.5))
                            .text_color(p.fg_dim)
                            .child(SharedString::from(match &self.agent_filter {
                                Some(id) => format!(
                                    "当前：{} · {} 个技能",
                                    self.agent_label(id),
                                    filtered.len()
                                ),
                                None => format!("全部 agent · {} 个技能", filtered.len()),
                            })),
                    ),
            )
            .child(centre);

        /* ================================================== detail */
        let detail: gpui::AnyElement = match effective.map(|i| filtered[i]) {
            Some(skill_ix) => {
                /* 文件清单缓存：选中变化时读一次，其余帧只查表。 */
                if let Some(entry) = self.skills.get(skill_ix).cloned() {
                    self.ensure_file_listing(&entry);
                    self.render_detail(&entry, cx)
                } else {
                    div().w(px(0.0)).flex_none().into_any_element()
                }
            }
            _ => div().w(px(0.0)).flex_none().into_any_element(),
        };

        /* =============================================== status bar */
        let status_bar = div()
            .h(px(24.0))
            .flex_none()
            .flex()
            .items_center()
            .gap_3()
            .px(px(12.0))
            .bg(p.statusbar_bg)
            .border_t_1()
            .border_color(p.line)
            .text_color(p.fg_dim)
            .text_size(px(11.0))
            .child(SharedString::from(format!(
                "✦ skillctl · {} 技能 · {} agent · {}",
                self.skills.len(),
                self.agents.len(),
                format_bytes(self.store_bytes)
            )))
            .child(div().flex_1())
            .child(
                div()
                    .font_family("Menlo")
                    .text_size(px(9.5))
                    .child(SharedString::from(format!(
                        "↑↓ 选择 · ↩ 详情 · {}K 搜索",
                        modifier_label()
                    ))),
            );

        /* ==================================================== root */
        div()
            .flex()
            .flex_col()
            .size_full()
            .bg(TRANSPARENT)
            .text_color(p.fg)
            .font_family(ui_font())
            .on_key_down(cx.listener(Self::on_root_key))
            .child(titlebar)
            .child(search_row)
            .child(
                div()
                    .flex()
                    .flex_1()
                    .min_h_0()
                    .child(tree)
                    .child(centre)
                    .child(detail),
            )
            .child(status_bar)
    }
}

/* --------------------------------------------------------------- tests */

#[cfg(test)]
mod fs_read_tests {
    use super::*;

    fn temp_root(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("startide-fs-{}-{}", tag, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[core::prelude::v1::test]
    fn listing_sorts_dirs_first_and_depth_two() {
        let root = temp_root("listing-sort");
        std::fs::create_dir_all(root.join("sub")).unwrap();
        std::fs::write(root.join("sub/inner.txt"), "x").unwrap();
        std::fs::write(root.join("b.txt"), "x").unwrap();
        std::fs::write(root.join("a.txt"), "x").unwrap();

        let entries = read_file_listing(&root).unwrap();
        let names: Vec<(&str, usize)> = entries
            .iter()
            .map(|e| (e.name.as_str(), e.depth))
            .collect();
        assert_eq!(
            names,
            vec![("sub", 1), ("inner.txt", 2), ("a.txt", 1), ("b.txt", 1)]
        );
        assert!(entries[0].is_dir);
        assert!(!entries[1].is_dir);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[core::prelude::v1::test]
    fn listing_caps_at_max_entries() {
        let root = temp_root("listing-cap");
        for i in 0..80 {
            std::fs::write(root.join(format!("f{:02}.txt", i)), "x").unwrap();
        }
        let entries = read_file_listing(&root).unwrap();
        assert_eq!(entries.len(), LISTING_MAX_ENTRIES);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[core::prelude::v1::test]
    fn listing_none_on_missing_dir() {
        let root = temp_root("listing-missing");
        assert!(read_file_listing(&root.join("nope")).is_none());
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[core::prelude::v1::test]
    fn cover_prefers_named_stems_then_first_image() {
        let root = temp_root("cover-priority");
        std::fs::write(root.join("random.png"), "x").unwrap();
        std::fs::write(root.join("icon.svg"), "x").unwrap();
        std::fs::write(root.join("preview.jpg"), "x").unwrap();
        std::fs::write(root.join("cover.png"), "x").unwrap();

        let cover = find_cover_image(&root).unwrap();
        assert_eq!(cover.file_name().unwrap(), "cover.png");

        std::fs::remove_file(root.join("cover.png")).unwrap();
        assert_eq!(
            find_cover_image(&root).unwrap().file_name().unwrap(),
            "preview.jpg"
        );

        std::fs::remove_file(root.join("preview.jpg")).unwrap();
        assert_eq!(
            find_cover_image(&root).unwrap().file_name().unwrap(),
            "icon.svg"
        );

        std::fs::remove_file(root.join("icon.svg")).unwrap();
        assert_eq!(
            find_cover_image(&root).unwrap().file_name().unwrap(),
            "random.png"
        );
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[core::prelude::v1::test]
    fn cover_skips_dot_dirs_and_over_limit_files() {
        let root = temp_root("cover-skip");
        std::fs::create_dir_all(root.join(".hidden")).unwrap();
        std::fs::write(root.join(".hidden/cover.png"), "x").unwrap();
        std::fs::write(root.join(".cover.png"), "x").unwrap();
        let big = vec![0u8; (COVER_MAX_FILE_BYTES + 1) as usize];
        std::fs::write(root.join("huge.png"), &big).unwrap();
        std::fs::write(root.join("ok.png"), "x").unwrap();

        let cover = find_cover_image(&root).unwrap();
        assert_eq!(cover.file_name().unwrap(), "ok.png");
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[core::prelude::v1::test]
    fn cover_scans_second_level() {
        let root = temp_root("cover-depth2");
        std::fs::create_dir_all(root.join("assets")).unwrap();
        std::fs::write(root.join("assets/cover.png"), "x").unwrap();
        let cover = find_cover_image(&root).unwrap();
        assert!(cover.ends_with("assets/cover.png"));
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[core::prelude::v1::test]
    fn cover_none_without_images() {
        let root = temp_root("cover-none");
        std::fs::write(root.join("SKILL.md"), "x").unwrap();
        assert!(find_cover_image(&root).is_none());
        std::fs::remove_dir_all(&root).unwrap();
    }
}

fn main() {
    gpui_kit::application()
        .with_assets(Assets)
        .run(|cx| {
            init(cx);
            /* 主题：从偏好文件恢复（深/浅 × 强调色），并同步 kit 品牌色。 */
            apply_theme(load_pref(), cx);
            /* 默认窗口：侧栏 + 4 列卡片墙刚好铺满；其他尺寸下卡片等比自适应。 */
            let bounds = Bounds::centered(None, size(px(1320.0), px(780.0)), cx);
            open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Windowed(bounds)),
                    titlebar: Some(TitleBar::title_bar_options()),
                    window_background: WindowBackgroundAppearance::Blurred,
                    ..Default::default()
                },
                cx,
                |window, cx| {
                    let search_state = cx.new(|cx| {
                        let mut s = InputState::new(window, cx);
                        s.set_placeholder("按名称或用途搜索…", window, cx);
                        s
                    });
                    cx.new(|cx| {
                        let mut state = AppState::new();
                        state.search_state = Some(search_state);
                        if state.cli_ok {
                            state.refresh(cx);
                        } else {
                            state.set_message(
                                MessageKind::Err,
                                format!(
                                    "找不到 skillctl 引擎（{}）。运行 make 构建，或用 SKILLCTL_BIN 指定路径",
                                    state.cli.display()
                                ),
                            );
                        }
                        if let Some(s) = &state.search_state {
                            s.read(cx).focus_handle(cx).clone().focus(window, cx);
                        }
                        state
                    })
                },
            )
            .expect("open window");
        });
}
