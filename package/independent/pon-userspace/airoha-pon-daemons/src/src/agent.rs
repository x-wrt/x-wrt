// SPDX-License-Identifier: GPL-2.0-only

use std::env;
use std::io::{self, Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

use crate::config::{PonConfig, Section};
use crate::{oam, omci};

const VERSION: &str = env!("CARGO_PKG_VERSION");
const PROJECT_URL: &str =
    "https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-pon-daemons";

enum Protocol {
    Omci,
    Oam,
}

fn protocol_for_mode(mode: &str) -> io::Result<Protocol> {
    match mode {
        "epon-1g" | "epon-10g-1g" | "epon-10g-10g" => Ok(Protocol::Oam),
        "gpon" | "xgpon" | "xgspon" => Ok(Protocol::Omci),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("unknown PON line mode '{mode}'"),
        )),
    }
}

fn resolve_mode(
    line: &Section,
    read_mode: impl FnOnce(&str) -> io::Result<String>,
) -> io::Result<String> {
    let configured = line.option("mode").unwrap_or("");
    let mode = if configured.is_empty() {
        read_mode(required_option(line, "device")?)?
    } else {
        configured.to_owned()
    };
    protocol_for_mode(&mode)?;
    Ok(mode)
}

fn read_driver_mode(device: &str) -> io::Result<String> {
    let output = Command::new("/usr/sbin/ponctl")
        .args(["--device", device, "mode", "effective"])
        .output()?;
    if !output.status.success() {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "cannot read PON mode for {device}: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            ),
        ));
    }
    let mode = std::str::from_utf8(&output.stdout)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
    Ok(mode.trim().to_owned())
}

fn enable_control_interface(interface: &str) -> io::Result<()> {
    let status = Command::new("ip")
        .args(["link", "set", "dev", interface, "up"])
        .status()?;
    if !status.success() {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!("cannot bring up PON control interface {interface}: {status}"),
        ));
    }
    Ok(())
}

fn control_socket(line: &str) -> PathBuf {
    PathBuf::from(format!("/var/run/airoha-pond.{line}.sock"))
}

fn run_line(config_path: &Path, line_name: &str) -> io::Result<()> {
    let config = PonConfig::load(config_path)?;
    let line = config.section(line_name, "xpon")?;
    let mode = resolve_mode(line, read_driver_mode)?;
    let socket = control_socket(line_name);

    match protocol_for_mode(&mode)? {
        Protocol::Omci => {
            let section = config.linked_section("omci", line_name)?;
            let interface = required_option(section, "device")?;
            let identity = omci::IdentityConfig::from_section(section);
            println!(
                "PON agent selected OMCI: line={} interface={} mode={}",
                line_name, interface, mode
            );
            enable_control_interface(interface)?;
            omci::run_agent(interface, identity, &socket)
        }
        Protocol::Oam => {
            let section = config.linked_section("oam", line_name)?;
            let interface = required_option(section, "device")?;
            let oam_config = oam::OamConfig::from_section(section)?;
            println!(
                "PON agent selected OAM: line={} interface={} mode={}",
                line_name, interface, mode
            );
            enable_control_interface(interface)?;
            oam::run_agent(interface, oam_config, &socket)
        }
    }
}

fn required_option<'a>(section: &'a crate::config::Section, name: &str) -> io::Result<&'a str> {
    section.option(name).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "UCI {} section '{}' has no {name} option",
                section.kind, section.name
            ),
        )
    })
}

fn print_help(program: &str) {
    println!("{program} {VERSION}\n{PROJECT_URL}\n");
    if program == "pondctl" {
        println!(
            "Usage:\n  pondctl status --line NAME\n  pondctl datapath --line NAME\n  pondctl events --line NAME [--after SEQUENCE]"
        );
    } else {
        println!("Usage:\n  airoha-pond --line NAME");
    }
}

fn print_version(program: &str) {
    println!("{program} {VERSION}\n{PROJECT_URL}");
}

fn run_control(line: &str, request: &str) -> io::Result<()> {
    let mut stream = UnixStream::connect(control_socket(line))?;
    writeln!(stream, "{request}")?;
    stream.shutdown(std::net::Shutdown::Write)?;

    let mut output = io::stdout().lock();
    let mut buffer = [0u8; 4096];
    loop {
        let length = stream.read(&mut buffer)?;
        if length == 0 {
            return Ok(());
        }
        output.write_all(&buffer[..length])?;
    }
}

fn execute_daemon(arguments: &[String]) -> io::Result<()> {
    match arguments {
        [flag, line] if flag == "--line" => run_line(Path::new("/etc/config/pon"), line),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "usage: airoha-pond --line NAME",
        )),
    }
}

fn execute_control(arguments: &[String]) -> io::Result<()> {
    match arguments {
        [command, flag, line] if flag == "--line" && command == "status" => {
            run_control(line, "STATUS 1")
        }
        [command, flag, line] if flag == "--line" && command == "datapath" => {
            run_control(line, "DATAPATH 1")
        }
        [command, flag, line] if flag == "--line" && command == "events" => {
            run_control(line, "EVENTS 1 0")
        }
        [command, flag, line, after_flag, after]
            if flag == "--line" && command == "events" && after_flag == "--after" =>
        {
            let sequence = after.parse::<u64>().map_err(|_| {
                io::Error::new(io::ErrorKind::InvalidInput, "invalid event sequence")
            })?;
            run_control(line, &format!("EVENTS 1 {sequence}"))
        }
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "run 'pondctl --help' for usage",
        )),
    }
}

pub fn run() -> ExitCode {
    let mut arguments = env::args();
    let program = arguments
        .next()
        .and_then(|path| {
            Path::new(&path)
                .file_name()
                .map(|name| name.to_string_lossy().into_owned())
        })
        .unwrap_or_else(|| "airoha-pond".to_owned());
    let program = if program == "pondctl" {
        "pondctl"
    } else {
        "airoha-pond"
    };
    let arguments = arguments.collect::<Vec<_>>();

    if arguments.is_empty()
        || matches!(arguments.as_slice(), [arg] if matches!(arg.as_str(), "help" | "--help" | "-h"))
    {
        print_help(program);
        return ExitCode::SUCCESS;
    }
    if matches!(arguments.as_slice(), [arg] if matches!(arg.as_str(), "--version" | "-V")) {
        print_version(program);
        return ExitCode::SUCCESS;
    }

    let result = if program == "pondctl" {
        execute_control(&arguments)
    } else {
        execute_daemon(&arguments)
    };

    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn selects_oam_for_epon_modes() {
        for mode in ["epon-1g", "epon-10g-1g", "epon-10g-10g"] {
            assert!(matches!(protocol_for_mode(mode), Ok(Protocol::Oam)));
        }
    }

    #[test]
    fn selects_omci_for_itu_modes() {
        for mode in ["gpon", "xgpon", "xgspon"] {
            assert!(matches!(protocol_for_mode(mode), Ok(Protocol::Omci)));
        }
    }

    #[test]
    fn inherits_driver_mode_for_empty_or_missing_uci_mode() {
        for option in ["", "\toption mode ''\n"] {
            let config = PonConfig::parse(&format!(
                "config xpon 'line0'\n\toption device 'pon1'\n{option}"
            ))
            .unwrap();
            let line = config.section("line0", "xpon").unwrap();
            for mode in ["epon-10g-1g", "xgpon"] {
                let resolved = resolve_mode(line, |device| {
                    assert_eq!(device, "pon1");
                    Ok(mode.to_owned())
                })
                .unwrap();
                assert_eq!(resolved, mode);
            }
        }
    }

    #[test]
    fn explicit_mode_does_not_query_the_driver() {
        let config =
            PonConfig::parse("config xpon 'line0'\n\toption mode 'epon-10g-1g'\n").unwrap();
        let resolved = resolve_mode(config.section("line0", "xpon").unwrap(), |_| {
            panic!("explicit mode must not query the driver")
        })
        .unwrap();
        assert_eq!(resolved, "epon-10g-1g");
    }

    #[test]
    fn missing_or_invalid_driver_mode_does_not_fall_back_to_omci() {
        let config =
            PonConfig::parse("config xpon 'line0'\n\toption device 'pon0'\n\toption mode ''\n")
                .unwrap();
        let line = config.section("line0", "xpon").unwrap();
        assert!(resolve_mode(line, |_| Err(io::Error::from(io::ErrorKind::NotFound))).is_err());
        for mode in ["", "none", "epon-unknown"] {
            assert!(resolve_mode(line, |_| Ok(mode.to_owned())).is_err());
        }
    }
}
