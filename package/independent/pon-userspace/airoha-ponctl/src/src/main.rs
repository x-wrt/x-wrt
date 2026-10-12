// SPDX-License-Identifier: GPL-2.0-only
use std::env;
use std::error::Error;
use std::fs;
use std::path::{Path, PathBuf};

mod netlink;
mod status;

use netlink::LineMode;

const NET_CLASS: &str = "/sys/class/net";
const ARPHRD_NONE: &str = "65534";
const VERSION: &str = env!("CARGO_PKG_VERSION");
const PROJECT_URL: &str = "https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-ponctl";

type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Debug, Clone)]
struct PonDevice {
    name: String,
    net_path: PathBuf,
    xpon_path: PathBuf,
}

fn read_trimmed(path: &Path) -> Result<String> {
    Ok(fs::read_to_string(path)?.trim().to_owned())
}

fn write_value(path: &Path, value: &str) -> Result<()> {
    fs::write(path, format!("{value}\n"))?;
    Ok(())
}

fn discover_pon_devices() -> Result<Vec<PonDevice>> {
    let mut devices = Vec::new();

    for entry in fs::read_dir(NET_CLASS)? {
        let entry = entry?;
        let net_path = entry.path();
        let xpon_path = net_path.join("xpon");

        if !xpon_path.join("serial_number").is_file() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        devices.push(PonDevice {
            name,
            net_path,
            xpon_path,
        });
    }
    devices.sort_by(|left, right| left.name.cmp(&right.name));
    Ok(devices)
}

fn select_pon_device(requested: Option<&str>) -> Result<PonDevice> {
    let devices = discover_pon_devices()?;

    if let Some(name) = requested {
        return devices
            .into_iter()
            .find(|device| device.name == name)
            .ok_or_else(|| {
                format!("PON device '{name}' was not found or has no xpon binding").into()
            });
    }

    match devices.len() {
        0 => Err("no PON device with an xpon binding was found".into()),
        1 => Ok(devices.into_iter().next().expect("length checked")),
        _ => Err(format!(
            "multiple PON devices are present ({}); select one with --device ponX",
            devices
                .iter()
                .map(|device| device.name.as_str())
                .collect::<Vec<_>>()
                .join(", ")
        )
        .into()),
    }
}

fn associated_controls(device: &PonDevice) -> Result<Vec<String>> {
    let pon_ifindex = read_trimmed(&device.net_path.join("ifindex"))?;
    let mut matches = Vec::new();

    for entry in fs::read_dir(NET_CLASS)? {
        let entry = entry?;
        let path = entry.path();
        if path == device.net_path {
            continue;
        }
        if read_trimmed(&path.join("type")).ok().as_deref() != Some(ARPHRD_NONE) {
            continue;
        }
        if read_trimmed(&path.join("iflink")).ok().as_deref() != Some(&pon_ifindex) {
            continue;
        }
        matches.push(entry.file_name().to_string_lossy().into_owned());
    }
    matches.sort();

    Ok(matches)
}

fn normalize_serial(input: &str) -> Result<String> {
    let input = input.trim();
    if input.is_empty() || input.eq_ignore_ascii_case("default") {
        return Ok("default".to_owned());
    }
    if input.len() == 16 && input.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Ok(input.to_ascii_lowercase());
    }
    if input.len() == 12 {
        let bytes = input.as_bytes();
        let vendor = &bytes[..4];
        let serial = &bytes[4..];
        if vendor.iter().all(|byte| byte.is_ascii_alphanumeric())
            && serial.iter().all(|byte| byte.is_ascii_hexdigit())
        {
            let mut normalized = String::with_capacity(16);
            for byte in vendor {
                normalized.push_str(&format!("{byte:02x}"));
            }
            normalized.push_str(&input[4..].to_ascii_lowercase());
            return Ok(normalized);
        }
    }
    Err("serial must be 'default', 16 raw hex digits, or VEND followed by 8 hex digits".into())
}

fn hex_encode_padded(input: &[u8], width: usize) -> String {
    let mut output = String::with_capacity(width * 2);
    for byte in input {
        output.push_str(&format!("{byte:02x}"));
    }
    for _ in input.len()..width {
        output.push_str("00");
    }
    output
}

fn normalize_registration_id(input: &str) -> Result<String> {
    let input = input.trim();
    if input.is_empty() || input.eq_ignore_ascii_case("default") {
        return Ok("default".to_owned());
    }

    if let Some(raw) = input.strip_prefix("hex:") {
        if raw.len() == 72 && raw.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Ok(raw.to_ascii_lowercase());
        }
        return Err("hex Registration-ID must contain exactly 72 hex digits".into());
    }

    let text = input.strip_prefix("text:").unwrap_or(input);
    if input.len() == 72 && input.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Ok(input.to_ascii_lowercase());
    }
    if text.len() > 36 {
        return Err("text Registration-ID must not exceed 36 UTF-8 bytes".into());
    }
    Ok(hex_encode_padded(text.as_bytes(), 36))
}

fn show_identity(device: &PonDevice) -> Result<()> {
    println!(
        "serial_number: {}",
        read_trimmed(&device.xpon_path.join("serial_number"))?
    );
    println!(
        "active_serial_number: {}",
        read_trimmed(&device.xpon_path.join("active_serial_number"))?
    );
    println!(
        "registration_id: {}",
        read_trimmed(&device.xpon_path.join("registration_id"))?
    );
    Ok(())
}

fn set_identity(device: &PonDevice, arguments: &[String]) -> Result<()> {
    let mut serial = None;
    let mut registration = None;
    let mut options = arguments.chunks_exact(2);

    for option in &mut options {
        match option[0].as_str() {
            "--serial" => serial = Some(normalize_serial(&option[1])?),
            "--registration-id" => registration = Some(normalize_registration_id(&option[1])?),
            name => return Err(format!("unknown argument '{name}'").into()),
        }
    }
    if let Some(name) = options.remainder().first() {
        return Err(format!("{name} requires a value").into());
    }

    if serial.is_none() && registration.is_none() {
        return Err("identity set requires --serial or --registration-id".into());
    }
    if let Some(serial) = serial {
        write_value(&device.xpon_path.join("serial_number"), &serial)?;
    }
    if let Some(registration) = registration {
        write_value(&device.xpon_path.join("registration_id"), &registration)?;
    }
    show_identity(device)
}

fn line_ifindex(device: &PonDevice) -> Result<u32> {
    Ok(read_trimmed(&device.net_path.join("ifindex"))?.parse()?)
}

fn show_mode(device: &PonDevice) -> Result<()> {
    let state = netlink::get_line(line_ifindex(device)?)?;
    println!("configured={}", state.configured.name());
    println!(
        "active={}",
        state.active.map(LineMode::name).unwrap_or("none")
    );
    let controls = associated_controls(device)?;
    println!(
        "controls={}",
        if controls.is_empty() {
            "none".to_owned()
        } else {
            controls.join(",")
        }
    );
    Ok(())
}

fn print_help() {
    println!(
        "ponctl {VERSION}\n\
         {PROJECT_URL}\n\
         \n\
         Usage:\n\
         \x20 ponctl list\n\
         \x20 ponctl [--device ponX] status [--json]\n\
         \x20 ponctl [--device ponX] control|identity\n\
         \x20 ponctl [--device ponX] mode [show]\n\
         \x20 ponctl [--device ponX] mode set MODE\n\
         \x20 ponctl [--device ponX] identity set [--serial SN] [--registration-id ID]\n\
         \x20 ponctl [--device ponX] data-path [show]"
    );
}

fn run() -> Result<()> {
    let mut arguments: Vec<String> = env::args().skip(1).collect();
    let mut requested_device = None;

    if matches!(
        arguments.first().map(String::as_str),
        Some("-d" | "--device")
    ) {
        if arguments.len() < 2 {
            return Err("--device requires a netdev name".into());
        }
        requested_device = Some(arguments.remove(1));
        arguments.remove(0);
    }
    let command = arguments.first().map(String::as_str).unwrap_or("help");

    if command == "help" || command == "--help" || command == "-h" {
        print_help();
        return Ok(());
    }
    if command == "--version" || command == "-V" {
        println!("ponctl {VERSION}\n{PROJECT_URL}");
        return Ok(());
    }
    if command == "list" {
        if requested_device.is_some() || arguments.len() != 1 {
            return Err("list does not accept --device or additional arguments".into());
        }
        let devices = discover_pon_devices()?;
        if devices.is_empty() {
            return Err("no PON device with an xpon binding was found".into());
        }
        for device in devices {
            let controls = associated_controls(&device)?;
            println!(
                "device={} controls={} xpon={}",
                device.name,
                if controls.is_empty() {
                    "none".to_owned()
                } else {
                    controls.join(",")
                },
                device.xpon_path.display()
            );
        }
        return Ok(());
    }

    let device = select_pon_device(requested_device.as_deref())?;
    match command {
        "status" if arguments.len() == 1 || (arguments.len() == 2 && arguments[1] == "--json") => {
            let snapshot = netlink::get_status(line_ifindex(&device)?)?;
            if arguments.len() == 2 {
                println!("{}", status::json(&snapshot));
            } else {
                print!("{}", status::human(&snapshot));
            }
        }
        "control" if arguments.len() == 1 => {
            for control in associated_controls(&device)? {
                println!("{control}");
            }
        }
        "mode" if arguments.len() == 1 || arguments.get(1).map(String::as_str) == Some("show") => {
            if arguments.len() > 2 {
                return Err("mode show accepts no additional arguments".into());
            }
            show_mode(&device)?;
        }
        "mode" if arguments.get(1).map(String::as_str) == Some("set") => {
            if arguments.len() != 3 {
                return Err("mode set requires one mode name".into());
            }
            let mode = LineMode::parse(&arguments[2])
                .ok_or("mode must be gpon, xgpon, xgspon, epon-1g, epon-10g-1g, or epon-10g-10g")?;
            netlink::set_line(line_ifindex(&device)?, mode)?;
            show_mode(&device)?;
        }
        "identity"
            if arguments.len() == 1 || arguments.get(1).map(String::as_str) == Some("show") =>
        {
            if arguments.len() > 2 {
                return Err("identity show accepts no additional arguments".into());
            }
            show_identity(&device)?;
        }
        "identity" if arguments.get(1).map(String::as_str) == Some("set") => {
            set_identity(&device, &arguments[2..])?;
        }
        "data-path"
            if arguments.len() == 1 || arguments.get(1).map(String::as_str) == Some("show") =>
        {
            if arguments.len() > 2 {
                return Err("data-path show accepts no additional arguments".into());
            }
            print!(
                "{}",
                fs::read_to_string(device.xpon_path.join("data_path"))?
            );
        }
        _ => {
            return Err(
                format!("unknown or malformed command '{command}'; run 'ponctl --help'").into(),
            );
        }
    }
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("ponctl: {error}");
        std::process::exit(1);
    }
}
