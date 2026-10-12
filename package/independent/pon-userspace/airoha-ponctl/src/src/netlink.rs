// SPDX-License-Identifier: GPL-2.0-only

use std::io;
use std::mem::size_of;
use std::os::raw::{c_int, c_void};

const AF_NETLINK: c_int = 16;
const SOCK_RAW: c_int = 3;
const SOCK_CLOEXEC: c_int = 0o2000000;
const NETLINK_GENERIC: c_int = 16;

const NLM_F_REQUEST: u16 = 0x01;
const NLM_F_ACK: u16 = 0x04;
const NLMSG_ERROR: u16 = 0x02;
const GENL_ID_CTRL: u16 = 0x10;
const CTRL_CMD_GETFAMILY: u8 = 3;
const CTRL_ATTR_FAMILY_ID: u16 = 1;
const CTRL_ATTR_FAMILY_NAME: u16 = 2;

const XPON_FAMILY_NAME: &str = "xpon";
const XPON_VERSION: u8 = 1;
const XPON_CMD_GET_LINE: u8 = 1;
const XPON_CMD_SET_LINE: u8 = 2;
const XPON_CMD_GET_STATUS: u8 = 3;
const XPON_ATTR_IFINDEX: u16 = 1;
const XPON_ATTR_MODE: u16 = 2;
const XPON_ATTR_ACTIVE_MODE: u16 = 3;

#[repr(C)]
struct SockAddrNl {
    family: u16,
    pad: u16,
    pid: u32,
    groups: u32,
}

extern "C" {
    fn socket(domain: c_int, socket_type: c_int, protocol: c_int) -> c_int;
    fn bind(fd: c_int, address: *const c_void, length: u32) -> c_int;
    fn sendto(
        fd: c_int,
        buffer: *const c_void,
        length: usize,
        flags: c_int,
        address: *const c_void,
        address_length: u32,
    ) -> isize;
    fn recv(fd: c_int, buffer: *mut c_void, length: usize, flags: c_int) -> isize;
    fn close(fd: c_int) -> c_int;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LineMode {
    Gpon = 1,
    Xgpon = 2,
    Xgspon = 3,
    Epon1g = 4,
    Epon10g1g = 5,
    Epon10g10g = 6,
}

impl LineMode {
    pub fn parse(name: &str) -> Option<Self> {
        match name {
            "gpon" => Some(Self::Gpon),
            "xgpon" => Some(Self::Xgpon),
            "xgspon" => Some(Self::Xgspon),
            "epon-1g" => Some(Self::Epon1g),
            "epon-10g-1g" => Some(Self::Epon10g1g),
            "epon-10g-10g" => Some(Self::Epon10g10g),
            _ => None,
        }
    }

    pub fn from_id(id: u8) -> Option<Self> {
        match id {
            1 => Some(Self::Gpon),
            2 => Some(Self::Xgpon),
            3 => Some(Self::Xgspon),
            4 => Some(Self::Epon1g),
            5 => Some(Self::Epon10g1g),
            6 => Some(Self::Epon10g10g),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Gpon => "gpon",
            Self::Xgpon => "xgpon",
            Self::Xgspon => "xgspon",
            Self::Epon1g => "epon-1g",
            Self::Epon10g1g => "epon-10g-1g",
            Self::Epon10g10g => "epon-10g-10g",
        }
    }
}

pub struct LineState {
    pub configured: LineMode,
    pub active: Option<LineMode>,
}

struct NetlinkSocket(c_int);

impl Drop for NetlinkSocket {
    fn drop(&mut self) {
        unsafe {
            close(self.0);
        }
    }
}

fn align4(length: usize) -> usize {
    (length + 3) & !3
}

fn append_attr(message: &mut Vec<u8>, kind: u16, value: &[u8]) {
    let length = 4 + value.len();
    message.extend_from_slice(&(length as u16).to_ne_bytes());
    message.extend_from_slice(&kind.to_ne_bytes());
    message.extend_from_slice(value);
    message.resize(align4(message.len()), 0);
}

fn attrs(payload: &[u8]) -> io::Result<Vec<(u16, &[u8])>> {
    let mut result = Vec::new();
    let mut offset = 0;

    while offset < payload.len() {
        if payload.len() - offset < 4 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "truncated netlink attribute",
            ));
        }
        let length = u16::from_ne_bytes([payload[offset], payload[offset + 1]]) as usize;
        let kind = u16::from_ne_bytes([payload[offset + 2], payload[offset + 3]]) & 0x3fff;
        if length < 4 || offset + length > payload.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid netlink attribute length",
            ));
        }
        result.push((kind, &payload[offset + 4..offset + length]));
        offset += align4(length);
    }
    Ok(result)
}

impl NetlinkSocket {
    fn open() -> io::Result<Self> {
        let fd = unsafe { socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let socket = Self(fd);
        let local = SockAddrNl {
            family: AF_NETLINK as u16,
            pad: 0,
            pid: 0,
            groups: 0,
        };
        let ret = unsafe {
            bind(
                socket.0,
                &local as *const _ as *const c_void,
                size_of::<SockAddrNl>() as u32,
            )
        };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(socket)
    }

    fn request(
        &self,
        message_type: u16,
        command: u8,
        version: u8,
        request_ack: bool,
        attributes: &[(u16, &[u8])],
    ) -> io::Result<Vec<u8>> {
        let sequence = 1u32;
        let mut message = vec![0u8; 20];
        message[4..6].copy_from_slice(&message_type.to_ne_bytes());
        let flags = NLM_F_REQUEST | if request_ack { NLM_F_ACK } else { 0 };
        message[6..8].copy_from_slice(&flags.to_ne_bytes());
        message[8..12].copy_from_slice(&sequence.to_ne_bytes());
        message[16] = command;
        message[17] = version;
        for (kind, value) in attributes {
            append_attr(&mut message, *kind, value);
        }
        let message_len = message.len() as u32;
        message[..4].copy_from_slice(&message_len.to_ne_bytes());

        let kernel = SockAddrNl {
            family: AF_NETLINK as u16,
            pad: 0,
            pid: 0,
            groups: 0,
        };
        let sent = unsafe {
            sendto(
                self.0,
                message.as_ptr() as *const c_void,
                message.len(),
                0,
                &kernel as *const _ as *const c_void,
                size_of::<SockAddrNl>() as u32,
            )
        };
        if sent < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut buffer = vec![0u8; 8192];
        loop {
            let received =
                unsafe { recv(self.0, buffer.as_mut_ptr() as *mut c_void, buffer.len(), 0) };
            if received < 0 {
                return Err(io::Error::last_os_error());
            }
            let received = received as usize;
            let mut offset = 0;
            while offset + 16 <= received {
                let length =
                    u32::from_ne_bytes(buffer[offset..offset + 4].try_into().unwrap()) as usize;
                let kind = u16::from_ne_bytes(buffer[offset + 4..offset + 6].try_into().unwrap());
                let reply_sequence =
                    u32::from_ne_bytes(buffer[offset + 8..offset + 12].try_into().unwrap());
                if length < 16 || offset + length > received {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "invalid netlink message length",
                    ));
                }
                if reply_sequence == sequence {
                    if kind == NLMSG_ERROR {
                        if length < 20 {
                            return Err(io::Error::new(
                                io::ErrorKind::InvalidData,
                                "truncated netlink error",
                            ));
                        }
                        let error = i32::from_ne_bytes(
                            buffer[offset + 16..offset + 20].try_into().unwrap(),
                        );
                        if error == 0 {
                            return Ok(Vec::new());
                        }
                        return Err(io::Error::from_raw_os_error(-error));
                    }
                    if length < 20 {
                        return Err(io::Error::new(
                            io::ErrorKind::InvalidData,
                            "truncated generic netlink reply",
                        ));
                    }
                    return Ok(buffer[offset + 20..offset + length].to_vec());
                }
                offset += align4(length);
            }
        }
    }

    fn family_id(&self, family_name: &str) -> io::Result<u16> {
        let mut name = family_name.as_bytes().to_vec();
        name.push(0);
        let payload = self.request(
            GENL_ID_CTRL,
            CTRL_CMD_GETFAMILY,
            1,
            false,
            &[(CTRL_ATTR_FAMILY_NAME, &name)],
        )?;
        for (kind, value) in attrs(&payload)? {
            if kind == CTRL_ATTR_FAMILY_ID && value.len() == 2 {
                return Ok(u16::from_ne_bytes(value.try_into().unwrap()));
            }
        }
        Err(io::Error::new(
            io::ErrorKind::NotFound,
            "generic netlink family xpon was not found",
        ))
    }
}

pub fn get_line(ifindex: u32) -> io::Result<LineState> {
    let socket = NetlinkSocket::open()?;
    let family = socket.family_id(XPON_FAMILY_NAME)?;
    let payload = socket.request(
        family,
        XPON_CMD_GET_LINE,
        XPON_VERSION,
        false,
        &[(XPON_ATTR_IFINDEX, &ifindex.to_ne_bytes())],
    )?;
    let mut configured = None;
    let mut active = None;
    for (kind, value) in attrs(&payload)? {
        if value.len() != 1 {
            continue;
        }
        if kind == XPON_ATTR_MODE {
            configured = LineMode::from_id(value[0]);
        } else if kind == XPON_ATTR_ACTIVE_MODE {
            active = LineMode::from_id(value[0]);
        }
    }
    let configured = configured.ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            "xpon reply has no valid configured mode",
        )
    })?;
    Ok(LineState { configured, active })
}

pub fn set_line(ifindex: u32, mode: LineMode) -> io::Result<()> {
    let socket = NetlinkSocket::open()?;
    let family = socket.family_id(XPON_FAMILY_NAME)?;
    socket.request(
        family,
        XPON_CMD_SET_LINE,
        XPON_VERSION,
        true,
        &[
            (XPON_ATTR_IFINDEX, &ifindex.to_ne_bytes()),
            (XPON_ATTR_MODE, &[mode as u8]),
        ],
    )?;
    Ok(())
}

#[derive(Clone, Copy)]
enum StatusKind {
    String,
    Bool,
    U32,
    S32,
    U64,
}

const LINE_FIELDS: &[(&str, StatusKind)] = &[
    ("configured_mode", StatusKind::String),
    ("active_mode", StatusKind::String),
    ("mode_pending", StatusKind::Bool),
    ("lifecycle", StatusKind::String),
    ("last_start_error", StatusKind::S32),
    ("optical_signal", StatusKind::Bool),
    ("rx_active", StatusKind::Bool),
    ("phy_ready", StatusKind::Bool),
    ("xgtc_sync", StatusKind::String),
    ("pcs_sync", StatusKind::Bool),
    ("pcs_profile_valid", StatusKind::Bool),
    ("pcs_profile", StatusKind::String),
];

const FRONTEND_FIELDS: &[(&str, StatusKind)] = &[
    ("error", StatusKind::S32),
    ("calibration", StatusKind::String),
    ("tx_gate_enabled", StatusKind::Bool),
    ("temperature_8472", StatusKind::S32),
    ("voltage_8472", StatusKind::U32),
    ("tx_bias_8472", StatusKind::U32),
    ("tx_power_8472", StatusKind::U32),
    ("rx_power_8472", StatusKind::U32),
];

const REGISTRATION_FIELDS: &[(&str, StatusKind)] = &[
    ("onu_state", StatusKind::String),
    ("onu_id_valid", StatusKind::Bool),
    ("onu_id", StatusKind::U32),
    ("mpcp_state", StatusKind::String),
    ("llid_valid", StatusKind::Bool),
    ("llid", StatusKind::U32),
    ("upstream_tx_armed", StatusKind::Bool),
    ("serial_configured", StatusKind::Bool),
    ("registration_id_configured", StatusKind::Bool),
];

const DATAPATH_FIELDS: &[(&str, StatusKind)] = &[
    ("data_path_configured", StatusKind::Bool),
    ("service_ready", StatusKind::Bool),
];

const COUNTER_FIELDS: &[(&str, StatusKind)] = &[
    ("rx_start_count", StatusKind::U32),
    ("xgtc_rx", StatusKind::U64),
    ("upstream_bursts_tx", StatusKind::U64),
    ("ploamd_rx", StatusKind::U64),
    ("ploamu_tx", StatusKind::U64),
    ("xgem_rx", StatusKind::U64),
    ("xgem_tx", StatusKind::U64),
    ("discovery_gates", StatusKind::U32),
    ("register_requests", StatusKind::U32),
    ("register_messages", StatusKind::U32),
    ("register_acks", StatusKind::U32),
    ("register_nacks", StatusKind::U32),
    ("mpcp_timeouts", StatusKind::U32),
    ("mac_errors", StatusKind::U32),
    ("sync_losses", StatusKind::U32),
    ("recoveries", StatusKind::U32),
    ("full_reinitializations", StatusKind::U32),
];

fn status_group(kind: u16) -> Option<(&'static str, &'static [(&'static str, StatusKind)])> {
    match kind {
        4 => Some(("line", LINE_FIELDS)),
        5 => Some(("frontend", FRONTEND_FIELDS)),
        6 => Some(("registration", REGISTRATION_FIELDS)),
        7 => Some(("datapath", DATAPATH_FIELDS)),
        8 => Some(("counters", COUNTER_FIELDS)),
        _ => None,
    }
}

fn status_value(kind: StatusKind, data: &[u8]) -> io::Result<crate::status::Value> {
    use crate::status::Value;

    match kind {
        StatusKind::String if data.last() == Some(&0) => Ok(Value::Text(
            std::str::from_utf8(&data[..data.len() - 1])
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid status UTF-8"))?
                .to_owned(),
        )),
        StatusKind::Bool if data.len() == 1 && data[0] <= 1 => Ok(Value::Bool(data[0] != 0)),
        StatusKind::U32 if data.len() == 4 => Ok(Value::Integer(u32::from_ne_bytes(
            data.try_into().unwrap(),
        ) as i64)),
        StatusKind::S32 if data.len() == 4 => Ok(Value::Integer(i32::from_ne_bytes(
            data.try_into().unwrap(),
        ) as i64)),
        StatusKind::U64 if data.len() == 8 => Ok(Value::Unsigned(u64::from_ne_bytes(
            data.try_into().unwrap(),
        ))),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid status attribute type",
        )),
    }
}

pub fn get_status(ifindex: u32) -> io::Result<crate::status::Snapshot> {
    use crate::status::Snapshot;

    let socket = NetlinkSocket::open()?;
    let family = socket.family_id(XPON_FAMILY_NAME)?;
    let payload = socket.request(
        family,
        XPON_CMD_GET_STATUS,
        XPON_VERSION,
        false,
        &[(XPON_ATTR_IFINDEX, &ifindex.to_ne_bytes())],
    )?;
    let mut snapshot = Snapshot::new();

    for (group_id, group_data) in attrs(&payload)? {
        let Some((group_name, fields)) = status_group(group_id) else {
            continue;
        };
        let mut values = std::collections::BTreeMap::new();

        for (field_id, data) in attrs(group_data)? {
            let Some((name, kind)) = field_id
                .checked_sub(1)
                .and_then(|id| fields.get(id as usize))
            else {
                continue;
            };
            values.insert((*name).to_owned(), status_value(*kind, data)?);
        }
        snapshot.insert(group_name.to_owned(), values);
    }

    crate::status::convert_optics(&mut snapshot);
    Ok(snapshot)
}
