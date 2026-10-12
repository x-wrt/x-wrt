// SPDX-License-Identifier: GPL-2.0-only

use std::collections::VecDeque;
use std::fs;
use std::io::{self, BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use super::backend::BackendStatus;
use super::protocol::Request;
use super::provisioning::{DataPathGraph, ProvisioningSnapshot};

const CONTROL_PROTOCOL_VERSION: u8 = 1;
const EVENT_CAPACITY: usize = 256;

#[derive(Clone, Debug)]
struct Transaction {
    tci: u16,
    action: u8,
    class_id: u16,
    entity_id: u16,
    attribute_mask: u16,
    result: Option<u8>,
}

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
    loid_configured: bool,
    rx_messages: u64,
    tx_messages: u64,
    parse_errors: u64,
    authentication_status: Option<u8>,
    authentication_meaning: &'static str,
    provisioning: ProvisioningSnapshot,
    data_path_graph: DataPathGraph,
    backend: BackendStatus,
    last_transaction: Option<Transaction>,
    event_sequence: u64,
    events: VecDeque<Event>,
}

#[derive(Clone, Debug)]
pub struct StatusHub {
    shared: Arc<Mutex<Inner>>,
}

impl StatusHub {
    pub fn new(interface: &str, loid_configured: bool) -> Self {
        let hub = Self {
            shared: Arc::new(Mutex::new(Inner {
                interface: interface.to_owned(),
                loid_configured,
                rx_messages: 0,
                tx_messages: 0,
                parse_errors: 0,
                authentication_status: None,
                authentication_meaning: "not-reported",
                provisioning: ProvisioningSnapshot::default(),
                data_path_graph: DataPathGraph::default(),
                backend: BackendStatus::default(),
                last_transaction: None,
                event_sequence: 0,
                events: VecDeque::with_capacity(EVENT_CAPACITY),
            })),
        };
        hub.record_event("lifecycle", "OMCI daemon started".to_owned());
        hub
    }

    pub fn record_rx(&self, request: &Request<'_>) {
        let mut inner = self.shared.lock().expect("status mutex poisoned");
        inner.rx_messages += 1;
        inner.last_transaction = Some(Transaction {
            tci: request.tci,
            action: request.action,
            class_id: request.class_id,
            entity_id: request.entity_id,
            attribute_mask: request.attribute_mask,
            result: None,
        });
        drop(inner);
        self.record_event(
            "rx",
            format!(
                "tci=0x{:04x} action=0x{:02x} class={} entity={} {}=0x{:04x}",
                request.tci,
                request.action,
                request.class_id,
                request.entity_id,
                request.selector_name(),
                request.attribute_mask
            ),
        );
    }

    pub fn record_tx(&self, request: &Request<'_>, result: Option<u8>, retransmission: bool) {
        let mut inner = self.shared.lock().expect("status mutex poisoned");
        inner.tx_messages += 1;
        inner.last_transaction = Some(Transaction {
            tci: request.tci,
            action: request.action,
            class_id: request.class_id,
            entity_id: request.entity_id,
            attribute_mask: request.attribute_mask,
            result,
        });
        drop(inner);
        self.record_event(
            "tx",
            format!(
                "tci=0x{:04x} action=0x{:02x} class={} entity={} result={} retransmission={}",
                request.tci,
                request.action,
                request.class_id,
                request.entity_id,
                result
                    .map(|value| format!("0x{value:02x}"))
                    .unwrap_or_else(|| "none".to_owned()),
                retransmission
            ),
        );
    }

    pub fn record_parse_error(&self, error: &str) {
        let mut inner = self.shared.lock().expect("status mutex poisoned");
        inner.parse_errors += 1;
        drop(inner);
        self.record_event("parse-error", error.to_owned());
    }

    pub fn record_authentication_status(&self, status: u8, meaning: &'static str) {
        let mut inner = self.shared.lock().expect("status mutex poisoned");
        inner.authentication_status = Some(status);
        inner.authentication_meaning = meaning;
        drop(inner);
        self.record_event(
            "authentication",
            format!("status={status} meaning={meaning}"),
        );
    }

    pub fn record_transport_error(&self, error: &str) {
        self.record_event("transport-error", error.to_owned());
    }

    pub fn record_provisioning(
        &self,
        provisioning: ProvisioningSnapshot,
        data_path_graph: DataPathGraph,
        backend: BackendStatus,
    ) {
        let mut inner = self.shared.lock().expect("status mutex poisoned");
        inner.provisioning = provisioning;
        inner.data_path_graph = data_path_graph;
        inner.backend = backend;
    }

    fn record_event(&self, kind: &'static str, message: String) {
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
    {
        let mut reader = BufReader::new(&stream);
        reader.read_line(&mut request)?;
    }
    let fields: Vec<&str> = request.split_whitespace().collect();

    match fields.as_slice() {
        ["STATUS", "1"] => {
            let inner = status.shared.lock().expect("status mutex poisoned");
            writeln!(stream, "{}", status_json(&inner))
        }
        ["DATAPATH", "1"] => {
            let inner = status.shared.lock().expect("status mutex poisoned");
            writeln!(stream, "{}", data_path_json(&inner))
        }
        ["EVENTS", "1", after] => {
            let after = after
                .parse::<u64>()
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid event cursor"))?;
            write_events(&mut stream, status, after)
        }
        _ => {
            writeln!(
                stream,
                "{{\"error\":\"unsupported control request\",\"protocol_version\":{CONTROL_PROTOCOL_VERSION}}}"
            )
        }
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
    let carrier = read_carrier(&inner.interface);
    let carrier_json = carrier
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let authentication_status = inner
        .authentication_status
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let last_transaction = inner
        .last_transaction
        .as_ref()
        .map(transaction_json)
        .unwrap_or_else(|| "null".to_owned());
    let vlan_ids = inner
        .provisioning
        .vlan_ids
        .iter()
        .map(u16::to_string)
        .collect::<Vec<_>>()
        .join(",");
    let multicast_vlan_ids = inner
        .provisioning
        .multicast_vlan_ids
        .iter()
        .map(u16::to_string)
        .collect::<Vec<_>>()
        .join(",");
    let igmp_upstream_vlan_ids = inner
        .provisioning
        .igmp_upstream_vlan_ids
        .iter()
        .map(u16::to_string)
        .collect::<Vec<_>>()
        .join(",");
    let igmp_upstream_tag_controls = inner
        .provisioning
        .igmp_upstream_tag_controls
        .iter()
        .map(u8::to_string)
        .collect::<Vec<_>>()
        .join(",");
    let broadcast_key_indexes = inner
        .provisioning
        .broadcast_key_indexes
        .iter()
        .map(u8::to_string)
        .collect::<Vec<_>>()
        .join(",");
    let data_path_active = carrier != Some(false);
    let active_alloc_id = data_path_active
        .then_some(inner.backend.active_alloc_id)
        .flatten()
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let active_gem_id = data_path_active
        .then_some(inner.backend.active_gem_id)
        .flatten()
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    let backend_state = if data_path_active {
        inner.backend.state
    } else {
        "inactive"
    };
    let data_path_all_vlans = data_path_active && inner.backend.all_vlans;
    let backend_error = inner
        .backend
        .error
        .as_deref()
        .map(json_string)
        .unwrap_or_else(|| "null".to_owned());

    format!(
        concat!(
            "{{\"protocol_version\":{},\"interface\":{},",
            "\"channel_available\":{},\"loid_configured\":{},",
            "\"authentication_status\":{},\"authentication_meaning\":{},",
            "\"olt_vendor_id\":{},\"olt_equipment_id\":{},\"olt_version\":{},",
            "\"rx_messages\":{},\"tx_messages\":{},\"parse_errors\":{},",
            "\"tcont_count\":{},\"gem_port_count\":{},",
            "\"gem_iwtp_count\":{},\"vlan_rule_count\":{},",
            "\"vlan_ids\":[{}],\"multicast_vlan_ids\":[{}],",
            "\"igmp_upstream_vlan_ids\":[{}],",
            "\"igmp_upstream_tag_controls\":[{}],",
            "\"enhanced_security\":{},\"broadcast_key_indexes\":[{}],",
            "\"data_path_candidate_count\":{},",
            "\"data_path_all_vlans\":{},",
            "\"backend_state\":{},\"active_alloc_id\":{},",
            "\"active_gem_id\":{},\"backend_error\":{},",
            "\"event_sequence\":{},\"last_transaction\":{}}}"
        ),
        CONTROL_PROTOCOL_VERSION,
        json_string(&inner.interface),
        carrier_json,
        inner.loid_configured,
        authentication_status,
        json_string(inner.authentication_meaning),
        json_string(&inner.provisioning.olt_vendor_id),
        json_string(&inner.provisioning.olt_equipment_id),
        json_string(&inner.provisioning.olt_version),
        inner.rx_messages,
        inner.tx_messages,
        inner.parse_errors,
        inner.provisioning.configured_tconts,
        inner.provisioning.gem_ports,
        inner.provisioning.gem_interworking_tps,
        inner.provisioning.vlan_rules,
        vlan_ids,
        multicast_vlan_ids,
        igmp_upstream_vlan_ids,
        igmp_upstream_tag_controls,
        inner.provisioning.enhanced_security,
        broadcast_key_indexes,
        inner.provisioning.data_paths.len(),
        data_path_all_vlans,
        json_string(backend_state),
        active_alloc_id,
        active_gem_id,
        backend_error,
        inner.event_sequence,
        last_transaction
    )
}

fn transaction_json(transaction: &Transaction) -> String {
    let result = transaction
        .result
        .map(|value| value.to_string())
        .unwrap_or_else(|| "null".to_owned());
    format!(
        concat!(
            "{{\"tci\":{},\"action\":{},\"class_id\":{},\"entity_id\":{},",
            "\"attribute_mask\":{},\"result\":{}}}"
        ),
        transaction.tci,
        transaction.action,
        transaction.class_id,
        transaction.entity_id,
        transaction.attribute_mask,
        result
    )
}

fn data_path_json(inner: &Inner) -> String {
    let mut candidates = String::new();
    for (index, candidate) in inner.data_path_graph.candidates.iter().enumerate() {
        if index != 0 {
            candidates.push(',');
        }
        use std::fmt::Write as _;
        let _ = write!(
            candidates,
            concat!(
                "{{\"alloc_id\":{},\"gem_id\":{},\"vlan_id\":{},",
                "\"pbit_mask\":{},\"multicast\":{}}}"
            ),
            candidate.alloc_id,
            candidate.gem_id,
            candidate.vlan_id,
            candidate.pbit_mask,
            candidate.multicast
        );
    }

    let mut entities = String::new();
    for (entity_index, entity) in inner.data_path_graph.entities.iter().enumerate() {
        if entity_index != 0 {
            entities.push(',');
        }
        let mut attributes = String::new();
        for (attribute_index, attribute) in entity.attributes.iter().enumerate() {
            if attribute_index != 0 {
                attributes.push(',');
            }
            let mut rows = String::new();
            for (row_index, row) in attribute.table_rows.iter().enumerate() {
                if row_index != 0 {
                    rows.push(',');
                }
                rows.push_str("{\"key\":");
                rows.push_str(&json_string(&hex_bytes(&row.key)));
                rows.push_str(",\"value\":");
                rows.push_str(&json_string(&hex_bytes(&row.value)));
                rows.push('}');
            }
            use std::fmt::Write as _;
            let _ = write!(
                attributes,
                "{{\"index\":{},\"value\":{},\"table_rows\":[{}]}}",
                attribute.index,
                json_string(&hex_bytes(&attribute.value)),
                rows
            );
        }
        use std::fmt::Write as _;
        let _ = write!(
            entities,
            concat!(
                "{{\"class_id\":{},\"class_name\":{},",
                "\"entity_id\":{},\"attributes\":[{}]}}"
            ),
            entity.class_id,
            json_string(data_path_class_name(entity.class_id)),
            entity.entity_id,
            attributes
        );
    }

    let backend_error = inner
        .backend
        .error
        .as_deref()
        .map(json_string)
        .unwrap_or_else(|| "null".to_owned());
    format!(
        concat!(
            "{{\"protocol_version\":{},\"backend_state\":{},",
            "\"backend_error\":{},\"data_path_all_vlans\":{},",
            "\"candidates\":[{}],\"entities\":[{}]}}"
        ),
        CONTROL_PROTOCOL_VERSION,
        json_string(inner.backend.state),
        backend_error,
        inner.backend.all_vlans,
        candidates,
        entities
    )
}

fn data_path_class_name(class_id: u16) -> &'static str {
    match class_id {
        47 => "MAC Bridge Port Configuration Data",
        84 => "VLAN Tagging Filter Data",
        130 => "IEEE 802.1p Mapper Service Profile",
        171 => "Extended VLAN Tagging Operation Configuration Data",
        266 => "GEM Interworking Termination Point",
        268 => "GEM Port Network CTP",
        280 => "Traffic Descriptor",
        281 => "Multicast GEM Interworking Termination Point",
        309 => "Multicast Operations Profile",
        310 => "Multicast Subscriber Configuration Info",
        _ => "Unknown",
    }
}

fn hex_bytes(bytes: &[u8]) -> String {
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(output, "{byte:02x}");
    }
    output
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
    let carrier = fs::read_to_string(format!("/sys/class/net/{interface}/carrier")).ok()?;
    match carrier.trim() {
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
