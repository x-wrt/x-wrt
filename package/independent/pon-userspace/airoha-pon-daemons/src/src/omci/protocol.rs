// SPDX-License-Identifier: GPL-2.0-only

//! OMCI frame decoding, response encoding, and operation result handling.
// The QDMA descriptor path appends the outbound MIC.

use std::fmt;

use crate::wire::Writer;

pub const BASELINE_LEN: usize = 44;
pub const BASELINE_WITH_MIC_LEN: usize = 48;
pub const EXTENDED_HEADER_LEN: usize = 10;
pub const EXTENDED_MAX_PDU_LEN: usize = 1980;
pub const DEVICE_ID_BASELINE: u8 = 0x0a;
pub const DEVICE_ID_EXTENDED: u8 = 0x0b;
pub const BASELINE_VALUE_CAPACITY: usize = 29;
pub const EXTENDED_GET_VALUE_CAPACITY: usize = EXTENDED_MAX_PDU_LEN - EXTENDED_HEADER_LEN - 7;
pub const EXTENDED_GET_NEXT_VALUE_CAPACITY: usize = EXTENDED_MAX_PDU_LEN - EXTENDED_HEADER_LEN - 3;

pub const RESULT_SUCCESS: u8 = 0x00;
pub const RESULT_COMMAND_NOT_SUPPORTED: u8 = 0x02;
pub const RESULT_PARAMETER_ERROR: u8 = 0x03;
pub const RESULT_UNKNOWN_ME: u8 = 0x04;
pub const RESULT_UNKNOWN_INSTANCE: u8 = 0x05;
pub const RESULT_INSTANCE_EXISTS: u8 = 0x07;
pub const RESULT_ATTRIBUTE_FAILED: u8 = 0x09;

pub const ACTION_CREATE: u8 = 0x04;
pub const ACTION_DELETE: u8 = 0x06;
pub const ACTION_SET: u8 = 0x08;
pub const ACTION_GET: u8 = 0x09;
pub const ACTION_GET_ALL_ALARMS: u8 = 0x0b;
pub const ACTION_GET_ALL_ALARMS_NEXT: u8 = 0x0c;
pub const ACTION_MIB_UPLOAD: u8 = 0x0d;
pub const ACTION_MIB_UPLOAD_NEXT: u8 = 0x0e;
pub const ACTION_MIB_RESET: u8 = 0x0f;
pub const ACTION_SYNCHRONIZE_TIME: u8 = 0x18;
pub const ACTION_GET_NEXT: u8 = 0x1a;
pub const ACTION_GET_CURRENT_DATA: u8 = 0x1c;
pub const ACTION_SET_TABLE: u8 = 0x1d;
pub const ACTION_ATTRIBUTE_VALUE_CHANGE: u8 = 0x11;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Encoding {
    Baseline,
    Extended,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Request<'a> {
    pub encoding: Encoding,
    pub tci: u16,
    pub message_type: u8,
    pub action: u8,
    pub class_id: u16,
    pub entity_id: u16,
    pub attribute_mask: u16,
    // The full message contents carry set-by-create attributes for Create requests.
    pub payload: &'a [u8],
    // The ME handlers receive content after the Set/Get mask or Get-next sequence.
    pub content: &'a [u8],
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Response {
    bytes: Vec<u8>,
    result: Option<u8>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParseError {
    TooShort(usize),
    InvalidBaselineLength(usize),
    InvalidExtendedLength { actual: usize, declared: usize },
    UnsupportedDeviceId(u8),
}

impl fmt::Display for ParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::TooShort(length) => write!(formatter, "OMCI PDU is too short: {length}"),
            Self::InvalidBaselineLength(length) => {
                write!(formatter, "invalid baseline length {length}")
            }
            Self::InvalidExtendedLength { actual, declared } => write!(
                formatter,
                "invalid extended length {actual}, message contents declare {declared}"
            ),
            Self::UnsupportedDeviceId(device) => {
                write!(formatter, "unsupported OMCI device id 0x{device:02x}")
            }
        }
    }
}

impl<'a> Request<'a> {
    pub fn parse(frame: &'a [u8]) -> Result<Self, ParseError> {
        if frame.len() < 4 {
            return Err(ParseError::TooShort(frame.len()));
        }

        match frame[3] {
            DEVICE_ID_BASELINE => Self::parse_baseline(frame),
            DEVICE_ID_EXTENDED => Self::parse_extended(frame),
            device => Err(ParseError::UnsupportedDeviceId(device)),
        }
    }

    fn parse_baseline(frame: &'a [u8]) -> Result<Self, ParseError> {
        if frame.len() != BASELINE_LEN && frame.len() != BASELINE_WITH_MIC_LEN {
            return Err(ParseError::InvalidBaselineLength(frame.len()));
        }

        Ok(Self::from_parts(Encoding::Baseline, frame, &frame[8..40]))
    }

    fn parse_extended(frame: &'a [u8]) -> Result<Self, ParseError> {
        if frame.len() < EXTENDED_HEADER_LEN {
            return Err(ParseError::TooShort(frame.len()));
        }
        let declared = u16::from_be_bytes([frame[8], frame[9]]) as usize;
        let pdu_length = EXTENDED_HEADER_LEN + declared;
        if pdu_length > EXTENDED_MAX_PDU_LEN
            || (frame.len() != pdu_length && frame.len() != pdu_length + 4)
        {
            return Err(ParseError::InvalidExtendedLength {
                actual: frame.len(),
                declared,
            });
        }

        Ok(Self::from_parts(
            Encoding::Extended,
            frame,
            &frame[EXTENDED_HEADER_LEN..pdu_length],
        ))
    }

    fn from_parts(encoding: Encoding, frame: &'a [u8], payload: &'a [u8]) -> Self {
        let attribute_mask = payload
            .get(0..2)
            .map(|bytes| u16::from_be_bytes([bytes[0], bytes[1]]))
            .unwrap_or(0);
        let content = payload.get(2..).unwrap_or(&[]);

        Self {
            encoding,
            tci: u16::from_be_bytes([frame[0], frame[1]]),
            message_type: frame[2],
            action: frame[2] & 0x1f,
            class_id: u16::from_be_bytes([frame[4], frame[5]]),
            entity_id: u16::from_be_bytes([frame[6], frame[7]]),
            attribute_mask,
            payload,
            content,
        }
    }

    pub fn selector_name(&self) -> &'static str {
        match self.action {
            ACTION_GET_ALL_ALARMS_NEXT | ACTION_MIB_UPLOAD_NEXT => "sequence",
            _ => "mask",
        }
    }

    pub fn get_value_capacity(&self) -> usize {
        match self.encoding {
            Encoding::Baseline => BASELINE_VALUE_CAPACITY,
            Encoding::Extended => EXTENDED_GET_VALUE_CAPACITY,
        }
    }

    pub fn get_next_value_capacity(&self) -> usize {
        match self.encoding {
            Encoding::Baseline => BASELINE_VALUE_CAPACITY,
            Encoding::Extended => EXTENDED_GET_NEXT_VALUE_CAPACITY,
        }
    }
}

impl Response {
    pub fn attribute_value_change(
        tci: u16,
        class_id: u16,
        entity_id: u16,
        attribute_mask: u16,
        values: &[u8],
    ) -> Result<Self, &'static str> {
        if values.len() > 30 {
            return Err("AVC values exceed baseline capacity");
        }
        let mut bytes = vec![0u8; BASELINE_LEN];
        bytes[0..2].copy_from_slice(&tci.to_be_bytes());
        bytes[2] = ACTION_ATTRIBUTE_VALUE_CHANGE;
        bytes[3] = DEVICE_ID_BASELINE;
        bytes[4..6].copy_from_slice(&class_id.to_be_bytes());
        bytes[6..8].copy_from_slice(&entity_id.to_be_bytes());
        bytes[8..10].copy_from_slice(&attribute_mask.to_be_bytes());
        bytes[10..10 + values.len()].copy_from_slice(values);
        /* Autonomous baseline messages use the same 0x0028 trailer as responses. */
        bytes[42..44].copy_from_slice(&0x0028u16.to_be_bytes());
        Ok(Self {
            bytes,
            result: None,
        })
    }

    pub fn new(request: &Request<'_>, result: u8) -> Self {
        match request.encoding {
            Encoding::Baseline => Self::baseline_result(request, result),
            Encoding::Extended => {
                let content = match request.action {
                    // Create responses reserve the two-byte attribute execution mask.
                    ACTION_CREATE => vec![result, 0, 0],
                    // Get responses carry returned, optional, and execution masks.
                    ACTION_GET | ACTION_GET_CURRENT_DATA => vec![result, 0, 0, 0, 0, 0, 0],
                    ACTION_SET if result == RESULT_ATTRIBUTE_FAILED => {
                        let mut content = vec![result, 0, 0];
                        content.extend_from_slice(&request.attribute_mask.to_be_bytes());
                        content
                    }
                    _ => vec![result],
                };
                Self::extended(request, content, Some(result))
            }
        }
    }

    fn baseline_result(request: &Request<'_>, result: u8) -> Self {
        let mut bytes = Self::baseline_header(request);
        bytes[8] = result;
        Self {
            bytes,
            result: Some(result),
        }
    }

    pub fn get_success(
        request: &Request<'_>,
        returned_mask: u16,
        values: &[u8],
    ) -> Result<Self, &'static str> {
        if values.len() > request.get_value_capacity() {
            return Err("GET values exceed response capacity");
        }

        match request.encoding {
            Encoding::Baseline => {
                let mut response = Self::baseline_result(request, RESULT_SUCCESS);
                response.bytes[9..11].copy_from_slice(&returned_mask.to_be_bytes());
                response.bytes[11..11 + values.len()].copy_from_slice(values);
                Ok(response)
            }
            Encoding::Extended => {
                let mut content = vec![RESULT_SUCCESS];
                content.extend_from_slice(&returned_mask.to_be_bytes());
                // G.988 reserves four bytes for optional-attribute and execution masks.
                content.extend_from_slice(&[0, 0, 0, 0]);
                content.extend_from_slice(values);
                Ok(Self::extended(request, content, Some(RESULT_SUCCESS)))
            }
        }
    }

    pub fn set_success(request: &Request<'_>) -> Self {
        Self::new(request, RESULT_SUCCESS)
    }

    pub fn get_next_success(
        request: &Request<'_>,
        returned_mask: u16,
        values: &[u8],
    ) -> Result<Self, &'static str> {
        if values.len() > request.get_next_value_capacity() {
            return Err("GET-next values exceed response capacity");
        }

        match request.encoding {
            Encoding::Baseline => {
                let mut response = Self::baseline_result(request, RESULT_SUCCESS);
                response.bytes[9..11].copy_from_slice(&returned_mask.to_be_bytes());
                response.bytes[11..11 + values.len()].copy_from_slice(values);
                Ok(response)
            }
            Encoding::Extended => {
                let mut content = vec![RESULT_SUCCESS];
                content.extend_from_slice(&returned_mask.to_be_bytes());
                content.extend_from_slice(values);
                Ok(Self::extended(request, content, Some(RESULT_SUCCESS)))
            }
        }
    }

    pub fn mib_upload(request: &Request<'_>, command_count: u16) -> Self {
        match request.encoding {
            Encoding::Baseline => {
                let mut response = Self::baseline_without_result(request);
                response.bytes[8..10].copy_from_slice(&command_count.to_be_bytes());
                response
            }
            Encoding::Extended => {
                Self::extended(request, command_count.to_be_bytes().to_vec(), None)
            }
        }
    }

    pub fn get_all_alarms(request: &Request<'_>, command_count: u16) -> Self {
        Self::mib_upload(request, command_count)
    }

    pub fn get_all_alarms_next_empty(request: &Request<'_>) -> Self {
        match request.encoding {
            Encoding::Baseline => Self::baseline_without_result(request),
            Encoding::Extended => Self::extended(request, Vec::new(), None),
        }
    }

    pub fn mib_upload_next(
        request: &Request<'_>,
        class_id: u16,
        entity_id: u16,
        attribute_mask: u16,
        values: &[u8],
    ) -> Result<Self, &'static str> {
        match request.encoding {
            Encoding::Baseline => {
                if values.len() > 26 {
                    return Err("MIB-upload-next values exceed baseline capacity");
                }
                let mut response = Self::baseline_without_result(request);
                response.bytes[8..10].copy_from_slice(&class_id.to_be_bytes());
                response.bytes[10..12].copy_from_slice(&entity_id.to_be_bytes());
                response.bytes[12..14].copy_from_slice(&attribute_mask.to_be_bytes());
                response.bytes[14..14 + values.len()].copy_from_slice(values);
                Ok(response)
            }
            Encoding::Extended => {
                if class_id == 0 && entity_id == 0 && attribute_mask == 0 && values.is_empty() {
                    return Ok(Self::extended(request, Vec::new(), None));
                }
                let mut content = Writer::with_capacity(8 + values.len());
                content.be16(values.len() as u16);
                content.be16(class_id);
                content.be16(entity_id);
                content.be16(attribute_mask);
                content.bytes(values);
                Ok(Self::extended(request, content.into_vec(), None))
            }
        }
    }

    pub fn synchronize_time_success(request: &Request<'_>) -> Self {
        match request.encoding {
            Encoding::Baseline => Self::baseline_result(request, RESULT_SUCCESS),
            // Result information zero confirms synchronization to the 15-minute counter boundary.
            Encoding::Extended => {
                Self::extended(request, vec![RESULT_SUCCESS, 0], Some(RESULT_SUCCESS))
            }
        }
    }

    fn baseline_without_result(request: &Request<'_>) -> Self {
        Self {
            bytes: Self::baseline_header(request),
            result: None,
        }
    }

    fn baseline_header(request: &Request<'_>) -> Vec<u8> {
        let mut bytes = vec![0u8; BASELINE_LEN];
        bytes[0..2].copy_from_slice(&request.tci.to_be_bytes());
        bytes[2] = (request.message_type & 0x1f) | 0x20;
        bytes[3] = DEVICE_ID_BASELINE;
        bytes[4..6].copy_from_slice(&request.class_id.to_be_bytes());
        bytes[6..8].copy_from_slice(&request.entity_id.to_be_bytes());
        // The baseline trailer carries the fixed message length 0x0028.
        bytes[42] = 0x00;
        bytes[43] = 0x28;
        bytes
    }

    fn extended(request: &Request<'_>, content: Vec<u8>, result: Option<u8>) -> Self {
        let mut bytes = Writer::with_capacity(EXTENDED_HEADER_LEN + content.len());
        bytes.be16(request.tci);
        bytes.u8((request.message_type & 0x1f) | 0x20);
        bytes.u8(DEVICE_ID_EXTENDED);
        bytes.be16(request.class_id);
        bytes.be16(request.entity_id);
        bytes.be16(content.len() as u16);
        bytes.bytes(&content);
        Self {
            bytes: bytes.into_vec(),
            result,
        }
    }

    pub fn result(&self) -> Option<u8> {
        self.result
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }
}

pub fn attribute_bit(index: u8) -> Option<u16> {
    if (1..=16).contains(&index) {
        Some(0x8000u16 >> (index - 1))
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn baseline_get_codec_preserves_header_and_mask() {
        let mut frame = vec![0u8; BASELINE_LEN];
        frame[0..2].copy_from_slice(&0x1234u16.to_be_bytes());
        frame[2] = ACTION_GET;
        frame[3] = DEVICE_ID_BASELINE;
        frame[4..6].copy_from_slice(&256u16.to_be_bytes());
        frame[6..8].copy_from_slice(&1u16.to_be_bytes());
        frame[8..10].copy_from_slice(&0x8000u16.to_be_bytes());

        let request = Request::parse(&frame).unwrap();
        assert_eq!(request.tci, 0x1234);
        assert_eq!(request.class_id, 256);
        assert_eq!(request.entity_id, 1);
        assert_eq!(request.attribute_mask, 0x8000);

        let response = Response::get_success(&request, 0x8000, &[1, 2, 3, 4]).unwrap();
        assert_eq!(response.as_bytes().len(), BASELINE_LEN);
        assert_eq!(&response.as_bytes()[0..2], &frame[0..2]);
        assert_eq!(response.as_bytes()[2], ACTION_GET | 0x20);
        assert_eq!(&response.as_bytes()[3..8], &frame[3..8]);
        assert_eq!(response.as_bytes()[8], RESULT_SUCCESS);
        assert_eq!(&response.as_bytes()[9..11], &0x8000u16.to_be_bytes());
    }

    #[test]
    fn extended_response_declares_exact_content_length() {
        let frame = [
            0x12,
            0x34,
            ACTION_GET,
            DEVICE_ID_EXTENDED,
            0x01,
            0x00,
            0x00,
            0x01,
            0x00,
            0x02,
            0x80,
            0x00,
        ];
        let request = Request::parse(&frame).unwrap();
        let response = Response::get_success(&request, 0x8000, &[0xaa, 0xbb]).unwrap();
        let bytes = response.as_bytes();

        assert_eq!(
            u16::from_be_bytes([bytes[8], bytes[9]]) as usize,
            bytes.len() - 10
        );
        assert_eq!(
            &bytes[10..],
            &[RESULT_SUCCESS, 0x80, 0x00, 0, 0, 0, 0, 0xaa, 0xbb]
        );
    }
}
