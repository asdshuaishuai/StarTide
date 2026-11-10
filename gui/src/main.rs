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
use std::path::{Path, PathBuf};
use std::process::Command;
use std::rc::Rc;

use gpui_kit::assets::Assets;
use gpui_kit::component::badge::Badge;
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::status_bar::StatusBar;
use gpui_kit::component::TitleBar;
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

const BG_TITLE: Rgba = rgba_hex(0x3c3c3c, 1.0);
const BG_SIDE: Rgba = rgba_hex(0x252526, 1.0);
const BG_EDIT: Rgba = rgba_hex(0x1e1e1e, 1.0);
const BG_INPUT: Rgba = rgba_hex(0x3c3c3c, 1.0);
const BG_CARD: Rgba = rgba_hex(0x252526, 1.0);
const BG_HOVER: Rgba = rgba_hex(0x2a2d2e, 1.0);
const BG_SEL: Rgba = rgba_hex(0x094771, 1.0);
const BG_BADGE: Rgba = rgba_hex(0x4d4d4d, 1.0);
const LINE: Rgba = rgba_hex(0x333333, 1.0);
const FG: Rgba = rgba_hex(0xcccccc, 1.0);
const FG_BRIGHT: Rgba = rgba_hex(0xe7e7e7, 1.0);
const FG_DIM: Rgba = rgba_hex(0x858585, 1.0);
const FG_FAINT: Rgba = rgba_hex(0x6a6a6a, 1.0);
const ACCENT: Rgba = rgba_hex(0x007acc, 1.0);
const FOCUS_RING: Rgba = rgba_hex(0x007fd4, 1.0);
const LINK: Rgba = rgba_hex(0x3794ff, 1.0);
const GREEN: Rgba = rgba_hex(0x89d185, 1.0);
const AMBER: Rgba = rgba_hex(0xcca700, 1.0);
const TRANSPARENT: Rgba = rgba_hex(0x000000, 0.0);

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

const ROW_HEIGHT: f32 = 40.0;
const DETAIL_WIDTH: f32 = 312.0;
const TREE_WIDTH: f32 = 208.0;

/* --------------------------------------------------------- cli output */

#[derive(Deserialize)]
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
    unlink_armed: bool,

    busy: bool,
    msg_kind: MessageKind,
    msg: SharedString,

    search_state: Option<Entity<InputState>>,
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
    fn new(cx: &mut Context<Self>) -> Self {
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
            unlink_armed: false,
            busy: false,
            msg_kind: MessageKind::Idle,
            msg: "启动中".into(),
            search_state: None,
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

    /* ------------------------------------------------------- cli access */

    fn refresh(&mut self, cx: &mut Context<Self>) {
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

impl AppState {
    /* 视图标题栏的 ghost 图标按钮。 */
    fn icon_action(
        &self,
        id: &'static str,
        icon: IconName,
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

    /* 两态确认按钮（还原）。 */
    fn confirm_button(&self, cx: &Context<Self>) -> Button {
        let view = cx.entity();
        let armed = self.unlink_armed;
        Button::new("unlink")
            .icon(IconName::Undo)
            .label(if armed { "确认？" } else { "还原" })
            .danger()
            .tooltip("还原所有 agent 的原始目录")
            .disabled(self.busy || !self.cli_ok)
            .on_click(move |_e, _window, cx| {
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
        div()
            .id(SharedString::from(id))
            .h(px(22.0))
            .px(px(8.0))
            .flex()
            .items_center()
            .gap(px(5.0))
            .rounded(px(3.0))
            .text_size(px(11.5))
            .bg(if active { BG_SEL } else { TRANSPARENT })
            .text_color(if active { FG_BRIGHT } else { FG_DIM })
            .when(!active, |c| c.hover(|s| s.bg(BG_HOVER).text_color(FG)))
            .on_click(cx.listener(move |state, _e, _w, cx| on_click(state, cx)))
            .child(label)
            .child(
                div()
                    .font_family("Menlo")
                    .text_size(px(9.5))
                    .text_color(if active { FG_BRIGHT } else { FG_FAINT })
                    .child(SharedString::from(count.to_string())),
            )
    }

    /* 智能体树行。 */
    fn agent_row(
        &self,
        id: String,
        label: String,
        dot: Rgba,
        count: usize,
        active: bool,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(SharedString::from(id))
            .h(px(26.0))
            .pl(px(20.0))
            .pr(px(10.0))
            .flex()
            .items_center()
            .gap(px(8.0))
            .bg(if active { BG_SEL } else { TRANSPARENT })
            .when(!active, |r| r.hover(|s| s.bg(BG_HOVER)))
            .on_click(cx.listener(move |state, _e, _w, cx| on_click(state, cx)))
            .child(div().size(px(9.0)).flex_none().rounded_full().bg(dot))
            .child(
                div()
                    .min_w_0()
                    .flex_1()
                    .truncate()
                    .text_size(px(12.5))
                    .text_color(if active { FG_BRIGHT } else { FG })
                    .child(SharedString::from(label)),
            )
            .child(
                div()
                    .font_family("Menlo")
                    .flex_none()
                    .text_size(px(10.0))
                    .text_color(if active { FG_BRIGHT } else { FG_FAINT })
                    .child(SharedString::from(count.to_string())),
            )
    }

    fn section_label(&self, text: &'static str) -> gpui::Div {
        div()
            .text_size(px(10.0))
            .font_weight(FontWeight::BOLD)
            .text_color(FG_FAINT)
            .child(text)
    }

    fn meta_line(&self, label: &'static str, value: String) -> gpui::Div {
        div()
            .flex()
            .items_center()
            .justify_between()
            .gap_3()
            .py(px(3.0))
            .child(
                div()
                    .text_size(px(11.5))
                    .text_color(FG_DIM)
                    .child(label),
            )
            .child(
                div()
                    .font_family("Menlo")
                    .min_w_0()
                    .truncate()
                    .text_size(px(10.5))
                    .text_color(FG)
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
                    .text_color(if has_path { FG_DIM } else { FG_FAINT })
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
                        .text_color(LINK)
                        .hover(|s| s.text_color(FG_BRIGHT))
                        .on_click({
                            let p = path.to_string();
                            cx.listener(move |state, _e, _w, cx| {
                                cx.write_to_clipboard(gpui::ClipboardItem::new_string(p.clone()));
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
                        .text_color(LINK)
                        .hover(|s| s.text_color(FG_BRIGHT))
                        .on_click({
                            let p = path.to_string();
                            cx.listener(move |state, _e, _w, cx| {
                                reveal_in_file_manager(&p);
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
        let unit_path = if entry.stored && !self.store_home.is_empty() {
            format!("{}/units/{}", self.store_home, entry.id)
        } else {
            String::new()
        };

        let pills: gpui::AnyElement = if entry.agents.is_empty() {
            div()
                .text_size(px(11.0))
                .text_color(FG_FAINT)
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
                        .bg(BG_INPUT)
                        .child(div().size(px(6.0)).rounded_full().bg(self.agent_color(id)))
                        .child(
                            div()
                                .text_size(px(11.0))
                                .text_color(FG)
                                .child(SharedString::from(self.agent_label(id))),
                        )
                }))
                .into_any_element()
        };

        let mut sources_block = div()
            .flex()
            .flex_col()
            .gap_0p5()
            .child(self.section_label("来源位置"));
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
                    .text_color(FG_FAINT)
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
            .overflow_y_scroll()
            .bg(BG_SIDE)
            .border_l_1()
            .border_color(LINE)
            .px(px(20.0))
            .pt(px(16.0))
            .pb(px(24.0))
            .flex()
            .flex_col()
            .gap_4()
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
                                    .text_color(if entry.stored { GREEN } else { AMBER })
                                    .bg(if entry.stored {
                                        rgba_hex(0x89d185, 0.12)
                                    } else {
                                        rgba_hex(0xcca700, 0.12)
                                    })
                                    .child(if entry.stored { "已入库" } else { "仅目录" }),
                            )
                            .child(div().flex_1())
                            .child(
                                div()
                                    .font_family("Menlo")
                                    .text_size(px(9.5))
                                    .text_color(FG_FAINT)
                                    .child(SharedString::from(
                                        entry.id.chars().take(12).collect::<String>(),
                                    )),
                            ),
                    )
                    .child(
                        div()
                            .text_size(px(19.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .line_height(px(26.0))
                            .text_color(FG_BRIGHT)
                            .child(SharedString::from(entry.name.clone())),
                    )
                    .child(
                        div()
                            .text_size(px(12.0))
                            .line_height(px(19.0))
                            .text_color(FG_DIM)
                            .child(SharedString::from(if entry.description.is_empty() {
                                "暂无描述".to_string()
                            } else {
                                entry.description.clone()
                            })),
                    ),
            )
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap_1()
                    .child(self.section_label("元信息"))
                    .child(self.meta_line("大小", format_bytes(entry.size)))
                    .child(self.meta_line(
                        "哈希",
                        entry.id.chars().take(16).collect::<String>(),
                    ))
                    .child(self.meta_line("副本数", entry.sources.len().to_string())),
            )
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap_2()
                    .child(self.section_label("可见 agent"))
                    .child(pills),
            )
            .child(sources_block)
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap_0p5()
                    .child(self.section_label("仓库位置"))
                    .child(self.path_row(
                        "store".into(),
                        &unit_path,
                        "尚未复制进仓库",
                        true,
                        cx,
                    )),
            )
            .into_any_element()
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
                        rgba_hex(0x3a3a3a, 1.0)
                    }),
            );
        }
        if self.agents.len() > max_dots {
            dots = dots.child(
                div()
                    .text_size(px(9.0))
                    .text_color(FG_FAINT)
                    .child(SharedString::from(format!(
                        "+{}",
                        self.agents.len() - max_dots
                    ))),
            );
        }

        div()
            .id(SharedString::from(format!("card-{}", skill_ix)))
            .w(px(232.0))
            .flex()
            .flex_col()
            .gap_2()
            .p(px(12.0))
            .rounded(px(6.0))
            .border_1()
            .border_color(if selected { FOCUS_RING } else { LINE })
            .bg(if selected { BG_SEL } else { BG_CARD })
            .when(!selected, |c| {
                c.hover(|s| s.bg(BG_HOVER).border_color(rgba_hex(0x4a4a4a, 1.0)))
            })
            .on_click(cx.listener(move |state, _e, _w, cx| {
                state.select(Some(ix), cx);
            }))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .child(
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
                            .text_color(self.agent_color(
                                entry.agents.first().map(|s| s.as_str()).unwrap_or(""),
                            ))
                            .bg(rgba_hex(0x2d2d2f, 1.0))
                            .child(SharedString::from(
                                entry.name.chars().next().unwrap_or('?').to_string(),
                            )),
                    )
                    .child(
                        div()
                            .min_w_0()
                            .flex_1()
                            .truncate()
                            .text_size(px(12.5))
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(FG)
                            .child(SharedString::from(entry.name.clone())),
                    )
                    .child(
                        div()
                            .flex_none()
                            .px(px(5.0))
                            .rounded(px(3.0))
                            .font_family("Menlo")
                            .text_size(px(9.0))
                            .text_color(if entry.stored { GREEN } else { AMBER })
                            .bg(if entry.stored {
                                rgba_hex(0x89d185, 0.12)
                            } else {
                                rgba_hex(0xcca700, 0.12)
                            })
                            .child(if entry.stored { "已入库" } else { "仅目录" }),
                    ),
            )
            .child(
                div()
                    .truncate()
                    .text_size(px(11.0))
                    .text_color(FG_DIM)
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
                            .text_color(FG_FAINT)
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
                        rgba_hex(0x3a3a3a, 1.0)
                    }),
            );
        }
        if self.agents.len() > max_dots {
            dots = dots.child(
                div()
                    .text_size(px(9.0))
                    .text_color(FG_FAINT)
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
            .border_color(rgba_hex(0x2b2b2b, 1.0))
            .bg(if selected { BG_SEL } else { BG_EDIT })
            .when(!selected, |r| r.hover(|s| s.bg(BG_HOVER)))
            .on_click(cx.listener(move |state, _e, _w, cx| {
                state.select(Some(ix), cx);
            }))
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
                            .text_color(FG)
                            .child(SharedString::from(entry.name.clone())),
                    )
                    .child(
                        div()
                            .truncate()
                            .text_size(px(10.5))
                            .text_color(FG_DIM)
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
                    .text_color(FG_FAINT)
                    .child(SharedString::from(format_bytes(entry.size))),
            )
            .child(
                div()
                    .w(px(52.0))
                    .flex_none()
                    .text_size(px(10.0))
                    .text_color(if entry.stored { GREEN } else { AMBER })
                    .child(if entry.stored { "已入库" } else { "仅目录" }),
            )
            .into_any_element()
    }
}

impl gpui::Render for AppState {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl gpui::IntoElement {
        /* kit Input 持有过滤文本；每次渲染同步。 */
        if let Some(state) = &self.search_state {
            self.query = state.read(cx).value().to_string();
        }

        let filtered = Rc::new(self.visible_indices());
        let effective = self.selected.filter(|i| *i < filtered.len());

        /* ============================================ title bar */
        let titlebar = TitleBar::new().child(
            div()
                .flex()
                .items_center()
                .gap_3()
                .h_full()
                .pl(if cfg!(target_os = "macos") { px(78.0) } else { px(12.0) })
                .pr(px(10.0))
                .child(Icon::new(IconName::Star).with_size(px(16.0)).text_color(rgba_hex(0x4fc3f7, 1.0)))
                .child(
                    div()
                        .text_size(px(12.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(FG_BRIGHT)
                        .child("StarTide — 技能管理"),
                )
                .child(div().flex_1())
                .child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(2.0))
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
            .border_color(LINE)
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
                    .bg(BG_INPUT)
                    .on_mouse_down(gpui::MouseButton::Left, {
                        let st = self.search_state.clone();
                        move |_e, window, cx| {
                            if let Some(s) = &st {
                                s.read(cx).focus_handle(cx).clone().focus(window, cx);
                            }
                        }
                    })
                    .child(Icon::new(IconName::Search).with_size(px(13.0)).text_color(FG_FAINT))
                    .children(self.search_state.iter().map(|state| {
                        Input::new(state).appearance(false).h_full().flex_1()
                    })),
            )
            .child(
                div()
                    .text_size(px(11.0))
                    .text_color(FG_FAINT)
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
                    )),
            );

        /* ================================================ agent tree */
        let mut tree = div()
            .id("agent-tree")
            .w(px(208.0))
            .flex_none()
            .h_full()
            .bg(BG_SIDE)
            .border_r_1()
            .border_color(LINE)
            .overflow_y_scroll()
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
                            .text_color(FG_BRIGHT)
                            .child("智能体"),
                    )
                    .child(
                        div()
                            .font_family("Menlo")
                            .text_size(px(9.0))
                            .text_color(FG_FAINT)
                            .child(SharedString::from(self.agents.len().to_string())),
                    ),
            );
        tree = tree.child(self.agent_row(
            "agent-all".into(),
            "全部".into(),
            rgba_hex(0x4fc3f7, 1.0),
            filtered.len(),
            self.agent_filter.is_none(),
            cx,
            |state, cx| {
                state.agent_filter = None;
                state.selected = None;
                cx.notify();
            },
        ));
        for agent in &self.agents {
            let n = filtered
                .iter()
                .filter(|i| self.skills[**i].agents.iter().any(|a| a == &agent.id))
                .count();
            tree = tree.child(self.agent_row(
                format!("agent-{}", agent.id),
                if agent.label.is_empty() { agent.id.clone() } else { agent.label.clone() },
                self.agent_color(&agent.id),
                n,
                self.agent_filter.as_deref() == Some(agent.id.as_str()),
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
                .child(Icon::new(IconName::Inbox).with_size(px(30.0)).text_color(FG_FAINT))
                .child(
                    div()
                        .text_size(px(13.0))
                        .text_color(FG_DIM)
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
                if let Some(entry) = self.skills.get(*i) {
                    cards = cards.child(self.card(ix, *i, entry, effective == Some(ix), cx));
                }
            }
            div().id("cards-scroll").overflow_y_scroll().p(px(12.0)).child(cards).into_any_element()
        } else {
            let mut rows = div().flex().flex_col();
            for (ix, i) in filtered.iter().enumerate() {
                if let Some(entry) = self.skills.get(*i) {
                    rows = rows.child(self.list_row(ix, *i, entry, effective == Some(ix), cx));
                }
            }
            div().id("rows-scroll").overflow_y_scroll().child(rows).into_any_element()
        };

        let centre = div()
            .flex_1()
            .min_w_0()
            .flex()
            .flex_col()
            .bg(BG_EDIT)
            .border_l_1()
            .border_color(LINE)
            .overflow_hidden()
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .h(px(30.0))
                    .px(px(12.0))
                    .border_b_1()
                    .border_color(LINE)
                    .child(
                        div()
                            .text_size(px(11.5))
                            .text_color(FG_DIM)
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
            Some(skill_ix) if self.skills.get(skill_ix).is_some() => {
                self.render_detail(&self.skills[skill_ix], cx)
            }
            _ => div().w(px(0.0)).flex_none().into_any_element(),
        };

        /* =============================================== status bar */
        let status_bar = div()
            .h(px(22.0))
            .flex_none()
            .flex()
            .items_center()
            .gap_3()
            .px(px(12.0))
            .bg(ACCENT)
            .text_color(rgba_hex(0xffffff, 1.0))
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
                        "{} 范围 · {}K 搜索",
                        if self.skill_view == SkillView::Cards { "卡片" } else { "列表" },
                        modifier_label()
                    ))),
            );

        /* ==================================================== root */
        div()
            .flex()
            .flex_col()
            .size_full()
            .bg(BG_EDIT)
            .text_color(FG)
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

fn main() {
    gpui_kit::application()
        .with_assets(Assets)
        .run(|cx| {
            init(cx);
            /* 深色主题：让 kit 组件（ghost 按钮 hover 等）融入 VS Code 风格。 */
            Theme::change(ThemeMode::Dark, None, cx);
            let bounds = Bounds::centered(None, size(px(1200.0), px(780.0)), cx);
            open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Windowed(bounds)),
                    titlebar: Some(TitleBar::title_bar_options()),
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
                        let mut state = AppState::new(cx);
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
