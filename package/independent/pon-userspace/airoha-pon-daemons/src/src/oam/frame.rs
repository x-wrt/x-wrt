// SPDX-License-Identifier: GPL-2.0-only

//! IEEE 802.3ah OAM Ethernet frame codec.

use crate::wire::{Reader, Writer};

pub const CODE_INFORMATION: u8 = 0x00;
pub const CODE_EVENT_NOTIFICATION: u8 = 0x01;
pub const CODE_VARIABLE_REQUEST: u8 = 0x02;
pub const CODE_LOOPBACK_CONTROL: u8 = 0x04;
pub const CODE_ORGANIZATION_SPECIFIC: u8 = 0xfe;

pub const FLAGS_STABLE: u16 = 0x0050;
pub const SLOW_PROTOCOLS_MULTICAST: [u8; 6] = [0x01, 0x80, 0xc2, 0x00, 0x00, 0x02];

const ETHER_TYPE_SLOW_PROTOCOLS: u16 = 0x8809;
const OAM_SUBTYPE: u8 = 0x03;
const OAM_HEADER_LEN: usize = 18;
const MIN_ETHERNET_FRAME_LEN: usize = 60;

#[derive(Debug)]
pub struct OamPdu<'a> {
    pub code: u8,
    pub payload: &'a [u8],
}

impl<'a> OamPdu<'a> {
    pub fn parse(frame: &'a [u8]) -> Result<Self, &'static str> {
        if frame.len() < OAM_HEADER_LEN {
            return Err("OAMPDU is shorter than its header");
        }
        let mut reader = Reader::new(frame);
        reader.bytes(12).expect("OAM header length was checked");
        if reader.be16() != Some(ETHER_TYPE_SLOW_PROTOCOLS) {
            return Err("OAMPDU has an unexpected EtherType");
        }
        if reader.u8() != Some(OAM_SUBTYPE) {
            return Err("slow-protocol frame is not an OAMPDU");
        }
        reader.be16().expect("OAM header length was checked");
        let code = reader.u8().expect("OAM header length was checked");

        Ok(Self {
            code,
            payload: reader.rest(),
        })
    }
}

pub fn build(source: [u8; 6], flags: u16, code: u8, payload: &[u8]) -> Vec<u8> {
    let mut frame = Writer::with_capacity(OAM_HEADER_LEN + payload.len());
    frame.bytes(&SLOW_PROTOCOLS_MULTICAST);
    frame.bytes(&source);
    frame.be16(ETHER_TYPE_SLOW_PROTOCOLS);
    frame.u8(OAM_SUBTYPE);
    frame.be16(flags);
    frame.u8(code);
    frame.bytes(payload);
    frame.resize(frame.len().max(MIN_ETHERNET_FRAME_LEN), 0);
    frame.into_vec()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_codec_preserves_code_and_payload() {
        let payload = [0xfe, 0x11, 0x11, 0x11, 0x01];
        let frame = build(
            [0x02, 0, 0, 0, 0, 1],
            FLAGS_STABLE,
            CODE_ORGANIZATION_SPECIFIC,
            &payload,
        );
        let pdu = OamPdu::parse(&frame).unwrap();

        assert_eq!(pdu.code, CODE_ORGANIZATION_SPECIFIC);
        assert_eq!(&pdu.payload[..payload.len()], payload);
        assert_eq!(frame.len(), MIN_ETHERNET_FRAME_LEN);
    }
}
