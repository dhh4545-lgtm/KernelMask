use anyhow::{Context, Result};
use const_format::concatcp;
use log::{info, warn};
use rustix::cstr;
use std::fs;
use std::path::Path;

use crate::assets;
use crate::defs::{WORKING_DIR};
use crate::utils::is_safe_mode;

const KERNEL_LIB_DIR: &str = concatcp!(WORKING_DIR, "kernel/lib/");
const PATHHIDE_KO: &str = concatcp!(KERNEL_LIB_DIR, "KernelMask.ko");
const PATHHIDE_CONFIG: &str = concatcp!(WORKING_DIR, "pathhide.txt");
const PATHHIDE_DISABLE: &str = concatcp!(WORKING_DIR, "pathhide.disable");

fn ensure_dirs() -> Result<()> {
    fs::create_dir_all(KERNEL_LIB_DIR).context("create kernel/lib dir")?;
    Ok(())
}

pub fn is_disabled() -> bool {
    if is_safe_mode() {
        info!("pathhide: safe mode detected, module disabled");
        return true;
    }
    if Path::new(PATHHIDE_DISABLE).exists() {
        info!("pathhide: disable flag exists, module disabled");
        return true;
    }
    false
}

pub fn disable() -> Result<()> {
    ensure_dirs()?;
    fs::write(PATHHIDE_DISABLE, "")?;
    info!("pathhide disabled (flag file created)");
    if is_loaded() {
        if let Err(e) = unload() {
            warn!("pathhide unload after disable failed: {e:#}");
        }
    }
    Ok(())
}

pub fn enable() -> Result<()> {
    if Path::new(PATHHIDE_DISABLE).exists() {
        fs::remove_file(PATHHIDE_DISABLE)?;
        info!("pathhide enabled (flag file removed)");
    }
    if !is_loaded() && !is_safe_mode() {
        if let Err(e) = load() {
            warn!("pathhide load after enable failed: {e:#}");
        }
    }
    Ok(())
}

pub fn deploy_ko() -> Result<()> {
    ensure_dirs()?;
    let kmi = crate::boot_patch::get_current_kmi()
        .context("failed to detect current KMI for pathhide")?;
    let asset_name = format!("pathhide_{kmi}.ko");
    info!("deploying pathhide ko for KMI: {kmi} (asset: {asset_name})");
    let data = assets::get_asset_data(&asset_name)
        .with_context(|| format!("no embedded pathhide module for KMI {kmi}: {asset_name}"))?;
    fs::write(PATHHIDE_KO, &data).context("write KernelMask.ko")?;
    fs::set_permissions(PATHHIDE_KO, std::os::unix::fs::PermissionsExt::from_mode(0o644))?;
    info!("KernelMask.ko deployed to {PATHHIDE_KO}");
    Ok(())
}

pub fn is_loaded() -> bool {
    fs::read_to_string("/proc/modules")
        .map(|s| s.lines().any(|l| l.starts_with("pathhide ")))
        .unwrap_or(false)
}

pub fn get_config() -> Result<String> {
    ensure_dirs()?;
    if !Path::new(PATHHIDE_CONFIG).exists() {
        fs::write(PATHHIDE_CONFIG, "# One absolute path per line\n")?;
    }
    let content = fs::read_to_string(PATHHIDE_CONFIG).unwrap_or_default();
    Ok(content)
}

pub fn set_config(content: &str) -> Result<()> {
    ensure_dirs()?;
    fs::write(PATHHIDE_CONFIG, content).context("write config")?;
    info!("pathhide config updated ({} bytes)", content.len());
    Ok(())
}

pub fn load() -> Result<()> {
    if is_loaded() {
        info!("pathhide already loaded");
        return Ok(());
    }
    if is_disabled() {
        info!("pathhide is disabled, skip loading");
        return Ok(());
    }
    ensure_dirs()?;

    // Ensure config file exists
    if !Path::new(PATHHIDE_CONFIG).exists() {
        fs::write(PATHHIDE_CONFIG, "# One absolute path per line\n")?;
    }

    // Always deploy ko first, even if config is empty
    if !Path::new(PATHHIDE_KO).exists() {
        deploy_ko()?;
    }

    // Check if config has any valid (non-comment, non-empty) lines
    let config_content = fs::read_to_string(PATHHIDE_CONFIG).unwrap_or_default();
    let active_count = config_content
        .lines()
        .map(|l| l.trim())
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .count();
    if active_count == 0 {
        info!("pathhide: config is empty, skip loading");
        return Ok(());
    }

    let ko_data = fs::read(PATHHIDE_KO).context("read KernelMask.ko")?;
    let params = format!("config_path={PATHHIDE_CONFIG}");
    let cparams = std::ffi::CString::new(params)?;

    ksuinit::load_module(&ko_data, &cparams).context("load ksu.ko")?;
    info!("ksu.ko loaded with config={PATHHIDE_CONFIG}");
    Ok(())
}

pub fn unload() -> Result<()> {
    if !is_loaded() {
        info!("pathhide not loaded");
        return Ok(());
    }
    rustix::system::delete_module(cstr!("pathhide"), 0).context("rmmod pathhide")?;
    info!("pathhide unloaded");
    Ok(())
}

pub fn reload() -> Result<()> {
    if is_loaded() {
        let _ = unload();
    }
    std::thread::sleep(std::time::Duration::from_millis(500));
    load()
}

pub fn status() {
    let loaded = is_loaded();
    println!("loaded: {loaded}");
    if loaded {
        if let Ok(content) = get_config() {
            let paths: Vec<&str> = content
                .lines()
                .map(|l| l.trim())
                .filter(|l| !l.is_empty() && !l.starts_with('#'))
                .collect();
            println!("hidden paths ({}):", paths.len());
            for p in paths {
                println!("  {p}");
            }
        }
    }
}
