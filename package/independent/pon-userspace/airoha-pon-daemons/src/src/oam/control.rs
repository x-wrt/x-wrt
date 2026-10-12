// SPDX-License-Identifier: GPL-2.0-only

use std::collections::VecDeque;
use std::fs;
use std::io::{self, BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use super::ctc::Snapshot;

const CONTROL_PROTOCOL_VERSION: u8 = 1;
const EVENT_CAPACITY: usize = 256;

#[derive(Clone, Debug)]
struct Event {
    sequence: u64,
    timestamp_ms: u128,
    kind: &'static str,
    message: String,
}

#[derive(Debug)]
struct Inner {
    interface: String,
    operator: String,
    loid_configured: bool,
    ieee_operational: bool,
    rx_messages: u64,
    tx_messages: u64,
    parse_errors: u64,
    information_rx: u64,
    event_rx: u64,
    organization_rx: u64,
    last_code: Option<u8>,
    ctc: Snapshot,
    event_sequence: u64,
    events: VecDeque<Event>,
}

#[derive(Clone)]
pub struct StatusHub {
    shared: Arc<Mutex<Inner>>,
}

impl StatusHub {
    pub fn new(interface: &str, operator: &str, loid_configured: bool, ctc: Snapshot) -> Self {
        let hub = Self {
            shared: Arc::new(Mutex::new(Inner {
                interface: interface.to_owned(),
                operator: operator.to_owned(),
                loid_configured,
                ieee_operational: false,
                rx_messages: 0,
                tx_messages: 0,
                parse_errors: 0,
                information_rx: 0,
                event_rx: 0,
                organization_rx: 0,
                last_code: None,
                ctc,
                event_sequence: 0,
                events: VecDeque::with_capacity(EVENT_CAPACITY),
            })),
        };
        hub.record_event("lifecycle", "OAM daemon started".to_owned());
        hub
    }

    pub fn record_rx(&self, code: u8) {
        let mut inner = self.shared.lock().expect("status mutex poisoned");
        inner.rx_messages += 1;
        inner.last_code = Some(code);
        match code {
            0x00 => inner.information_rx += 1,
            0x01 => inner.event_rx += 1,
            0xfe => inner.organization_rx += 1,
            _ => {}
        }
    }

    pub fn record_tx(&self) {
        self.shared
            .lock()
            .expect("status mutex poisoned")
            .tx_messages += 1;
    }

    pub fn record_parse_error(&self, error: &str) {
        self.shared
            .lock()
            .expect("status mutex poisoned")
            .parse_errors += 1;
        self.record_event("parse-error", error.to_owned());
    }

    pub fn record_ieee_operational(&self) {
        let mut inner = self.shared.lock().expect("status mutex poisoned");

        /* The first successful Information exchange marks IEEE OAM operational. */
        if !inner.ieee_operational {
            inner.ieee_operational = true;
            drop(inner);
            self.record_event("ieee-oam", "IEEE OAM discovery completed".to_owned());
        }
    }

    pub fn update_ctc(&self, snapshot: Snapshot) {
        self.shared.lock().expect("status mutex poisoned").ctc = snapshot;
    }

    pub fn record_event(&self, kind: &'static str, message: String) {
        let mut inner = self.shared.lock().expect("status mutex poisoned");
        inner.event_sequence += 1;
        let sequence = inner.event_sequence;
        if inner.events.len() == EVENT_CAPACITY {
            inner.events.pop_front();
        }
        inner.events.push_back(Event {
            sequence,
            timestamp_ms: now_ms(),
            kind,
            message,
        });
    }
}

pub fn start_server(
    socket_path: &Path,
    status: StatusHub,
) -> io::Result<crate::control::ControlServer> {
    crate::control::ControlServer::start(socket_path, move |stream| handle_client(stream, &status))
}

fn handle_client(mut stream: UnixStream, status: &StatusHub) -> io::Result<()> {
    let mut request = String::new();
    BufReader::new(&stream).read_line(&mut request)?;
    let fields: Vec<&str> = request.split_whitespace().collect();

    match fields.as_slice() {
        ["STATUS", "1"] => {
            let inner = status.shared.lock().expect("status mutex poisoned");
            writeln!(stream, "{}", status_json(&inner))
        }
        ["EVENTS", "1", after] => {
            let after = after
                .parse::<u64>()
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid event cursor"))?;
            write_events(&mut stream, status, after)
        }
        _ => writeln!(
            stream,
            "{{\"error\":\"unsupported control request\",\"protocol_version\":{CONTROL_PROTOCOL_VERSION}}}"
        ),
    }
}

fn write_events(stream: &mut UnixStream, status: &StatusHub, after: u64) -> io::Result<()> {
    let inner = status.shared.lock().expect("status mutex poisoned");
    for event in inner.events.iter().filter(|event| event.sequence > after) {
        writeln!(stream, "{}", event_json(event))?;
    }
    stream.flush()
}

fn status_json(inner: &Inner) -> String {
    let carrier = read_carrier(&inner.interface)
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let selected_version = inner
        .ctc
        .selected_version
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let authentication_failure = inner
        .ctc
        .authentication_failure
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let last_code = inner
        .last_code
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let vlan_mode = inner
        .ctc
        .vlan_mode
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let vlan_object = inner
        .ctc
        .vlan_object
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let vlan_ids = inner
        .ctc
        .vlan_ids
        .iter()
        .map(u16::to_string)
        .collect::<Vec<_>>()
        .join(",");

    format!(
        concat!(
            "{{\"protocol_version\":{},\"interface\":{},\"channel_available\":{},",
            "\"operator\":{},\"ieee_discovery_completed\":{},\"loid_configured\":{},",
            "\"rx_messages\":{},",
            "\"tx_messages\":{},\"parse_errors\":{},\"information_rx\":{},",
            "\"event_rx\":{},\"organization_rx\":{},\"last_code\":{},",
            "\"ctc_discovery_state\":{},\"ctc_version\":{},",
            "\"authentication_status\":{},\"authentication_failure\":{},",
            "\"ctc_get_requests\":{},\"ctc_set_requests\":{},",
            "\"ctc_unsupported_requests\":{},\"vlan_object\":{},\"vlan_mode\":{},",
            "\"vlan_ids\":[{}],\"event_sequence\":{}}}"
        ),
        CONTROL_PROTOCOL_VERSION,
        json_string(&inner.interface),
        carrier,
        json_string(&inner.operator),
        inner.ieee_operational,
        inner.loid_configured,
        inner.rx_messages,
        inner.tx_messages,
        inner.parse_errors,
        inner.information_rx,
        inner.event_rx,
        inner.organization_rx,
        last_code,
        json_string(inner.ctc.discovery_state),
        selected_version,
        json_string(inner.ctc.authentication),
        authentication_failure,
        inner.ctc.get_requests,
        inner.ctc.set_requests,
        inner.ctc.unsupported_requests,
        vlan_object,
        vlan_mode,
        vlan_ids,
        inner.event_sequence,
    )
}

fn event_json(event: &Event) -> String {
    format!(
        "{{\"sequence\":{},\"timestamp_ms\":{},\"kind\":{},\"message\":{}}}",
        event.sequence,
        event.timestamp_ms,
        json_string(event.kind),
        json_string(&event.message)
    )
}

fn json_string(value: &str) -> String {
    let mut output = String::with_capacity(value.len() + 2);
    output.push('"');
    for character in value.chars() {
        match character {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            character if character.is_control() => {
                use std::fmt::Write as _;
                let _ = write!(output, "\\u{:04x}", character as u32);
            }
            character => output.push(character),
        }
    }
    output.push('"');
    output
}

fn read_carrier(interface: &str) -> Option<bool> {
    match fs::read_to_string(format!("/sys/class/net/{interface}/carrier"))
        .ok()?
        .trim()
    {
        "0" => Some(false),
        "1" => Some(true),
        _ => None,
    }
}

fn now_ms() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
}
