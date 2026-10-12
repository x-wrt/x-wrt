// SPDX-License-Identifier: GPL-2.0-only

use crate::config::Section;

#[derive(Clone, Debug)]
pub struct IdentityConfig {
    pub omcc_version: u8,
    pub disable_enhanced_security: bool,
    /// Seconds a data path may wait for PLOAM Alloc-IDs before it is reported as failed; 0 never fails.
    pub alloc_id_timeout: u32,
    pub vendor_id: Vec<u8>,
    pub equipment_id: Vec<u8>,
    pub hardware_version: Vec<u8>,
    pub software_version: Vec<u8>,
    pub loid: Vec<u8>,
    pub loid_password: Vec<u8>,
    pub operator_id: Vec<u8>,
    pub serial_number: Vec<u8>,
}

impl Default for IdentityConfig {
    fn default() -> Self {
        Self {
            omcc_version: 0xb0,
            disable_enhanced_security: false,
            alloc_id_timeout: 30,
            vendor_id: b"OWRT".to_vec(),
            equipment_id: b"AN7581-XG-PON-ONU".to_vec(),
            hardware_version: b"AN7581".to_vec(),
            software_version: b"OpenWrt".to_vec(),
            loid: Vec::new(),
            loid_password: Vec::new(),
            operator_id: b"CTC".to_vec(),
            serial_number: Vec::new(),
        }
    }
}

impl IdentityConfig {
    pub fn from_section(section: &Section) -> Self {
        let mut config = Self::default();
        config.omcc_version = match section.option("omcc_version") {
            Some("0x86") => 0x86,
            _ => 0xb0,
        };
        config.disable_enhanced_security = section.option("disable_enhanced_security") == Some("1");
        if let Some(value) = section
            .option("alloc_id_timeout")
            .filter(|value| !value.is_empty())
        {
            match value.parse() {
                Ok(seconds) => config.alloc_id_timeout = seconds,
                Err(_) => println!(
                    "Invalid OMCI alloc_id_timeout {value:?}; using {} s",
                    config.alloc_id_timeout
                ),
            }
        }

        for (name, destination) in [
            ("vendor_id", &mut config.vendor_id),
            ("equipment_id", &mut config.equipment_id),
            ("hardware_version", &mut config.hardware_version),
            ("software_version", &mut config.software_version),
            ("loid", &mut config.loid),
            ("loid_password", &mut config.loid_password),
            ("operator_id", &mut config.operator_id),
            ("serial_number", &mut config.serial_number),
        ] {
            if let Some(value) = section.option(name).filter(|value| !value.is_empty()) {
                *destination = value.as_bytes().to_vec();
            }
        }

        config
    }

    pub fn onu_g_serial(&self) -> Vec<u8> {
        let fallback = || {
            let mut value = fixed_width(&self.vendor_id, 4);
            value.extend_from_slice(&[0; 4]);
            value
        };

        if self.serial_number.is_empty() {
            return fallback();
        }

        if self.serial_number.len() == 8 {
            return self.serial_number.clone();
        }

        let text = match std::str::from_utf8(&self.serial_number) {
            Ok(text) => text.trim(),
            Err(_) => return fallback(),
        };
        let raw_hex = text.strip_prefix("hex:").unwrap_or(text);
        if raw_hex.len() == 16 {
            if let Ok(value) = decode_hex(raw_hex) {
                return value;
            }
        }
        if text.len() == 12 && text.is_ascii() {
            if let Ok(tail) = decode_hex(&text[4..]) {
                let mut value = text.as_bytes()[..4].to_vec();
                value.extend_from_slice(&tail);
                return value;
            }
        }
        println!("Invalid OMCI serial number; using the vendor ID with a zero VSSN");
        fallback()
    }
}

pub fn fixed_width(value: &[u8], width: usize) -> Vec<u8> {
    let mut output = vec![0; width];
    let copied = value.len().min(width);
    output[..copied].copy_from_slice(&value[..copied]);
    output
}

pub fn decode_hex(input: &str) -> Result<Vec<u8>, &'static str> {
    let bytes = input.as_bytes();
    if bytes.len() % 2 != 0 {
        return Err("odd digit count");
    }

    let mut output = Vec::with_capacity(bytes.len() / 2);
    for pair in bytes.chunks_exact(2) {
        let high = hex_nibble(pair[0]).ok_or("non-hex digit")?;
        let low = hex_nibble(pair[1]).ok_or("non-hex digit")?;
        output.push((high << 4) | low);
    }
    Ok(output)
}

fn hex_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::PonConfig;

    #[test]
    fn empty_uci_values_keep_protocol_defaults() {
        let config = PonConfig::parse(
            "config omci 'line0_omci'\n\
             \toption line 'line0'\n\
             \toption vendor_id ''\n\
             \toption loid ''\n",
        )
        .unwrap();
        let identity =
            IdentityConfig::from_section(config.linked_section("omci", "line0").unwrap());

        assert_eq!(identity.vendor_id, b"OWRT");
        assert!(identity.loid.is_empty());
    }

    #[test]
    fn compatibility_options_select_the_omcc_profile() {
        let config = PonConfig::parse(
            "config omci 'line0_omci'\n\
             \toption line 'line0'\n\
             \toption omcc_version '0x86'\n\
             \toption disable_enhanced_security '1'\n\
             \toption alloc_id_timeout '0'\n",
        )
        .unwrap();
        let identity =
            IdentityConfig::from_section(config.linked_section("omci", "line0").unwrap());

        assert_eq!(identity.omcc_version, 0x86);
        assert!(identity.disable_enhanced_security);
        assert_eq!(identity.alloc_id_timeout, 0);
        assert_eq!(IdentityConfig::default().omcc_version, 0xb0);
        assert!(!IdentityConfig::default().disable_enhanced_security);
        assert_eq!(IdentityConfig::default().alloc_id_timeout, 30);
    }
}
