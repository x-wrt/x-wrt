// SPDX-License-Identifier: GPL-2.0-only

use std::env;
use std::io::{self, Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use crate::config::PonConfig;
use crate::{oam, omci};

const VERSION: &str = env!("CARGO_PKG_VERSION");
const PROJECT_URL: &str =
    "https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-pon-daemons";

enum Protocol {
    Omci,
    Oam,
}

fn protocol_for_mode(mode: &str) -> Protocol {
    if mode.starts_with("epon-") {
        Protocol::Oam
    } else {
        Protocol::Omci
    }
}

fn control_socket(line: &str) -> PathBuf {
    PathBuf::from(format!("/var/run/airoha-pond.{line}.sock"))
}

fn run_line(config_path: &Path, line_name: &str) -> io::Result<()> {
    let config = PonConfig::load(config_path)?;
    let line = config.section(line_name, "xpon")?;
    let mode = line.option("mode").unwrap_or("");
    let socket = control_socket(line_name);

    match protocol_for_mode(mode) {
        Protocol::Omci => {
            let section = config.linked_section("omci", line_name)?;
            let interface = required_option(section, "device")?;
            let identity = omci::IdentityConfig::from_section(section);
            println!(
                "PON agent selected OMCI: line={} interface={} mode={}",
                line_name,
                interface,
                display_mode(mode)
            );
            omci::run_agent(interface, identity, &socket)
        }
        Protocol::Oam => {
            let section = config.linked_section("oam", line_name)?;
            let interface = required_option(section, "device")?;
            let oam_config = oam::OamConfig::from_section(section)?;
            println!(
                "PON agent selected OAM: line={} interface={} mode={}",
                line_name,
                interface,
                display_mode(mode)
            );
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

fn display_mode(mode: &str) -> &str {
    if mode.is_empty() {
        "driver-default"
    } else {
        mode
    }
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
        assert!(matches!(protocol_for_mode("epon-1g"), Protocol::Oam));
        assert!(matches!(protocol_for_mode("epon-10g-1g"), Protocol::Oam));
        assert!(matches!(protocol_for_mode("epon-10g-10g"), Protocol::Oam));
    }

    #[test]
    fn selects_omci_for_xgpon_and_driver_default() {
        assert!(matches!(protocol_for_mode("xgpon"), Protocol::Omci));
        assert!(matches!(protocol_for_mode(""), Protocol::Omci));
    }
}
