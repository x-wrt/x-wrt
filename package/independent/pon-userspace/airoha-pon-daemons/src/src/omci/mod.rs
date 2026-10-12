// SPDX-License-Identifier: GPL-2.0-only

//! ITU-T OMCI state machine, MIB, control plane, and data-path backend.
//!
//! The `airoha-xpon` kernel driver owns the XGTC, PLOAM, and BEN state machines.
//! Each OMCI request passes through `protocol` decoding, `mib` state updates,
//! and service graph generation. `backend` then applies the complete GEM/T-CONT
//! path table to the kernel, while `control` publishes the same state snapshot.

mod backend;
mod config;
mod control;
mod mib;
mod protocol;
mod provisioning;
mod schema;
mod security;

use std::collections::VecDeque;
use std::io;
use std::path::Path;

use self::backend::DataPathBackend;
use self::control::{start_server, StatusHub};
use self::mib::{Mib, CLASS_CTC_LOID_AUTH};
use self::protocol::{Request, Response, ACTION_SET};
use crate::transport::{line_is_down, PacketSocket, PACKET_OUTGOING};

const RECEIVE_BUFFER_LEN: usize = 2048;
const RESPONSE_CACHE_SIZE: usize = 32;

pub use self::config::IdentityConfig;

fn ctc_authentication_status_name(status: u8) -> &'static str {
    // Q/CT 2360-2011 defines the result codes for the CTC LOID Authentication ME.
    match status {
        0 => "not-authenticated",
        1 => "accepted",
        2 => "loid-not-found",
        3 => "password-mismatch",
        4 => "loid-conflict",
        _ => "reserved-status",
    }
}

#[derive(Clone)]
struct CachedResponse {
    // The complete baseline request is the retransmission cache key.
    request: Vec<u8>,
    response: Response,
}

fn cache_lookup(cache: &VecDeque<CachedResponse>, frame: &[u8]) -> Option<Response> {
    cache
        .iter()
        .find(|entry| entry.request == frame)
        .map(|entry| entry.response.clone())
}

fn cache_insert(cache: &mut VecDeque<CachedResponse>, frame: &[u8], response: &Response) {
    if cache.len() == RESPONSE_CACHE_SIZE {
        cache.pop_front();
    }
    cache.push_back(CachedResponse {
        request: frame.to_vec(),
        response: response.clone(),
    });
}

pub fn run_agent(
    interface: &str,
    mut identity: IdentityConfig,
    control_socket: &Path,
) -> io::Result<()> {
    let loid_configured = !identity.loid.is_empty();
    let omcc_version = identity.omcc_version;
    let disable_enhanced_security = identity.disable_enhanced_security;
    let mut backend = DataPathBackend::for_omci_interface(interface, identity.alloc_id_timeout)?;
    if let Some(serial) = backend.active_serial_number()? {
        identity.serial_number = serial.to_vec();
    }
    let mut mib = Mib::from_identity(&identity);
    let socket = PacketSocket::open(interface)?;
    let status = StatusHub::new(interface, loid_configured);
    let initial_provisioning = mib.provisioning_snapshot();
    backend.reconcile(&initial_provisioning);
    status.record_provisioning(
        initial_provisioning,
        mib.data_path_graph(),
        backend.status(),
    );
    let _control_server = start_server(control_socket, status.clone())?;
    let mut receive_buffer = [0u8; RECEIVE_BUFFER_LEN];
    let mut cache = VecDeque::<CachedResponse>::with_capacity(RESPONSE_CACHE_SIZE);

    println!(
        "OMCI agent started: interface={} control_socket={} loid_configured={} omcc_version=0x{:02x} disable_enhanced_security={}",
        interface,
        control_socket.display(),
        loid_configured,
        omcc_version,
        disable_enhanced_security
    );

    loop {
        if let Some(interval) = backend.recheck_interval() {
            match socket.wait_readable(interval) {
                Ok(true) => {}
                Ok(false) => {
                    // The kernel may have applied deferred paths since the last OMCI request.
                    let provisioning = mib.provisioning_snapshot();
                    backend.reconcile(&provisioning);
                    status.record_provisioning(
                        provisioning,
                        mib.data_path_graph(),
                        backend.status(),
                    );
                    continue;
                }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => return Err(error),
            }
        }
        let received = match socket.receive(&mut receive_buffer) {
            Ok(frame) => frame,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => {
                status.record_transport_error(&error.to_string());
                if line_is_down(&error) && socket.interface_present() {
                    // Retransmissions never span a line down; the MIB itself is retained.
                    cache.clear();
                    continue;
                }
                return Err(error);
            }
        };
        if received.packet_type == PACKET_OUTGOING {
            continue;
        }

        let request = match Request::parse(received.bytes) {
            Ok(request) => request,
            Err(error) => {
                status.record_parse_error(&error.to_string());
                continue;
            }
        };
        status.record_rx(&request);

        // CTC LOID Authentication attribute 4 carries the OLT authentication result.
        if request.action == ACTION_SET
            && request.class_id == CLASS_CTC_LOID_AUTH
            && request.entity_id == 0
            && request.attribute_mask == 0x1000
        {
            let authentication_status = request.content[0];
            let meaning = ctc_authentication_status_name(authentication_status);
            status.record_authentication_status(authentication_status, meaning);
            println!(
                "OMCI CTC LOID authentication result: status={} meaning={} loid_configured={}",
                authentication_status, meaning, loid_configured
            );
        }

        let (response, retransmission, attribute_value_changes) = match cache_lookup(
            &cache,
            received.bytes,
        ) {
            Some(response) => (response, true, Vec::new()),
            None => {
                if let Some(serial) = backend.active_serial_number()? {
                    mib.set_onu_serial(serial);
                }
                let response = mib.dispatch(&request);
                if let Some(msk) = mib.take_pending_master_session_key() {
                    let install_result = backend.install_master_session_key(&msk);
                    let installed = install_result.is_ok();
                    mib.complete_master_session_key_install(installed);
                    match install_result {
                        Ok(()) => println!("OMCI enhanced security authentication: accepted"),
                        Err(error) => println!(
                            "OMCI enhanced security authentication: kernel key install failed: {error}"
                        ),
                    }
                }
                let attribute_value_changes = mib.take_attribute_value_changes();
                let provisioning = mib.provisioning_snapshot();
                backend.reconcile(&provisioning);
                status.record_provisioning(provisioning, mib.data_path_graph(), backend.status());
                cache_insert(&mut cache, received.bytes, &response);
                (response, false, attribute_value_changes)
            }
        };

        if let Err(error) = socket.send(response.as_bytes()) {
            status.record_transport_error(&error.to_string());
            if line_is_down(&error) {
                cache.clear();
                continue;
            }
            return Err(error);
        }
        let result = response.result();
        status.record_tx(&request, result, retransmission);

        for change in attribute_value_changes {
            let notification = Response::attribute_value_change(
                0,
                change.class_id,
                change.entity_id,
                change.attribute_mask,
                &change.value,
            )
            .expect("MIB attribute value change fits a baseline OMCI PDU");
            if let Err(error) = socket.send(notification.as_bytes()) {
                status.record_transport_error(&error.to_string());
                if line_is_down(&error) {
                    break;
                }
                return Err(error);
            }
        }
    }
}
