// SPDX-License-Identifier: GPL-2.0-only

pub struct Reader<'a> {
    input: &'a [u8],
    offset: usize,
}

impl<'a> Reader<'a> {
    pub fn new(input: &'a [u8]) -> Self {
        Self { input, offset: 0 }
    }

    pub fn offset(&self) -> usize {
        self.offset
    }

    pub fn remaining(&self) -> usize {
        self.input.len() - self.offset
    }

    pub fn u8(&mut self) -> Option<u8> {
        let value = *self.input.get(self.offset)?;
        self.offset += 1;
        Some(value)
    }

    pub fn be16(&mut self) -> Option<u16> {
        let bytes = self.bytes(2)?;
        Some(u16::from_be_bytes([bytes[0], bytes[1]]))
    }

    pub fn be32(&mut self) -> Option<u32> {
        let bytes = self.bytes(4)?;
        Some(u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    pub fn bytes(&mut self, length: usize) -> Option<&'a [u8]> {
        let end = self.offset + length;
        let value = self.input.get(self.offset..end)?;
        self.offset = end;
        Some(value)
    }

    pub fn rest(&self) -> &'a [u8] {
        &self.input[self.offset..]
    }
}

pub struct Writer {
    output: Vec<u8>,
}

impl Writer {
    pub fn new() -> Self {
        Self { output: Vec::new() }
    }

    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            output: Vec::with_capacity(capacity),
        }
    }

    pub fn u8(&mut self, value: u8) {
        self.output.push(value);
    }

    pub fn be16(&mut self, value: u16) {
        self.output.extend_from_slice(&value.to_be_bytes());
    }

    pub fn be32(&mut self, value: u32) {
        self.output.extend_from_slice(&value.to_be_bytes());
    }

    pub fn bytes(&mut self, value: &[u8]) {
        self.output.extend_from_slice(value);
    }

    pub fn resize(&mut self, length: usize, value: u8) {
        self.output.resize(length, value);
    }

    pub fn len(&self) -> usize {
        self.output.len()
    }

    pub fn into_vec(self) -> Vec<u8> {
        self.output
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_and_writes_network_order_values() {
        let mut writer = Writer::new();
        writer.u8(0x12);
        writer.be16(0x3456);
        writer.be32(0x789abcde);

        let encoded = writer.into_vec();
        let mut reader = Reader::new(&encoded);
        assert_eq!(reader.u8(), Some(0x12));
        assert_eq!(reader.be16(), Some(0x3456));
        assert_eq!(reader.be32(), Some(0x789abcde));
        assert_eq!(reader.remaining(), 0);
    }
}
