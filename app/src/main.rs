/*
 * StarTide — Tauri shell.
 *
 * The C engine (skillctl) is the only source of truth: every command the
 * window runs goes through it and its JSON output feeds the UI.
 */
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::path::PathBuf;
use std::process::Command;

/// Locate the skillctl binary: $SKILLCTL_BIN, next to this executable's
/// project tree, or on the PATH.
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
                    let cand = d.join("build").join("skillctl");
                    if cand.exists() {
                        return Some(cand);
                    }
                    let win = d.join("build-win").join("skillctl.exe");
                    if win.exists() {
                        return Some(win);
                    }
                    dir = d.parent().map(|p| p.to_path_buf());
                }
                None => break,
            }
        }
    }
    which_on_path("skillctl")
}

fn which_on_path(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path) {
        let cand = dir.join(name);
        if cand.is_file() {
            return Some(cand);
        }
        let exe = dir.join(format!("{}.exe", name));
        if exe.is_file() {
            return Some(exe);
        }
    }
    None
}

/// Run one skillctl subcommand and return its stdout as a string.
#[tauri::command]
fn cli(cmd: String, args: Vec<String>) -> Result<String, String> {
    let bin = find_cli().ok_or_else(|| {
        "找不到 skillctl 引擎：运行 make 构建，或用 SKILLCTL_BIN 指定路径".to_string()
    })?;
    let out = Command::new(bin)
        .arg(&cmd)
        .args(&args)
        .output()
        .map_err(|e| format!("无法运行 skillctl {}: {}", cmd, e))?;
    if out.status.success() {
        Ok(String::from_utf8_lossy(&out.stdout).into_owned())
    } else {
        let err = String::from_utf8_lossy(&out.stderr);
        Err(if err.is_empty() {
            format!("skillctl {} 退出码 {}", cmd, out.status)
        } else {
            err.lines().next().unwrap_or("失败").to_string()
        })
    }
}

/// Reveal a path in the platform file manager.
#[tauri::command]
fn reveal(path: String) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    let status = std::process::Command::new("open").arg("-R").arg(&path).status();
    #[cfg(target_os = "windows")]
    let status = std::process::Command::new("explorer")
        .arg(format!("/select,\"{}\"", path))
        .status();
    #[cfg(all(unix, not(target_os = "macos")))]
    let status = {
        let parent = std::path::Path::new(&path)
            .parent()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_else(|| path.clone());
        std::process::Command::new("xdg-open").arg(parent).status()
    };
    status.map(|_| ()).map_err(|e| format!("无法打开文件管理器: {}", e))
}

/// Where the engine lives, for the status display.
#[tauri::command]
fn engine_path() -> String {
    find_cli()
        .map(|p| p.display().to_string())
        .unwrap_or_else(|| "未找到".to_string())
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![cli, engine_path, reveal])
        .run(tauri::generate_context!())
        .expect("error while running StarTide");
}
