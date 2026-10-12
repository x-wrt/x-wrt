// SPDX-License-Identifier: GPL-2.0-only

use std::collections::BTreeSet;

use super::config::{fixed_width, fixed_width_right, OamConfig};
use crate::wire::{Reader, Writer};

const INFO_TYPE_ORGANIZATION_SPECIFIC: u8 = 0xfe;

const OPCODE_GET_REQUEST: u8 = 0x01;
const OPCODE_GET_RESPONSE: u8 = 0x02;
const OPCODE_SET_REQUEST: u8 = 0x03;
const OPCODE_SET_RESPONSE: u8 = 0x04;
const OPCODE_AUTHENTICATION: u8 = 0x05;

const AUTH_REQUEST: u8 = 0x01;
const AUTH_RESPONSE: u8 = 0x02;
const AUTH_SUCCESS: u8 = 0x03;
const AUTH_FAILURE: u8 = 0x04;
const AUTH_TYPE_LOID_PASSWORD: u8 = 0x01;
const AUTH_TYPE_NAK: u8 = 0x02;

const OBJECT_BRANCH_V1: u8 = 0x36;
const OBJECT_BRANCH_V2: u8 = 0x37;
const OBJECT_LEAF_PORT: u16 = 0x0001;
const STANDARD_ATTRIBUTE: u8 = 0x07;
const STANDARD_ACTION: u8 = 0x09;
const EXTENDED_ATTRIBUTE: u8 = 0xc7;
const EXTENDED_ACTION: u8 = 0xc9;

const LEAF_ONU_SN: u16 = 0x0001;
const LEAF_FIRMWARE_VERSION: u16 = 0x0002;
const LEAF_CHIPSET_ID: u16 = 0x0003;
const LEAF_ONU_CAPABILITIES_1: u16 = 0x0004;
const LEAF_ONU_CAPABILITIES_2: u16 = 0x0007;
const LEAF_ONU_CAPABILITIES_3: u16 = 0x000c;
const LEAF_VLAN: u16 = 0x0021;

const SET_OK: u8 = 0x80;
const BAD_PARAMETERS: u8 = 0x86;
const NO_RESOURCE: u8 = 0x87;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum DiscoveryState {
    PassiveWait,
    Offered,
    Operational,
}

#[derive(Clone, Copy, Debug)]
struct Object {
    branch: u8,
    leaf: u16,
    index: u32,
    encoded_len: usize,
}

#[derive(Clone, Debug)]
struct VlanConfiguration {
    object_index: u32,
    mode: u8,
    raw: Vec<u8>,
}

#[derive(Clone, Debug)]
pub struct Snapshot {
    pub discovery_state: &'static str,
    pub selected_version: Option<u8>,
    pub authentication: &'static str,
    pub authentication_failure: Option<u8>,
    pub get_requests: u64,
    pub set_requests: u64,
    pub unsupported_requests: u64,
    pub vlan_object: Option<u32>,
    pub vlan_mode: Option<u8>,
    pub vlan_ids: Vec<u16>,
}

#[derive(Debug)]
pub struct OrgOutcome {
    pub response: Option<Vec<u8>>,
    pub events: Vec<String>,
}

struct VariableOutcome {
    response: Vec<u8>,
    events: Vec<String>,
}

pub struct CtcSession {
    state: DiscoveryState,
    selected_version: Option<u8>,
    authentication: &'static str,
    authentication_failure: Option<u8>,
    get_requests: u64,
    set_requests: u64,
    unsupported_requests: u64,
    vlan: Option<VlanConfiguration>,
}

impl CtcSession {
    pub fn new() -> Self {
        Self {
            state: DiscoveryState::PassiveWait,
            selected_version: None,
            authentication: "not-requested",
            authentication_failure: None,
            get_requests: 0,
            set_requests: 0,
            unsupported_requests: 0,
            vlan: None,
        }
    }

    /// CTC discovery uses an organization-specific Information TLV whose length includes its header.
    pub fn handle_information_tlv(
        &mut self,
        tlv: &[u8],
        config: &OamConfig,
    ) -> Result<Option<Vec<u8>>, &'static str> {
        if tlv.len() < 7 || tlv[0] != INFO_TYPE_ORGANIZATION_SPECIFIC {
            return Err("CTC information TLV is truncated");
        }
        let length = tlv[1] as usize;
        if length < 7 || length > tlv.len() || (length - 7) % 4 != 0 {
            return Err("CTC information TLV has an invalid length");
        }

        let has_version_list = length > 7;
        if has_version_list {
            self.state = DiscoveryState::Offered;
            self.selected_version = None;
            let mut response = Writer::with_capacity(7 + config.ctc_versions.len() * 4);
            response.u8(INFO_TYPE_ORGANIZATION_SPECIFIC);
            response.u8((7 + config.ctc_versions.len() * 4) as u8);
            response.bytes(&config.ctc_oui);
            response.u8(1);
            response.u8(0);
            for version in &config.ctc_versions {
                response.bytes(&config.ctc_oui);
                response.u8(*version);
            }
            return Ok(Some(response.into_vec()));
        }

        if self.state == DiscoveryState::Offered
            && tlv[5] == 1
            && config.ctc_versions.contains(&tlv[6])
        {
            self.state = DiscoveryState::Operational;
            self.selected_version = Some(tlv[6]);
            return Ok(Some(vec![
                INFO_TYPE_ORGANIZATION_SPECIFIC,
                7,
                config.ctc_oui[0],
                config.ctc_oui[1],
                config.ctc_oui[2],
                1,
                tlv[6],
            ]));
        }

        if self.state == DiscoveryState::Operational && Some(tlv[6]) == self.selected_version {
            return Ok(Some(vec![
                INFO_TYPE_ORGANIZATION_SPECIFIC,
                7,
                config.ctc_oui[0],
                config.ctc_oui[1],
                config.ctc_oui[2],
                1,
                tlv[6],
            ]));
        }

        self.state = DiscoveryState::PassiveWait;
        self.selected_version = None;
        Ok(Some(vec![
            INFO_TYPE_ORGANIZATION_SPECIFIC,
            7,
            config.ctc_oui[0],
            config.ctc_oui[1],
            config.ctc_oui[2],
            0,
            tlv[6],
        ]))
    }

    pub fn handle_organization_pdu(
        &mut self,
        payload: &[u8],
        config: &OamConfig,
        source_mac: [u8; 6],
    ) -> Result<OrgOutcome, &'static str> {
        if payload.len() < 4 {
            return Err("CTC organization PDU is truncated");
        }
        if payload[..3] != config.ctc_oui {
            return Err("CTC organization PDU has an unexpected OUI");
        }
        if self.state != DiscoveryState::Operational {
            return Err("CTC organization PDU arrived before version negotiation");
        }

        match payload[3] {
            OPCODE_AUTHENTICATION => self.handle_authentication(&payload[4..], config),
            OPCODE_GET_REQUEST => {
                self.get_requests += 1;
                let outcome = self.handle_get(&payload[4..], config, source_mac)?;
                Ok(OrgOutcome {
                    response: Some(with_org_header(
                        config.ctc_oui,
                        OPCODE_GET_RESPONSE,
                        outcome.response,
                    )),
                    events: outcome.events,
                })
            }
            OPCODE_SET_REQUEST => {
                self.set_requests += 1;
                let outcome = self.handle_set(&payload[4..])?;
                Ok(OrgOutcome {
                    response: Some(with_org_header(
                        config.ctc_oui,
                        OPCODE_SET_RESPONSE,
                        outcome.response,
                    )),
                    events: outcome.events,
                })
            }
            opcode => {
                self.unsupported_requests += 1;
                Ok(OrgOutcome {
                    response: None,
                    events: vec![format!("CTC opcode=0x{opcode:02x} result=unsupported")],
                })
            }
        }
    }

    pub fn snapshot(&self) -> Snapshot {
        Snapshot {
            discovery_state: match self.state {
                DiscoveryState::PassiveWait => "passive-wait",
                DiscoveryState::Offered => "version-offered",
                DiscoveryState::Operational => "operational",
            },
            selected_version: self.selected_version,
            authentication: self.authentication,
            authentication_failure: self.authentication_failure,
            get_requests: self.get_requests,
            set_requests: self.set_requests,
            unsupported_requests: self.unsupported_requests,
            vlan_object: self
                .vlan
                .as_ref()
                .map(|configuration| configuration.object_index),
            vlan_mode: self.vlan.as_ref().map(|configuration| configuration.mode),
            vlan_ids: self.vlan.as_ref().map(vlan_ids).unwrap_or_default(),
        }
    }

    fn handle_authentication(
        &mut self,
        data: &[u8],
        config: &OamConfig,
    ) -> Result<OrgOutcome, &'static str> {
        if data.len() < 3 {
            return Err("CTC authentication message is truncated");
        }
        let code = data[0];
        let length = u16::from_be_bytes([data[1], data[2]]);

        match code {
            AUTH_REQUEST if length == 1 || length == 0x0101 => {
                let auth_type = if length == 0x0101 {
                    AUTH_TYPE_LOID_PASSWORD
                } else {
                    *data.get(3).ok_or("CTC authentication type is missing")?
                };
                self.authentication = "pending";
                self.authentication_failure = None;

                let mut response = Writer::new();
                response.u8(AUTH_RESPONSE);
                if auth_type == AUTH_TYPE_LOID_PASSWORD {
                    response.be16(0x0025);
                    response.u8(AUTH_TYPE_LOID_PASSWORD);
                    // CTC stores the LOID right-aligned in a 24-byte field.
                    response.bytes(&fixed_width_right(&config.loid, 24));
                    response.bytes(&fixed_width(&config.loid_password, 12));
                } else {
                    response.be16(2);
                    response.u8(AUTH_TYPE_NAK);
                    response.u8(AUTH_TYPE_LOID_PASSWORD);
                }
                Ok(OrgOutcome {
                    response: Some(with_org_header(
                        config.ctc_oui,
                        OPCODE_AUTHENTICATION,
                        response.into_vec(),
                    )),
                    events: vec![format!("CTC authentication request type={auth_type}")],
                })
            }
            AUTH_SUCCESS => {
                self.authentication = "accepted";
                self.authentication_failure = None;
                Ok(OrgOutcome {
                    response: None,
                    events: vec!["CTC authentication accepted".to_owned()],
                })
            }
            AUTH_FAILURE => {
                self.authentication = "rejected";
                self.authentication_failure = data.get(3).copied();
                Ok(OrgOutcome {
                    response: None,
                    events: vec![format!(
                        "CTC authentication rejected reason={}",
                        self.authentication_failure
                            .map(|value| format!("0x{value:02x}"))
                            .unwrap_or_else(|| "missing".to_owned())
                    )],
                })
            }
            _ => Err("CTC authentication message has an invalid code or length"),
        }
    }

    fn handle_get(
        &mut self,
        data: &[u8],
        config: &OamConfig,
        source_mac: [u8; 6],
    ) -> Result<VariableOutcome, &'static str> {
        let mut offset = 0;
        let mut object = None;
        let mut object_written = false;
        let mut response = Writer::new();
        let mut events = Vec::new();

        while offset < data.len() {
            let branch = data[offset];
            if branch == 0 {
                break;
            }
            if branch == OBJECT_BRANCH_V1 || branch == OBJECT_BRANCH_V2 {
                let parsed = parse_object(&data[offset..])?;
                offset += parsed.encoded_len;
                object = Some(parsed);
                object_written = false;
                continue;
            }
            if !is_attribute_or_action(branch) || offset + 3 > data.len() {
                return Err("CTC GET request contains an invalid branch");
            }

            let leaf = u16::from_be_bytes([data[offset + 1], data[offset + 2]]);
            let value = self.get_value(branch, leaf, object, config, source_mac);
            if object.is_some() && !object_written {
                encode_object(&mut response, object.expect("object was checked"));
                object_written = true;
            }
            match value {
                Ok(value) => {
                    events.push(format_variable_event(
                        "GET",
                        object,
                        branch,
                        leaf,
                        value.len(),
                        SET_OK,
                        None,
                    ));
                    encode_data_tlv(&mut response, branch, leaf, &value);
                }
                Err(code) => {
                    events.push(format_variable_event(
                        "GET", object, branch, leaf, 0, code, None,
                    ));
                    encode_result_tlv(&mut response, branch, leaf, code);
                }
            }
            offset += 3;
        }
        response.u8(0);
        Ok(VariableOutcome {
            response: response.into_vec(),
            events,
        })
    }

    fn handle_set(&mut self, data: &[u8]) -> Result<VariableOutcome, &'static str> {
        let mut offset = 0;
        let mut object = None;
        let mut object_written = false;
        let mut response = Writer::new();
        let mut events = Vec::new();

        while offset < data.len() {
            let branch = data[offset];
            if branch == 0 {
                break;
            }
            if branch == OBJECT_BRANCH_V1 || branch == OBJECT_BRANCH_V2 {
                let parsed = parse_object(&data[offset..])?;
                offset += parsed.encoded_len;
                object = Some(parsed);
                object_written = false;
                continue;
            }
            if !is_attribute_or_action(branch) || offset + 3 > data.len() {
                return Err("CTC SET request contains an invalid branch");
            }

            let leaf = u16::from_be_bytes([data[offset + 1], data[offset + 2]]);
            let (value, consumed) = if branch == EXTENDED_ACTION && leaf == 0x0001 {
                (&[][..], 3)
            } else {
                if offset + 4 > data.len() {
                    return Err("CTC SET value length is missing");
                }
                let width = if data[offset + 3] == 0 {
                    128
                } else {
                    data[offset + 3] as usize
                };
                if offset + 4 + width > data.len() {
                    return Err("CTC SET value is truncated");
                }
                (&data[offset + 4..offset + 4 + width], 4 + width)
            };

            let result = self.set_value(branch, leaf, object, value);
            events.push(format_variable_event(
                "SET",
                object,
                branch,
                leaf,
                value.len(),
                result,
                Some(value),
            ));
            if object.is_some() && !object_written {
                encode_object(&mut response, object.expect("object was checked"));
                object_written = true;
            }
            encode_result_tlv(&mut response, branch, leaf, result);
            offset += consumed;
        }
        response.u8(0);
        Ok(VariableOutcome {
            response: response.into_vec(),
            events,
        })
    }

    fn get_value(
        &self,
        branch: u8,
        leaf: u16,
        object: Option<Object>,
        config: &OamConfig,
        source_mac: [u8; 6],
    ) -> Result<Vec<u8>, u8> {
        if branch != EXTENDED_ATTRIBUTE {
            return Err(BAD_PARAMETERS);
        }

        match leaf {
            LEAF_ONU_SN if object.is_none() => {
                let mut value = fixed_width(&config.vendor_id, 4);
                value.extend_from_slice(&fixed_width(&config.model, 4));
                value.extend_from_slice(&source_mac);
                value.extend_from_slice(&fixed_width(&config.hardware_version, 8));
                value.extend_from_slice(&fixed_width(&config.software_version, 16));
                if self.selected_version.unwrap_or(0) >= 0x30 {
                    value.extend_from_slice(&fixed_width(&config.equipment_id, 16));
                }
                Ok(value)
            }
            LEAF_FIRMWARE_VERSION if object.is_none() => {
                Ok(config.firmware_version.iter().copied().take(127).collect())
            }
            LEAF_CHIPSET_ID if object.is_none() => Ok(config.chipset_id.to_vec()),
            LEAF_ONU_CAPABILITIES_1 if object.is_none() => {
                let mut value = vec![0; 26];
                value[0] = 0x01;
                value[1] = config.ge_ports;
                for port in 0..config.ge_ports.min(64) {
                    value[2 + 7 - (port as usize / 8)] |= 1 << (port % 8);
                }
                value[21] = 8;
                value[22] = 8;
                value[23] = 8;
                value[24] = 8;
                Ok(value)
            }
            LEAF_ONU_CAPABILITIES_2 if object.is_none() => {
                // HGU profile with one LLID, one PON port, and one GE UNI class.
                let mut value = Vec::with_capacity(15);
                value.extend_from_slice(&1u32.to_be_bytes());
                value.extend_from_slice(&[0, 0, 1, 0, 1]);
                value.extend_from_slice(&0u32.to_be_bytes());
                value.extend_from_slice(&(config.ge_ports as u16).to_be_bytes());
                Ok(value)
            }
            LEAF_ONU_CAPABILITIES_3 if object.is_none() => Ok(vec![0, 0, 1]),
            LEAF_VLAN => {
                let object = object.ok_or(BAD_PARAMETERS)?;
                if object.leaf != OBJECT_LEAF_PORT {
                    return Err(BAD_PARAMETERS);
                }
                // The HGU reports transparent mode and publishes received Set rules for netifd.
                Ok(vec![0])
            }
            _ => Err(if leaf == 0x0005 {
                NO_RESOURCE
            } else {
                BAD_PARAMETERS
            }),
        }
    }

    fn set_value(&mut self, branch: u8, leaf: u16, object: Option<Object>, value: &[u8]) -> u8 {
        if branch != EXTENDED_ATTRIBUTE || leaf != LEAF_VLAN {
            self.unsupported_requests += 1;
            return BAD_PARAMETERS;
        }
        let object = match object {
            Some(object) if object.leaf == OBJECT_LEAF_PORT => object,
            _ => return BAD_PARAMETERS,
        };
        let mode = match value.first().copied() {
            Some(mode @ 0..=4) => mode,
            _ => return BAD_PARAMETERS,
        };

        // The HGU data path preserves OLT-provisioned tags for Linux VLAN and netifd policy.
        self.vlan = Some(VlanConfiguration {
            object_index: object.index,
            mode,
            raw: value.to_vec(),
        });
        SET_OK
    }
}

fn with_org_header(oui: [u8; 3], opcode: u8, data: Vec<u8>) -> Vec<u8> {
    let mut payload = Writer::with_capacity(4 + data.len());
    payload.bytes(&oui);
    payload.u8(opcode);
    payload.bytes(&data);
    payload.into_vec()
}

fn parse_object(data: &[u8]) -> Result<Object, &'static str> {
    let mut reader = Reader::new(data);
    let branch = reader.u8().ok_or("CTC object TLV is truncated")?;
    let leaf = reader.be16().ok_or("CTC object TLV is truncated")?;
    match (branch, reader.u8()) {
        (OBJECT_BRANCH_V1, Some(1)) => Ok(Object {
            branch,
            leaf,
            index: reader.u8().ok_or("CTC object TLV is truncated")? as u32,
            encoded_len: reader.offset(),
        }),
        (OBJECT_BRANCH_V2, Some(4)) => Ok(Object {
            branch,
            leaf,
            index: reader.be32().ok_or("CTC object TLV is truncated")?,
            encoded_len: reader.offset(),
        }),
        _ => Err("CTC object TLV has an invalid width or length"),
    }
}

fn encode_object(output: &mut Writer, object: Object) {
    output.u8(object.branch);
    output.be16(object.leaf);
    if object.branch == OBJECT_BRANCH_V1 {
        output.u8(1);
        output.u8(object.index as u8);
    } else {
        output.u8(4);
        output.be32(object.index);
    }
}

fn encode_data_tlv(output: &mut Writer, branch: u8, leaf: u16, value: &[u8]) {
    for chunk in value.chunks(128) {
        output.u8(branch);
        output.be16(leaf);
        output.u8(if chunk.len() == 128 {
            0
        } else {
            chunk.len() as u8
        });
        output.bytes(chunk);
    }
}

fn encode_result_tlv(output: &mut Writer, branch: u8, leaf: u16, result: u8) {
    output.u8(branch);
    output.be16(leaf);
    output.u8(result);
}

fn format_variable_event(
    operation: &str,
    object: Option<Object>,
    branch: u8,
    leaf: u16,
    value_length: usize,
    result: u8,
    value: Option<&[u8]>,
) -> String {
    /*
     * Branch, leaf, and object instance select the CTC node handler. The bounded
     * event ring retains Set payloads for node implementation and payload decoding.
     */
    let object = object
        .map(|object| {
            format!(
                "0x{:02x}:0x{:04x}:0x{:08x}",
                object.branch, object.leaf, object.index
            )
        })
        .unwrap_or_else(|| "global".to_owned());
    let value = value.map(hex_bytes).unwrap_or_default();

    format!(
        "CTC {operation} object={object} branch=0x{branch:02x} leaf=0x{leaf:04x} \
         length={value_length} result=0x{result:02x} value={value}"
    )
}

fn hex_bytes(value: &[u8]) -> String {
    use std::fmt::Write as _;

    let mut output = String::with_capacity(value.len() * 2);
    for byte in value {
        let _ = write!(output, "{byte:02x}");
    }
    output
}

fn is_attribute_or_action(branch: u8) -> bool {
    matches!(
        branch,
        STANDARD_ATTRIBUTE | STANDARD_ACTION | EXTENDED_ATTRIBUTE | EXTENDED_ACTION
    )
}

fn vlan_ids(configuration: &VlanConfiguration) -> Vec<u16> {
    let mut ids = BTreeSet::new();
    let data = &configuration.raw;
    if data.len() < 5 || configuration.mode == 0 {
        return Vec::new();
    }

    for tag in data[1..].chunks_exact(4) {
        let value = u32::from_be_bytes([tag[0], tag[1], tag[2], tag[3]]);
        ids.insert((value & 0x0fff) as u16);
    }
    ids.into_iter().collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vlan_set_records_object_leaf_result_and_value() {
        let mut session = CtcSession::new();
        let request = [
            OBJECT_BRANCH_V1,
            0x00,
            0x01,
            0x01,
            0x01,
            EXTENDED_ATTRIBUTE,
            0x00,
            0x21,
            0x05,
            0x01,
            0x00,
            0x00,
            0x00,
            0x64,
            0x00,
        ];

        let outcome = session.handle_set(&request).expect("valid VLAN SET");

        assert_eq!(outcome.events.len(), 1);
        assert_eq!(
            outcome.events[0],
            "CTC SET object=0x36:0x0001:0x00000001 branch=0xc7 leaf=0x0021 \
             length=5 result=0x80 value=0100000064"
        );
        assert_eq!(session.snapshot().vlan_ids, vec![100]);
    }

    #[test]
    fn unsupported_set_records_the_exact_leaf_and_result() {
        let mut session = CtcSession::new();
        let request = [EXTENDED_ATTRIBUTE, 0x00, 0x22, 0x01, 0x00, 0x00];

        let outcome = session.handle_set(&request).expect("well-formed SET");

        assert_eq!(outcome.events.len(), 1);
        assert_eq!(
            outcome.events[0],
            "CTC SET object=global branch=0xc7 leaf=0x0022 length=1 result=0x86 value=00"
        );
        assert_eq!(session.unsupported_requests, 1);
    }
}
