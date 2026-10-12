// SPDX-License-Identifier: GPL-2.0-only
use std::collections::BTreeMap;

pub enum Value {
    Text(String),
    Bool(bool),
    Integer(i64),
    Unsigned(u64),
    Number(f64),
}

pub type Snapshot = BTreeMap<String, BTreeMap<String, Value>>;

fn quoted(text: &str) -> String {
    let mut out = String::from("\"");
    for ch in text.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c < '\u{20}' => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

impl Value {
    fn json(&self) -> String {
        match self {
            Self::Text(value) => quoted(value),
            Self::Bool(value) => value.to_string(),
            Self::Integer(value) => value.to_string(),
            Self::Unsigned(value) => quoted(&value.to_string()),
            Self::Number(value) => format!("{value:.4}"),
        }
    }

    pub fn text(&self) -> String {
        match self {
            Self::Text(value) => value.clone(),
            Self::Bool(value) => if *value { "1" } else { "0" }.to_owned(),
            Self::Integer(value) => value.to_string(),
            Self::Unsigned(value) => value.to_string(),
            Self::Number(value) => format!("{value:.2}"),
        }
    }
}

pub fn json(snapshot: &Snapshot) -> String {
    let groups = snapshot
        .iter()
        .map(|(name, fields)| {
            let values = fields
                .iter()
                .map(|(key, value)| format!("{}:{}", quoted(key), value.json()))
                .collect::<Vec<_>>()
                .join(",");
            format!("{}:{{{values}}}", quoted(name))
        })
        .collect::<Vec<_>>()
        .join(",");
    format!("{{\"schema_version\":1,{groups}}}")
}

pub fn human(snapshot: &Snapshot) -> String {
    let mut output = String::new();
    for group in ["line", "frontend", "registration", "datapath", "counters"] {
        let Some(fields) = snapshot.get(group) else {
            continue;
        };
        if !output.is_empty() {
            output.push('\n');
        }
        output.push_str(&format!("[{group}]\n"));
        for (name, value) in fields {
            output.push_str(&format!("{name}: {}\n", value.text()));
        }
    }
    output
}

pub fn convert_optics(snapshot: &mut Snapshot) {
    let Some(frontend) = snapshot.get_mut("frontend") else {
        return;
    };

    for (raw, name, factor) in [
        ("temperature_8472", "temperature_celsius", 1.0 / 256.0),
        ("voltage_8472", "voltage_volts", 0.0001),
        ("tx_bias_8472", "tx_bias_ma", 0.002),
    ] {
        if let Some(Value::Integer(value)) = frontend.remove(raw) {
            frontend.insert(name.into(), Value::Number(value as f64 * factor));
        }
    }

    for (raw, name) in [
        ("tx_power_8472", "tx_power_dbm"),
        ("rx_power_8472", "rx_power_dbm"),
    ] {
        if let Some(Value::Integer(value)) = frontend.remove(raw) {
            let dbm = if value == 0 {
                -40.0
            } else {
                10.0 * (value as f64).log10() - 40.0
            };
            frontend.insert(name.into(), Value::Number(dbm));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn optics_use_display_units() {
        let mut snapshot = Snapshot::from([(
            "frontend".into(),
            BTreeMap::from([
                ("temperature_8472".into(), Value::Integer(-128)),
                ("tx_power_8472".into(), Value::Integer(0)),
                ("rx_power_8472".into(), Value::Integer(18)),
            ]),
        )]);

        convert_optics(&mut snapshot);
        let frontend = &snapshot["frontend"];
        assert_eq!(frontend["temperature_celsius"].text(), "-0.50");
        assert_eq!(frontend["tx_power_dbm"].text(), "-40.00");
        assert_eq!(frontend["rx_power_dbm"].text(), "-27.45");
        assert!(!frontend.contains_key("tx_power_8472"));
    }

    #[test]
    fn counters_keep_u64_precision() {
        let snapshot = Snapshot::from([(
            "counters".into(),
            BTreeMap::from([("frames".into(), Value::Unsigned(u64::MAX))]),
        )]);

        assert!(json(&snapshot).contains("\"frames\":\"18446744073709551615\""));
    }
}
