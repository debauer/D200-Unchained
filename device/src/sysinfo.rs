//! Device facts for the SYSINFO reply: CPU, memory, storage, uptime, load,
//! temperature and serial number, as `key=value` lines. Facts that cannot be
//! read on the running system are omitted.

use std::ffi::CString;
use std::fs;

pub fn collect() -> String {
    let mut out = String::new();

    let cpuinfo = read("/proc/cpuinfo");
    let (cores, model) = parse_cpuinfo(&cpuinfo);
    if let Some(cores) = cores {
        push(&mut out, "cpu_cores", &cores.to_string());
    }
    if let Some(model) = model {
        push(&mut out, "cpu_model", &model);
    }
    push(&mut out, "arch", std::env::consts::ARCH);
    push(&mut out, "kernel", read("/proc/sys/kernel/osrelease").trim());

    let meminfo = read("/proc/meminfo");
    for (key, field) in [
        ("mem_total_kb", "MemTotal"),
        ("mem_free_kb", "MemFree"),
        ("mem_available_kb", "MemAvailable"),
        ("swap_total_kb", "SwapTotal"),
        ("swap_free_kb", "SwapFree"),
    ] {
        if let Some(kb) = parse_meminfo(&meminfo, field) {
            push(&mut out, key, &kb.to_string());
        }
    }

    for (key, path) in [("userdata", "/userdata"), ("root", "/")] {
        if let Some((total, free)) = statvfs(path) {
            push(&mut out, &format!("{key}_total_bytes"), &total.to_string());
            push(&mut out, &format!("{key}_free_bytes"), &free.to_string());
        }
    }

    if let Some(seconds) = parse_uptime(&read("/proc/uptime")) {
        push(&mut out, "uptime_s", &seconds.to_string());
    }
    if let Some(load) = parse_loadavg(&read("/proc/loadavg")) {
        push(&mut out, "loadavg", &load);
    }
    if let Ok(mc) = read("/sys/class/thermal/thermal_zone0/temp").trim().parse::<u64>() {
        push(&mut out, "temp_mc", &mc.to_string());
    }
    if let Some(serial) = parse_serial(&read("/proc/cmdline")) {
        push(&mut out, "serial", &serial);
    }
    out
}

fn read(path: &str) -> String {
    fs::read_to_string(path).unwrap_or_default()
}

fn push(out: &mut String, key: &str, value: &str) {
    if !value.is_empty() {
        out.push_str(key);
        out.push('=');
        out.push_str(value);
        out.push('\n');
    }
}

/// Number of `processor` entries and the first `model name` (or `Hardware`) line.
fn parse_cpuinfo(text: &str) -> (Option<u32>, Option<String>) {
    let (mut cores, mut model, mut hardware) = (0u32, None, None);
    for line in text.lines() {
        if line.starts_with("processor") {
            cores += 1;
        } else if let Some(rest) = line.strip_prefix("model name") {
            model.get_or_insert_with(|| value_after_colon(rest));
        } else if let Some(rest) = line.strip_prefix("Hardware") {
            hardware.get_or_insert_with(|| value_after_colon(rest));
        }
    }
    let cores = (cores > 0).then_some(cores);
    (cores, model.or(hardware))
}

fn value_after_colon(rest: &str) -> String {
    rest.split_once(':').map_or("", |(_, v)| v.trim()).to_string()
}

/// Value of a `Field: value kB` line in `/proc/meminfo`, in kB.
fn parse_meminfo(text: &str, field: &str) -> Option<u64> {
    for line in text.lines() {
        let Some((key, rest)) = line.split_once(':') else { continue };
        if key == field {
            return rest.split_whitespace().next()?.parse().ok();
        }
    }
    None
}

/// First field of `/proc/uptime`, in whole seconds.
fn parse_uptime(text: &str) -> Option<u64> {
    Some(text.split_whitespace().next()?.parse::<f64>().ok()? as u64)
}

/// First three fields of `/proc/loadavg`.
fn parse_loadavg(text: &str) -> Option<String> {
    let fields: Vec<&str> = text.split_whitespace().take(3).collect();
    (fields.len() == 3).then(|| fields.join(" "))
}

/// `androidboot.serialno=...` token of `/proc/cmdline`.
fn parse_serial(cmdline: &str) -> Option<String> {
    cmdline.split_whitespace().find_map(|tok| tok.strip_prefix("androidboot.serialno=")).map(str::to_string)
}

/// Total and available bytes of the filesystem mounted at `path` (statvfs).
fn statvfs(path: &str) -> Option<(u64, u64)> {
    let path = CString::new(path).ok()?;
    let mut st = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    if unsafe { libc::statvfs(path.as_ptr(), st.as_mut_ptr()) } != 0 {
        return None;
    }
    let st = unsafe { st.assume_init() };
    let block = if st.f_frsize > 0 { st.f_frsize } else { st.f_bsize } as u64;
    Some((st.f_blocks as u64 * block, st.f_bavail as u64 * block))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_cpuinfo_and_prefers_model_name() {
        let arm = "processor\t: 0\nmodel name\t: ARMv7 Processor rev 5 (v7l)\nHardware\t: Rockchip (Device Tree)\n\
                   processor\t: 1\nmodel name\t: ARMv7 Processor rev 5 (v7l)\n";
        assert_eq!(parse_cpuinfo(arm), (Some(2), Some("ARMv7 Processor rev 5 (v7l)".into())));
        let hardware_only = "processor\t: 0\nHardware\t: Rockchip\n";
        assert_eq!(parse_cpuinfo(hardware_only), (Some(1), Some("Rockchip".into())));
        assert_eq!(parse_cpuinfo(""), (None, None));
    }

    #[test]
    fn parses_meminfo_fields() {
        let text = "MemTotal:         121364 kB\nMemFree:           45368 kB\nMemAvailable:      86192 kB\n";
        assert_eq!(parse_meminfo(text, "MemTotal"), Some(121364));
        assert_eq!(parse_meminfo(text, "MemAvailable"), Some(86192));
        assert_eq!(parse_meminfo(text, "SwapTotal"), None);
    }

    #[test]
    fn parses_uptime_and_loadavg() {
        assert_eq!(parse_uptime("3661.42 7300.10\n"), Some(3661));
        assert_eq!(parse_uptime(""), None);
        assert_eq!(parse_loadavg("0.10 0.05 0.01 1/42 123\n"), Some("0.10 0.05 0.01".into()));
        assert_eq!(parse_loadavg("0.10 0.05\n"), None);
    }

    #[test]
    fn parses_serial_and_skips_empty_values() {
        let cmdline = "coherent_pool=4k androidboot.serialno=D200TEST0000000000 rootwait\n";
        assert_eq!(parse_serial(cmdline), Some("D200TEST0000000000".into()));
        assert_eq!(parse_serial(""), None);
        let mut out = String::new();
        push(&mut out, "a", "1");
        push(&mut out, "b", "");
        assert_eq!(out, "a=1\n");
    }

    #[test]
    fn collects_real_system_facts() {
        let out = collect();
        assert!(out.contains("cpu_cores="), "{out}");
        assert!(out.contains("mem_total_kb="), "{out}");
        assert!(out.contains("arch="), "{out}");
        assert!(out.contains("root_total_bytes="), "{out}");
    }
}
