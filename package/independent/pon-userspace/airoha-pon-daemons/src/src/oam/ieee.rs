// SPDX-License-Identifier: GPL-2.0-only

use super::config::OamConfig;
use super::ctc::CtcSession;

const INFO_END: u8 = 0x00;
const INFO_LOCAL: u8 = 0x01;
const INFO_REMOTE: u8 = 0x02;
const INFO_ORGANIZATION_SPECIFIC: u8 = 0xfe;
const INFO_LENGTH: usize = 16;

pub struct IeeeSession {
    revision: u16,
    pub information_rx: u64,
    pub event_rx: u64,
}

impl IeeeSession {
    pub fn new() -> Self {
        Self {
            revision: 1,
            information_rx: 0,
            event_rx: 0,
        }
    }

    pub fn handle_information(
        &mut self,
        payload: &[u8],
        source_mac: [u8; 6],
        config: &OamConfig,
        ctc: &mut CtcSession,
    ) -> Result<Vec<u8>, &'static str> {
        self.information_rx += 1;
        let mut offset = 0;
        let mut remote_information = None;
        let mut ctc_response = None;

        while offset < payload.len() {
            let info_type = payload[offset];
            if info_type == INFO_END {
                break;
            }
            if offset + 2 > payload.len() {
                return Err("information TLV header is truncated");
            }
            let length = payload[offset + 1] as usize;
            if length < 2 || offset + length > payload.len() {
                return Err("information TLV has an invalid length");
            }
            let tlv = &payload[offset..offset + length];

            match info_type {
                INFO_LOCAL if length == INFO_LENGTH => remote_information = Some(tlv.to_vec()),
                INFO_ORGANIZATION_SPECIFIC if config.ctc_enabled() => {
                    ctc_response = ctc.handle_information_tlv(tlv, config)?;
                }
                _ => {}
            }
            offset += length;
        }

        let remote_information =
            remote_information.ok_or("information PDU does not contain Local Information")?;
        let mut response = local_information(self.revision, source_mac);
        response.push(INFO_REMOTE);
        response.extend_from_slice(&remote_information[1..]);
        if let Some(ctc_response) = ctc_response {
            response.extend_from_slice(&ctc_response);
        }
        response.push(INFO_END);
        Ok(response)
    }
}

fn local_information(revision: u16, source_mac: [u8; 6]) -> Vec<u8> {
    let mut information = Vec::with_capacity(INFO_LENGTH);
    information.push(INFO_LOCAL);
    information.push(INFO_LENGTH as u8);
    information.push(1);
    information.extend_from_slice(&revision.to_be_bytes());
    information.push(0);
    // Passive mode advertises Link Event support.
    information.push(0x08);
    information.extend_from_slice(&1500u16.to_be_bytes());
    information.extend_from_slice(&source_mac[..3]);
    information.extend_from_slice(&[0; 4]);
    information
}
