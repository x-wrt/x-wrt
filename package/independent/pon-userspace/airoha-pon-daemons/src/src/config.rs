// SPDX-License-Identifier: GPL-2.0-only

use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::Path;

#[derive(Clone, Debug)]
pub struct Section {
    pub name: String,
    pub kind: String,
    options: BTreeMap<String, String>,
}

impl Section {
    pub fn option(&self, name: &str) -> Option<&str> {
        self.options.get(name).map(String::as_str)
    }
}

#[derive(Clone, Debug)]
pub struct PonConfig {
    sections: Vec<Section>,
}

impl PonConfig {
    pub fn load(path: &Path) -> io::Result<Self> {
        let input = fs::read_to_string(path)?;
        Self::parse(&input)
    }

    pub fn parse(input: &str) -> io::Result<Self> {
        let mut sections = Vec::<Section>::new();

        for (line_number, line) in input.lines().enumerate() {
            let fields = split_uci_line(line).map_err(|message| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("line {}: {message}", line_number + 1),
                )
            })?;
            if fields.is_empty() {
                continue;
            }

            match fields[0].as_str() {
                "config" if fields.len() == 3 => sections.push(Section {
                    kind: fields[1].clone(),
                    name: fields[2].clone(),
                    options: BTreeMap::new(),
                }),
                "option" if fields.len() == 3 => {
                    let section = sections.last_mut().ok_or_else(|| {
                        io::Error::new(
                            io::ErrorKind::InvalidData,
                            format!("line {}: option appears before config", line_number + 1),
                        )
                    })?;
                    section.options.insert(fields[1].clone(), fields[2].clone());
                }
                "list" if fields.len() == 3 => {
                    let section = sections.last_mut().ok_or_else(|| {
                        io::Error::new(
                            io::ErrorKind::InvalidData,
                            format!("line {}: list appears before config", line_number + 1),
                        )
                    })?;
                    section
                        .options
                        .entry(fields[1].clone())
                        .and_modify(|value| {
                            value.push(' ');
                            value.push_str(&fields[2]);
                        })
                        .or_insert_with(|| fields[2].clone());
                }
                _ => {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("line {}: invalid UCI statement", line_number + 1),
                    ));
                }
            }
        }

        Ok(Self { sections })
    }

    pub fn section(&self, name: &str, kind: &str) -> io::Result<&Section> {
        self.sections
            .iter()
            .find(|section| section.name == name && section.kind == kind)
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("UCI {kind} section '{name}' was not found"),
                )
            })
    }

    pub fn linked_section(&self, kind: &str, line: &str) -> io::Result<&Section> {
        self.sections
            .iter()
            .find(|section| section.kind == kind && section.option("line") == Some(line))
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("UCI {kind} section for line '{line}' was not found"),
                )
            })
    }
}

fn split_uci_line(line: &str) -> Result<Vec<String>, &'static str> {
    let mut fields = Vec::new();
    let mut field = String::new();
    let mut field_started = false;
    let mut quote = None;
    let mut escaped = false;

    for character in line.chars() {
        if escaped {
            field.push(character);
            field_started = true;
            escaped = false;
            continue;
        }
        if character == '\\' && quote != Some('\'') {
            escaped = true;
            continue;
        }
        if let Some(active_quote) = quote {
            if character == active_quote {
                quote = None;
            } else {
                field.push(character);
            }
            continue;
        }
        match character {
            '\'' | '"' => {
                quote = Some(character);
                field_started = true;
            }
            '#' => break,
            character if character.is_whitespace() => {
                if field_started {
                    fields.push(std::mem::take(&mut field));
                    field_started = false;
                }
            }
            _ => {
                field.push(character);
                field_started = true;
            }
        }
    }

    if escaped || quote.is_some() {
        return Err("unterminated quoted value");
    }
    if field_started {
        fields.push(field);
    }
    Ok(fields)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_named_sections_and_quoted_values() {
        let config = PonConfig::parse(
            "config xpon 'line0'\n\
             \toption mode '10g-epon'\n\
             config oam 'line0_oam'\n\
             \toption line 'line0'\n\
             \toption loid_password 'fixture-secret'\n",
        )
        .unwrap();

        assert_eq!(
            config.section("line0", "xpon").unwrap().option("mode"),
            Some("10g-epon")
        );
        assert_eq!(
            config
                .linked_section("oam", "line0")
                .unwrap()
                .option("loid_password"),
            Some("fixture-secret")
        );
    }

    #[test]
    fn joins_list_values_in_declaration_order() {
        let config = PonConfig::parse(
            "config oam 'line0_oam'\n\
             \toption line 'line0'\n\
             \tlist ctc_versions '21'\n\
             \tlist ctc_versions '30'\n",
        )
        .unwrap();

        assert_eq!(
            config
                .linked_section("oam", "line0")
                .unwrap()
                .option("ctc_versions"),
            Some("21 30")
        );
    }

    #[test]
    fn preserves_empty_quoted_options() {
        let config = PonConfig::parse("config xpon 'line0'\n\toption mode ''\n").unwrap();

        assert_eq!(
            config.section("line0", "xpon").unwrap().option("mode"),
            Some("")
        );
    }
}
