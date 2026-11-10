/*
 * StarTide - a calm home for your AI agent skills.
 *
 * A thin native shell around the skillctl engine. The C command line is
 * the only source of truth; this window spawns it, parses its --json
 * output and draws the result.
 *
 * Design language: soft floating cards on warm cream, white surfaces,
 * gentle shadows, one teal accent. Made for people, not terminals.
 *
 * Keyboard map:
 *   up/down (or j/k)  move the selection
 *   home / end        first / last entry
 *   enter             open or close the detail card
 *   /                 focus the search field
 *   esc               clear the search, or drop the selection
 */
use std::path::{Path, PathBuf};
use std::process::Command;
use std::rc::Rc;

use gpui_kit::assets::Assets;
use gpui_kit::component::badge::Badge;
use gpui_kit::component::button::{Button, ButtonCustomVariant, ButtonVariants};
use gpui_kit::component::command::{Command as CommandPalette, CommandItem, CommandState};
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::Sizable;
use gpui_kit::component::status_bar::StatusBar;
use gpui_kit::component::{Disableable, Icon, IconName, TitleBar};
use gpui_kit::*;
use gpui_kit::prelude::*;
use serde::Deserialize;

/* ------------------------------------------------------------ palette */

const fn rgba_hex(hex: u32, alpha: f32) -> Rgba {
    Rgba {
        r: ((hex >> 16) & 0xff) as f32 / 255.0,
        g: ((hex >> 8) & 0xff) as f32 / 255.0,
        b: (hex & 0xff) as f32 / 255.0,
        a: alpha,
    }
}

/* Warm cream ground, white surfaces. */
const CREAM: Rgba = rgba_hex(0xf5f1e8, 1.0);
const CARD: Rgba = rgba_hex(0xffffff, 1.0);
const CARD_HOVER: Rgba = rgba_hex(0xfaf8f2, 1.0);
const HAIRLINE: Rgba = rgba_hex(0xefeae0, 1.0);
const INK: Rgba = rgba_hex(0x24221d, 1.0);
const INK_MID: Rgba = rgba_hex(0x6f6a5e, 1.0);
const INK_FAINT: Rgba = rgba_hex(0xa8a192, 1.0);

/* One calm accent. */
const TEAL: Rgba = rgba_hex(0x0f8a7e, 1.0);
const TEAL_SOFT: Rgba = rgba_hex(0x0f8a7e, 0.08);
const GREEN: Rgba = rgba_hex(0x2f8f5b, 1.0);
const AMBER: Rgba = rgba_hex(0xb8862c, 1.0);
const RED: Rgba = rgba_hex(0xc0503f, 1.0);
const TRANSPARENT: Rgba = rgba_hex(0x000000, 0.0);

/* One friendly hue per agent. */
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

const ROW_HEIGHT: f32 = 74.0;
const DETAIL_WIDTH: f32 = 356.0;

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
    /* Which agents' views include this skill under scope "own",
     * computed by the CLI (catalog --json). */
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

/* Raw rows of `status --json`; a few fields belong to the contract even
 * where the window does not render them yet. */
#[allow(dead_code)]
#[derive(Deserialize)]
struct AgentJson {
    id: String,
    #[serde(default)]
    label: String,
    #[serde(default)]
    description: String,
    #[serde(default)]
    known: bool,
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
enum Scope {
    Own,
    All,
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
    scope: Scope,
    selected: Option<usize>,
    show_detail: bool,
    unlink_armed: bool,
    agents_expanded: bool,
    command_open: bool,
    command_state: Option<Entity<CommandState>>,
    search_state: Option<Entity<InputState>>,

    busy: bool,
    msg_kind: MessageKind,
    msg: SharedString,

    list_scroll: UniformListScrollHandle,
    root_focus: FocusHandle,
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

/* Locate the skillctl binary next to this executable, or on the PATH. */
fn find_cli() -> PathBuf {
    if let Ok(p) = std::env::var("SKILLCTL_BIN") {
        if !p.is_empty() {
            return PathBuf::from(p);
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        let mut dir = exe.parent().map(Path::to_path_buf);
        for _ in 0..6 {
            match dir {
                Some(d) => {
                    let cand = d.join("build").join("skillctl");
                    if cand.exists() {
                        return cand;
                    }
                    dir = d.parent().map(Path::to_path_buf);
                }
                None => break,
            }
        }
    }
    PathBuf::from("skillctl")
}

impl AppState {
    fn new(cx: &mut Context<Self>) -> Self {
        let cli = find_cli();
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
            scope: Scope::Own,
            selected: None,
            show_detail: false,
            unlink_armed: false,
            agents_expanded: false,
            command_open: false,
            command_state: None,
            search_state: None,
            busy: false,
            msg_kind: MessageKind::Idle,
            msg: "启动中".into(),
            list_scroll: UniformListScrollHandle::new(),
            root_focus: cx.focus_handle(),
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
        /* A detailed result message set by apply() must survive the
         * refresh; only idle or busy chatter is replaced here. */
        if matches!(self.msg_kind, MessageKind::Idle | MessageKind::Busy) {
            self.set_message(
                MessageKind::Idle,
                format!(
                    "{} 个技能 · 仓库 {} 个单元",
                    self.skills.len(),
                    self.store_units
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
                        format!("{} exited with {}", command, out.status)
                    } else {
                        err.lines().next().unwrap_or("failed").to_string()
                    })
                }
                Err(e) => Err(format!("could not run {}: {}", command, e)),
            };

            let _ = this.update(cx, |state, cx| {
                state.busy = false;
                match result {
                    Ok(text) => {
                        if !state.apply(command, &text) {
                            state.set_message(MessageKind::Ok, format!("{} 完成", command));
                        }
                    }
                    Err(err) => {
                        state.set_message(MessageKind::Err, err);
                    }
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
                    format!("scanned {} skills, {} unique by content", units, unique),
                );
                true
            }
            "link" => {
                let skills = v["skills"].as_i64().unwrap_or(0);
                self.set_message(MessageKind::Ok, format!("would link {} skills (dry run)", skills));
                true
            }
            "gc" => {
                let removed = v["removed"].as_i64().unwrap_or(0);
                self.set_message(
                    MessageKind::Ok,
                    format!("{} store units are unreferenced (dry run)", removed),
                );
                true
            }
            "sync" => {
                let imported = v["imported"].as_i64().unwrap_or(0);
                self.set_message(MessageKind::Ok, format!("synced, {} newly imported", imported));
                true
            }
            "unlink" => {
                let restored = v["restored"].as_i64().unwrap_or(0);
                self.set_message(MessageKind::Ok, format!("restored {} agent directories", restored));
                true
            }
            _ => false,
        }
    }

    /* -------------------------------------------------------- selection */

    fn visible_indices(&self) -> Vec<usize> {
        let query = self.query.to_lowercase();

        let agent_filter = if self.scope == Scope::All {
            None
        } else {
            self.agent_filter.clone()
        };

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
        if let Some(i) = ix {
            self.list_scroll.scroll_to_item(i, ScrollStrategy::Center);
        }
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
        if let Some(state) = &self.search_state {
            self.query = state.read(cx).value().to_string();
        }
        let key = event.keystroke.key.as_str();
        let search_handle = self
            .search_state
            .as_ref()
            .map(|s| s.read(cx).focus_handle(cx).clone());
        let search_focused = search_handle
            .as_ref()
            .is_some_and(|h| h.is_focused(window));
        match key {
            "k" if event.keystroke.modifiers.platform => {
                self.command_open = !self.command_open;
                self.command_state = if self.command_open {
                    Some(cx.new(|cx| CommandState::new(window, cx)))
                } else {
                    None
                };
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
                if search_focused {
                    let first = self.visible_indices().first().copied();
                    self.select(first, cx);
                    self.root_focus.focus(window, cx);
                } else {
                    let count = self.visible_indices().len();
                    let effective = self.selected.filter(|i| *i < count);
                    if effective.is_some() {
                        self.show_detail = !self.show_detail;
                        cx.notify();
                    }
                }
            }
            "escape" => {
                if self.command_open {
                    self.command_open = false;
                    self.command_state = None;
                    cx.notify();
                } else if search_focused {
                    self.root_focus.focus(window, cx);
                    cx.notify();
                } else if self.selected.is_some() {
                    self.selected = None;
                    self.show_detail = false;
                    cx.notify();
                }
            }
            "/" => {
                if !search_focused {
                    if let Some(h) = &search_handle {
                        h.focus(window, cx);
                    }
                    cx.notify();
                }
            }
            _ => {}
        }
    }
}

/* -------------------------------------------------------------- view */

/* 三平台字体栈：macOS 用 Avenir Next，Windows 用 Segoe UI，Linux 用 Noto Sans。 */
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
        let _ = std::process::Command::new("open").arg("-R").arg(path).spawn();
    } else if cfg!(target_os = "windows") {
        let _ = std::process::Command::new("explorer")
            .arg(format!("/select,\"{}\"", path))
            .spawn();
    } else {
        let parent = std::path::Path::new(path)
            .parent()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        let _ = std::process::Command::new("xdg-open").arg(parent).spawn();
    }
}

/* 平台快捷键标签：macOS 是 ⌘，其他是 Ctrl。 */
fn modifier_label() -> &'static str {
    if cfg!(target_os = "macos") {
        "⌘"
    } else {
        "Ctrl"
    }
}

/* 在品牌色上叠一层透明度，做成柔和底色。 */
fn tint(c: Rgba) -> Rgba {
    Rgba { r: c.r, g: c.g, b: c.b, a: 0.12 }
}

impl AppState {
    /* A soft action button, powered by the kit's Button with the brand teal. */
    fn action_button(
        &self,
        id: &'static str,
        label: &'static str,
        icon: IconName,
        variant: impl Fn(Button) -> Button,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> Button {
        let armed = id == "unlink" && self.unlink_armed;
        let disabled = self.busy || !self.cli_ok;
        let text: SharedString = if armed { "确认？" } else { label }.into();
        let base = Button::new(id).icon(icon).label(text).disabled(disabled);
        let base = if armed { base.danger() } else { variant(base) };
        let view = cx.entity();
        base.on_click(move |_e, _window, cx| {
            let _ = view.update(cx, |state, cx| on_click(state, cx));
        })
    }

    /* A small filter pill used by the tabs and the scope switch. */
    fn chip(
        &self,
        id: String,
        label: String,
        count: Option<String>,
        active: bool,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(SharedString::from(id))
            .h_6()
            .px_2p5()
            .flex()
            .items_center()
            .gap_1p5()
            .rounded_full()
            .text_size(px(11.5))
            .bg(if active { TEAL_SOFT } else { TRANSPARENT })
            .text_color(if active { TEAL } else { INK_MID })
            .font_weight(if active {
                FontWeight::MEDIUM
            } else {
                FontWeight::NORMAL
            })
            .when(!active, |c| c.hover(|s| s.bg(HAIRLINE).text_color(INK)))
            .on_click(cx.listener(move |state, _e, _w, cx| on_click(state, cx)))
            .child(SharedString::from(label))
            .when(count.is_some(), |t| {
                t.child(
                    div()
                        .font_family("Menlo")
                        .text_size(px(10.0))
                        .text_color(if active { TEAL } else { INK_FAINT })
                        .child(SharedString::from(count.unwrap_or_default())),
                )
            })
    }

    /* The brand mark: one Lucide star. */
    fn brand_mark(&self) -> Icon {
        Icon::new(IconName::Star).with_size(px(22.0)).text_color(TEAL)
    }

    fn section_label(&self, text: &'static str) -> gpui::Div {
        div()
            .text_size(px(11.0))
            .text_color(INK_FAINT)
            .child(text)
    }

    /* A label/value line inside the inspector. */
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
                    .text_color(INK_MID)
                    .child(label),
            )
            .child(
                div()
                    .font_family("Menlo")
                    .min_w_0()
                    .truncate()
                    .text_size(px(10.5))
                    .text_color(INK)
                    .child(SharedString::from(value)),
            )
    }

    /* A path row with Copy / Show actions. */
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
            .px_3()
            .py_2()
            .rounded_md()
            .bg(CREAM)
            .child(
                div()
                    .min_w_0()
                    .flex_1()
                    .truncate()
                    .font_family("Menlo")
                    .text_size(px(10.0))
                    .text_color(if has_path { INK_MID } else { INK_FAINT })
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
                        .text_size(px(10.5))
                        .text_color(TEAL)
                        .hover(|s| s.opacity(0.7))
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
                        .text_size(px(10.5))
                        .text_color(TEAL)
                        .hover(|s| s.opacity(0.7))
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

    /* One agent on the icon rail: initial letter on its colour, name below. */
    fn agent_tile(
        &self,
        id: String,
        initial: String,
        color: Rgba,
        active: bool,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::AnyElement {
        div()
            .id(SharedString::from(id))
            .w(px(44.0))
            .h(px(44.0))
            .flex()
            .items_center()
            .justify_center()
            .rounded(px(14.0))
            .border_2()
            .border_color(if active { TEAL } else { TRANSPARENT })
            .bg(if active { color } else { tint(color) })
            .text_color(if active {
                rgba_hex(0xffffff, 1.0)
            } else {
                color
            })
            .text_size(px(14.0))
            .font_weight(FontWeight::MEDIUM)
            .when(!active, |t| {
                t.hover(|s| s.bg(tint(color)).border_color(rgba_hex(0x0f8a7e, 0.25)))
            })
            .on_click(cx.listener(move |state, _e, _w, cx| on_click(state, cx)))
            .child(SharedString::from(initial))
            .into_any_element()
    }

    /* Tile + always-visible name, so the rail identifies agents at a glance. */
    fn agent_cell(
        &self,
        id: String,
        name: String,
        initial: String,
        color: Rgba,
        active: bool,
        cx: &Context<Self>,
        on_click: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::Div {
        div()
            .flex()
            .flex_col()
            .items_center()
            .gap(px(3.0))
            .child(self.agent_tile(id, initial, color, active, cx, on_click))
            .child(
                div()
                    .w(px(64.0))
                    .truncate()
                    .text_center()
                    .text_size(px(9.0))
                    .text_color(if active { INK } else { INK_FAINT })
                    .child(SharedString::from(name)),
            )
    }
}

impl AppState {
    /* Everything the palette offers: the five actions, then every skill. */
    fn command_items(&self) -> Vec<CommandItem> {
        let mut items = vec![
            CommandItem::new()
                .label("扫描本机技能")
                .icon(IconName::HardDrive)
                .keywords(vec!["scan", "扫描"]),
            CommandItem::new()
                .label("同步所有 agent 视图")
                .icon(IconName::RefreshCw)
                .keywords(vec!["sync", "同步"]),
            CommandItem::new()
                .label("链接（预览）")
                .icon(IconName::Network)
                .keywords(vec!["link", "链接"]),
            CommandItem::new()
                .label("清理仓库（预览）")
                .icon(IconName::Delete)
                .keywords(vec!["gc", "清理"]),
            CommandItem::new()
                .label("还原所有 agent 的原始目录")
                .icon(IconName::Undo)
                .keywords(vec!["unlink", "还原"]),
        ];
        for skill in &self.skills {
            items.push(
                CommandItem::new()
                    .label(skill.name.clone())
                    .icon(if skill.stored {
                        IconName::CircleCheck
                    } else {
                        IconName::Info
                    })
                    .keywords(vec![skill.description.clone()]),
            );
        }
        items
    }

    /* The palette: a filterable list over actions + skills. */
    fn command_palette(&self, state: &Entity<CommandState>, cx: &Context<Self>) -> CommandPalette {
        const ACTION_COUNT: usize = 5;
        let view = cx.entity();
        CommandPalette::new(state)
            .items(self.command_items())
            .searchable(true)
            .filterable(true)
            .placeholder("搜索技能或操作…")
            .on_confirm(move |path, _window, cx| {
                view.update(cx, |state, cx| {
                    state.command_open = false;
                    state.command_state = None;
                    if path.row < ACTION_COUNT {
                        match path.row {
                            0 => state.run("scan", vec!["scan", "--json"], true, cx),
                            1 => state.run("sync", vec!["sync", "--json"], true, cx),
                            2 => {
                                let scope = if state.scope == Scope::Own {
                                    "own"
                                } else {
                                    "all"
                                };
                                state.run(
                                    "link",
                                    vec!["link", "--dry-run", "--json", "--scope", scope],
                                    true,
                                    cx,
                                );
                            }
                            3 => state.run("gc", vec!["gc", "--dry-run", "--json"], false, cx),
                            _ => state.run("unlink", vec!["unlink", "--json"], true, cx),
                        }
                    } else if let Some(entry) = state.skills.get(path.row - ACTION_COUNT) {
                        /* Jump to the skill inside the currently visible list. */
                        let target = entry.id.clone();
                        let visible = state.visible_indices();
                        let pos = visible
                            .into_iter()
                            .position(|vi| state.skills[vi].id == target);
                        if let Some(pos) = pos {
                            state.select(Some(pos), cx);
                            state.show_detail = true;
                        }
                    }
                    cx.notify();
                });
            })
            .on_cancel(|_window, _cx| {})
    }
}

impl gpui::Render for AppState {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl gpui::IntoElement {
        /* ⌘K opens the full-screen command palette. */
        if self.command_open {
            if let Some(state) = &self.command_state {
                return div()
                    .flex()
                    .justify_center()
                    .pt(px(72.0))
                    .child(self.command_palette(state, cx));
            }
        }

        /* The kit Input owns the text; the filter term follows it. */
        if let Some(state) = &self.search_state {
            self.query = state.read(cx).value().to_string();
        }

        let filtered = Rc::new(self.visible_indices());
        let effective_selected = self
            .selected
            .filter(|i| *i < filtered.len())
            .filter(|_| self.show_detail);

        /* ============================================== title bar */
        let titlebar = TitleBar::new().child(
            div()
                .flex()
                .items_center()
                .gap_3()
                .h(px(46.0))
                .pl(if cfg!(target_os = "macos") { px(80.0) } else { px(16.0) })
                .pr(px(14.0))
                .child(self.brand_mark())
                .child(
                    div()
                        .flex()
                        .flex_col()
                        .child(
                            div()
                                .text_size(px(15.0))
                                .font_weight(FontWeight::SEMIBOLD)
                                .text_color(INK)
                                .child("StarTide"),
                        )
                        .child(
                            div()
                                .font_family("Menlo")
                                .text_size(px(9.0))
                                .text_color(INK_FAINT)
                                .child(SharedString::from(format!(
                                    "{} skills · {} agents · {} shared",
                                    self.skills.len(),
                                    self.agents.len(),
                                    format_bytes(self.store_bytes)
                                ))),
                        ),
                )
                .child(div().flex_1())
                .child(
                    div()
                        .max_w(px(240.0))
                        .truncate()
                        .text_size(px(10.5))
                        .text_color(match self.msg_kind {
                            MessageKind::Idle => INK_MID,
                            MessageKind::Busy => INK_FAINT,
                            MessageKind::Ok => GREEN,
                            MessageKind::Err => RED,
                        })
                        .child(self.msg.clone()),
                )
                .child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .child(self.action_button(
                            "scan",
                            "扫描",
                            IconName::HardDrive,
                            |b| b.secondary(),
                            cx,
                            |state, cx| state.run("scan", vec!["scan", "--json"], true, cx),
                        ))
                        .child(self.action_button(
                            "sync",
                            "同步",
                            IconName::RefreshCw,
                            |b| b.secondary(),
                            cx,
                            |state, cx| state.run("sync", vec!["sync", "--json"], true, cx),
                        ))
                        .child(self.action_button(
                            "link",
                            "链接",
                            IconName::Network,
                            |b| b.custom(ButtonCustomVariant::new(cx)
                                .color(Hsla::from(TEAL))
                                .foreground(Hsla::from(rgba_hex(0xffffff, 1.0)))),
                            cx,
                            |state, cx| {
                                let scope = if state.scope == Scope::Own { "own" } else { "all" };
                                state.run(
                                    "link",
                                    vec!["link", "--dry-run", "--json", "--scope", scope],
                                    true,
                                    cx,
                                );
                            },
                        ))
                        .child(self.action_button(
                            "gc",
                            "清理",
                            IconName::Delete,
                            |b| b.secondary(),
                            cx,
                            |state, cx| state.run("gc", vec!["gc", "--dry-run", "--json"], false, cx),
                        ))
                        .child(self.action_button(
                            "unlink",
                            "还原",
                            IconName::Undo,
                            |b| b.secondary(),
                            cx,
                            |state, cx| {
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
                            },
                        )),
                ),
        );

        /* ================================================= search row */
        let stored_count = self.skills.iter().filter(|e| e.stored).count();
        let known_count = self.skills.len().saturating_sub(stored_count);

        let search_row = div()
            .flex()
            .items_center()
            .gap_4()
            .px(px(20.0))
            .h(px(54.0))
            .border_b_1()
            .border_color(HAIRLINE)
            .child(
                div()
                    .id("search")
                    .flex_1()
                    .h(px(42.0))
                    .px_4()
                    .flex()
                    .items_center()
                    .gap_2()
                    .rounded(px(12.0))
                    .bg(CARD)
                    .border_1()
                    .border_color(HAIRLINE)
                    .shadow_sm()
                    .child(
                        Icon::new(IconName::Search).with_size(px(16.0)).text_color(INK_FAINT),
                    )
                    .children(self.search_state.iter().map(|state| {
                        Input::new(state).appearance(false).h_full().flex_1()
                    })),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_1p5()
                    .child(
                        div()
                            .text_size(px(11.0))
                            .text_color(INK_FAINT)
                            .child("范围"),
                    )
                    .child(self.chip(
                        "scope-own".into(),
                        "own".into(),
                        None,
                        self.scope == Scope::Own,
                        cx,
                        |state, cx| {
                            state.scope = Scope::Own;
                            cx.notify();
                        },
                    ))
                    .child(self.chip(
                        "scope-all".into(),
                        "all".into(),
                        None,
                        self.scope == Scope::All,
                        cx,
                        |state, cx| {
                            state.scope = Scope::All;
                            cx.notify();
                        },
                    )),
            );

        /* =================================================== icon rail */
        let fold_at = 10usize;
        let shown = if self.agents_expanded {
            self.agents.len()
        } else {
            self.agents.len().min(fold_at)
        };
        let mut rail = div()
            .id("agent-rail")
            .w(px(68.0))
            .flex_none()
            .flex()
            .flex_col()
            .items_center()
            .gap(px(8.0))
            .py(px(12.0))
            .border_r_1()
            .border_color(HAIRLINE)
            .bg(CARD)
            .overflow_y_scroll();
        rail = rail.child(self.agent_cell(
            "rail-all".into(),
            "全部".into(),
            "全".into(),
            TEAL,
            self.agent_filter.is_none(),
            cx,
            |state, cx| {
                state.agent_filter = None;
                state.selected = None;
                cx.notify();
            },
        ));
        for agent in self.agents.iter().take(shown) {
            let label = if agent.label.is_empty() {
                agent.id.clone()
            } else {
                agent.label.clone()
            };
            let initial = label.chars().next().unwrap_or('•').to_string();
            let color = self.agent_color(&agent.id);
            let is_active = self.agent_filter.as_deref() == Some(agent.id.as_str());
            rail = rail.child(self.agent_cell(
                format!("rail-{}", agent.id),
                label.clone(),
                initial,
                color,
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
        if self.agents.len() > fold_at {
            let more = self.agents.len() - shown;
            rail = rail.child(self.agent_tile(
                "rail-more".into(),
                format!("+{}", more),
                INK_MID,
                false,
                cx,
                |state, cx| {
                    state.agents_expanded = !state.agents_expanded;
                    cx.notify();
                },
            ));
        }

        /* ================================================= filter row */
        let active_label = match &self.agent_filter {
            Some(id) => self.agent_label(id),
            None => "全部 agent".to_string(),
        };
        let filter_row = div()
            .flex()
            .items_center()
            .gap_3()
            .px(px(20.0))
            .h(px(44.0))
            .child(
                div()
                    .text_size(px(13.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(INK)
                    .child("技能"),
            )
            .child(
                div()
                    .text_size(px(10.5))
                    .text_color(INK_FAINT)
                    .child(SharedString::from(format!(
                        "{}/{}",
                        filtered.len(),
                        self.skills.len()
                    ))),
            )
            .child(div().w(px(1.0))).h(px(16.0)).bg(HAIRLINE)
            .child(
                div()
                    .text_size(px(11.0))
                    .text_color(INK_MID)
                    .child(SharedString::from(format!(
                        "当前：{} · {} 个技能",
                        active_label,
                        filtered.len()
                    ))),
            )
            .child(div().flex_1())
            .child(self.chip(
                "tab-all".into(),
                "全部".into(),
                Some(self.skills.len().to_string()),
                self.status_tab == StatusTab::All,
                cx,
                |state, cx| {
                    state.status_tab = StatusTab::All;
                    state.selected = None;
                    cx.notify();
                },
            ))
            .child(self.chip(
                "tab-stored".into(),
                "已入库".into(),
                Some(stored_count.to_string()),
                self.status_tab == StatusTab::Stored,
                cx,
                |state, cx| {
                    state.status_tab = StatusTab::Stored;
                    state.selected = None;
                    cx.notify();
                },
            ))
            .child(self.chip(
                "tab-known".into(),
                "仅目录".into(),
                Some(known_count.to_string()),
                self.status_tab == StatusTab::Known,
                cx,
                |state, cx| {
                    state.status_tab = StatusTab::Known;
                    state.selected = None;
                    cx.notify();
                },
            ));

        /* ========================================================= list */
        let list: gpui::AnyElement = if filtered.is_empty() {
            div()
                .flex_1()
                .flex()
                .flex_col()
                .items_center()
                .justify_center()
                .gap_3()
                .child(Icon::new(IconName::Inbox).with_size(px(28.0)).text_color(INK_FAINT))
                .child(
                    div()
                        .text_size(px(15.0))
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(INK_MID)
                        .child(if self.skills.is_empty() {
                            "目录还是空的"
                        } else {
                            "没有匹配的技能"
                        }),
                )
                .child(
                    div()
                        .text_size(px(11.5))
                        .text_color(INK_FAINT)
                        .child(if self.skills.is_empty() {
                            "点「扫描」索引这台机器上的所有技能"
                        } else {
                            "换个搜索词或筛选条件试试"
                        }),
                )
                .when(self.skills.is_empty(), |c| {
                    c.child(self.action_button(
                        "empty-scan",
                        "扫描本机",
                        IconName::HardDrive,
                        |b| b.custom(ButtonCustomVariant::new(cx)
                            .color(Hsla::from(TEAL))
                            .foreground(Hsla::from(rgba_hex(0xffffff, 1.0)))),
                        cx,
                        |state, cx| state.run("scan", vec!["scan", "--json"], true, cx),
                    ))
                })
                .into_any_element()
        } else {
            uniform_list(
                "skills",
                filtered.len(),
                {
                    let view = cx.entity();
                    let filtered = filtered.clone();
                    move |range, window, cx| {
                        let filtered = filtered.clone();
                        view.update(cx, |state, cx| {
                            state.render_rows(range, filtered, window, cx)
                        })
                    }
                },
            )
            .track_scroll(&self.list_scroll)
            .flex_1()
            .into_any_element()
        };

        let centre = div()
            .flex()
            .flex_col()
            .flex_1()
            .min_w_0()
            .bg(CARD)
            .border_1()
            .border_color(HAIRLINE)
            .shadow_sm()
            .rounded(px(16.0))
            .overflow_hidden()
            .child(filter_row)
            .child(list);

        /* ================================================== inspector */
        let mut inspector_inner = div()
            .flex()
            .items_center()
            .justify_between()
            .child(
                div()
                    .text_size(px(13.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(INK)
                    .child("详情"),
            )
            .child(
                div()
                    .id("collapse-detail")
                    .w(px(26.0))
                    .h(px(26.0))
                    .flex()
                    .items_center()
                    .justify_center()
                    .rounded_md()
                    .text_color(INK_MID)
                    .hover(|s| s.bg(HAIRLINE).text_color(INK))
                    .on_click(cx.listener(|state, _e, _w, cx| {
                        state.show_detail = false;
                        cx.notify();
                    }))
                    .child(Icon::new(IconName::ChevronRight).with_size(px(14.0))),
            );

        let mut inspector: gpui::AnyElement = div()
            .w(px(0.0))
            .flex_none()
            .into_any_element();

        if let (true, Some(ix)) = (self.show_detail, effective_selected) {
            if let Some(entry) = filtered.get(ix).and_then(|i| self.skills.get(*i)) {
                let unit_path = if entry.stored && !self.store_home.is_empty() {
                    format!("{}/units/{}", self.store_home, entry.id)
                } else {
                    String::new()
                };

                inspector_inner = inspector_inner
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap_2()
                            .child(
                                Badge::new()
                                    .dot()
                                    .color(if entry.stored {
                                        Hsla::from(TEAL)
                                    } else {
                                        Hsla::from(AMBER)
                                    })
                                    .child(if entry.stored { "已入库" } else { "仅目录" }),
                            )
                            .child(
                                div()
                                    .font_family("Georgia")
                                    .text_size(px(22.0))
                                    .line_height(px(28.0))
                                    .text_color(INK)
                                    .child(SharedString::from(entry.name.clone())),
                            )
                            .child(
                                div()
                                    .text_size(px(12.5))
                                    .line_height(px(20.0))
                                    .text_color(INK_MID)
                                    .child(SharedString::from(if entry.description.is_empty() {
                                        "暂无描述".to_string()
                                    } else {
                                        entry.description.clone()
                                    })),
                            ),
                    )
                    .child(div().h(px(1.0)).w_full().bg(HAIRLINE))
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
                    .child({
                        let mut pills = div().flex().flex_wrap().gap_1p5();
                        if entry.agents.is_empty() {
                            pills = pills.child(
                                div()
                                    .text_size(px(11.0))
                                    .text_color(INK_FAINT)
                                    .child("仅范围 all 时可见"),
                            );
                        } else {
                            for id in &entry.agents {
                                let color = self.agent_color(id);
                                pills = pills.child(
                                    div()
                                        .flex()
                                        .items_center()
                                        .gap_1p5()
                                        .px_2()
                                        .py(px(3.0))
                                        .rounded_full()
                                        .bg(CREAM)
                                        .child(div().size(px(6.0)).rounded_full().bg(color))
                                        .child(
                                            div()
                                                .text_size(px(11.0))
                                                .text_color(INK)
                                                .child(SharedString::from(self.agent_label(id))),
                                        ),
                                );
                            }
                        }
                        div()
                            .flex()
                            .flex_col()
                            .gap_2()
                            .child(self.section_label("哪些 agent 能看到"))
                            .child(pills)
                    })
                    .child({
                        let mut sources_block = div()
                            .flex()
                            .flex_col()
                            .gap_1p5()
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
                                    .text_size(px(10.0))
                                    .text_color(INK_FAINT)
                                    .child(SharedString::from(format!(
                                        "还有 {} 个位置",
                                        entry.sources.len() - 4
                                    ))),
                            );
                        }
                        sources_block
                    })
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap_1p5()
                            .child(self.section_label("仓库位置"))
                            .child(self.path_row(
                                "store".into(),
                                &unit_path,
                                "尚未复制进仓库",
                                true,
                                cx,
                            )),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap_2()
                            .pt_1()
                            .child(
                                div()
                                    .text_size(px(10.5))
                                    .text_color(INK_FAINT)
                                    .child(SharedString::from(format_bytes(entry.size))),
                            )
                            .child(div().w(px(3.0)).rounded_full().bg(INK_FAINT))
                            .child(
                                div()
                                    .text_size(px(10.5))
                                    .text_color(INK_FAINT)
                                    .child(SharedString::from(format!(
                                        "磁盘上 {} 份副本",
                                        entry.sources.len()
                                    ))),
                            ),
                    );

                inspector = div()
                    .id("inspector")
                    .flex()
                    .flex_col()
                    .gap_4()
                    .w(px(DETAIL_WIDTH))
                    .flex_none()
                    .h_full()
                    .overflow_y_scroll()
                    .rounded(px(16.0))
                    .bg(CARD)
                    .border_1()
                    .border_color(HAIRLINE)
                    .shadow_md()
                    .px(px(22.0))
                    .pt(px(16.0))
                    .pb(px(24.0))
                    .child(inspector_inner)
                    .into_any_element();
            }
        }

        /* ================================================== status bar */
        let status_bar = StatusBar::new()
            .left(
                div()
                    .flex()
                    .items_center()
                    .gap_1p5()
                    .child(
                        div()
                            .size(px(6.0))
                            .rounded_full()
                            .bg(if self.cli_ok { GREEN } else { RED }),
                    )
                    .child(
                        div()
                            .text_size(px(10.5))
                            .text_color(INK_MID)
                            .child(SharedString::from(format!(
                                "引擎 skillctl · {} agents · {} units",
                                self.agents.len(),
                                self.store_units
                            ))),
                    ),
            )
            .right(
                div()
                    .font_family("Menlo")
                    .text_size(px(10.0))
                    .text_color(INK_FAINT)
                    .child(SharedString::from(format!(
                        "{} 范围 · {}K 搜索",
                        if self.scope == Scope::Own { "own" } else { "all" },
                        modifier_label()
                    ))),
            );

        /* ======================================================= root */
        div()
            .track_focus(&self.root_focus)
            .flex()
            .flex_col()
            .size_full()
            .bg(CREAM)
            .text_color(INK)
            .font_family(ui_font())
            .on_key_down(cx.listener(Self::on_root_key))
            .child(titlebar)
            .child(search_row)
            .child(
                div()
                    .flex()
                    .flex_1()
                    .min_h_0()
                    .gap(px(12.0))
                    .px(px(16.0))
                    .pt(px(12.0))
                    .pb(px(12.0))
                    .child(rail)
                    .child(centre)
                    .child(inspector),
            )
            .child(status_bar)
    }
}

impl AppState {
    fn render_rows(
        &mut self,
        range: std::ops::Range<usize>,
        filtered: Rc<Vec<usize>>,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Vec<gpui::AnyElement> {
        let selected = self.selected.filter(|i| *i < filtered.len());
        range
            .map(|ix| {
                let skill_ix = filtered[ix];
                match self.skills.get(skill_ix) {
                    Some(entry) => self.row(ix, entry, selected == Some(ix), cx),
                    None => div().h(px(ROW_HEIGHT)).into_any_element(),
                }
            })
            .collect()
    }

    /* One entry of the index: name, purpose, attribution, status. */
    fn row(
        &self,
        ix: usize,
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
                    .size(px(7.0))
                    .rounded_full()
                    .bg(if lit {
                        self.agent_color(&agent.id)
                    } else {
                        rgba_hex(0xe6e0d4, 1.0)
                    }),
            );
        }
        if self.agents.len() > max_dots {
            dots = dots.child(
                div()
                    .text_size(px(9.0))
                    .text_color(INK_FAINT)
                    .child(SharedString::from(format!("+{}", self.agents.len() - max_dots))),
            );
        }

        div()
            .id(ix)
            .h(px(ROW_HEIGHT))
            .w_full()
            .flex()
            .items_center()
            .border_b_1()
            .border_color(HAIRLINE)
            .bg(if selected { TEAL_SOFT } else { CARD })
            .when(!selected, |r| r.hover(|s| s.bg(CARD_HOVER)))
            .on_click(cx.listener(move |state, _e, window, cx| {
                state.root_focus.focus(window, cx);
                state.select(Some(ix), cx);
                state.show_detail = true;
                cx.notify();
            }))
            .child(
                div()
                    .w(px(3.0))
                    .h_full()
                    .flex_none()
                    .rounded_r_xs()
                    .bg(if selected { TEAL } else { TRANSPARENT }),
            )
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .h_full()
                    .flex()
                    .items_center()
                    .gap_4()
                    .pl(px(14.0))
                    .pr_4()
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(3.0))
                            .child(
                                div()
                                    .truncate()
                                    .text_size(px(14.5))
                                    .font_weight(FontWeight::MEDIUM)
                                    .text_color(if selected { TEAL } else { INK })
                                    .child(SharedString::from(entry.name.clone())),
                            )
                            .child(
                                div()
                                    .truncate()
                                    .text_size(px(11.5))
                                    .text_color(INK_MID)
                                    .child(SharedString::from(entry.description.clone())),
                            ),
                    )
                    .child(dots)
                    .child(
                        div()
                            .w(px(56.0))
                            .flex_none()
                            .font_family("Menlo")
                            .text_size(px(9.5))
                            .text_color(INK_FAINT)
                            .child(SharedString::from(format_bytes(entry.size))),
                    )
                    .child(
                        Badge::new()
                            .dot()
                            .color(if entry.stored {
                                Hsla::from(TEAL)
                            } else {
                                Hsla::from(AMBER)
                            })
                            .child(if entry.stored { "已入库" } else { "仅目录" }),
                    ),
            )
            .into_any_element()
    }
}

fn main() {
    gpui_kit::application()
        .with_assets(Assets)
        .run(|cx| {
            init(cx);
            let bounds = Bounds::centered(None, size(px(1200.0), px(780.0)), cx);
            open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Windowed(bounds)),
                    ..TitleBar::window_options()
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
                        state.root_focus.focus(window, cx);
                        state
                    })
                },
            )
            .expect("open window");
        });
}
