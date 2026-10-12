// SPDX-License-Identifier: GPL-2.0-only
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use crate::runtime::parent_xpon_attribute;

use super::config::decode_hex;
use super::provisioning::{DataPathCandidate, ProvisioningSnapshot, DATA_PATH_VLAN_ANY};

/* The kernel applies deferred paths on its own; pond polls to report the outcome. */
const ALLOC_ID_RECHECK_INTERVAL: Duration = Duration::from_secs(1);

#[derive(Clone, Debug)]
pub struct BackendStatus {
    pub state: &'static str,
    pub active_alloc_id: Option<u16>,
    pub active_gem_id: Option<u16>,
    pub all_vlans: bool,
    pub error: Option<String>,
}

impl Default for BackendStatus {
    fn default() -> Self {
        Self {
            state: "waiting-for-gem",
            active_alloc_id: None,
            active_gem_id: None,
            all_vlans: false,
            error: None,
        }
    }
}

pub struct DataPathBackend {
    data_path: PathBuf,
    service_ready: Option<PathBuf>,
    omci_msk: PathBuf,
    active_serial_number: PathBuf,
    applied: Vec<DataPathCandidate>,
    status: BackendStatus,
    alloc_id_timeout: Option<Duration>,
    waiting_since: Option<Instant>,
}

impl DataPathBackend {
    pub fn for_omci_interface(interface: &str, alloc_id_timeout: u32) -> io::Result<Self> {
        let data_path = parent_xpon_attribute(interface, "data_path")?.ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                format!("OMCI interface {interface} has no parent xpon data path"),
            )
        })?;
        let service_ready = parent_xpon_attribute(interface, "service_ready")?;
        let omci_msk = parent_xpon_attribute(interface, "omci_msk")?.ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                format!("OMCI interface {interface} has no parent xpon MSK attribute"),
            )
        })?;
        let active_serial_number = parent_xpon_attribute(interface, "active_serial_number")?
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::NotFound,
                    format!("OMCI interface {interface} has no active serial-number attribute"),
                )
            })?;
        let applied = read_current_mapping(&data_path)?;
        let mut status = BackendStatus::default();
        if let Some(mapping) = applied.iter().find(|path| !path.multicast) {
            status.state = "kernel-mapping-present";
            status.active_alloc_id = Some(mapping.alloc_id);
            status.active_gem_id = Some(mapping.gem_id);
        }
        Ok(Self {
            data_path,
            service_ready,
            omci_msk,
            active_serial_number,
            applied,
            status,
            alloc_id_timeout: (alloc_id_timeout != 0)
                .then(|| Duration::from_secs(alloc_id_timeout.into())),
            waiting_since: None,
        })
    }

    /// Interval at which the caller should reconcile again without new OMCI requests.
    pub fn recheck_interval(&self) -> Option<Duration> {
        self.waiting_since.map(|_| ALLOC_ID_RECHECK_INTERVAL)
    }

    pub fn reconcile(&mut self, snapshot: &ProvisioningSnapshot) {
        self.status.error = None;
        /* Only a repeated EAGAIN keeps the wait running. */
        let waiting_since = self.waiting_since.take();

        /* A new PON epoch clears hardware GEM mappings while the OMCI MIB retains provisioned state. */
        self.applied = match read_current_mapping(&self.data_path) {
            Ok(mapping) => mapping,
            Err(error) => {
                self.publish_service_ready(false);
                self.status.state = "apply-failed";
                self.status.error = Some(error.to_string());
                return;
            }
        };
        self.status.active_alloc_id = self
            .applied
            .iter()
            .find(|path| !path.multicast)
            .map(|path| path.alloc_id);
        self.status.active_gem_id = self
            .applied
            .iter()
            .find(|path| !path.multicast)
            .map(|path| path.gem_id);

        let mut desired = snapshot.data_paths.clone();
        let distinct_gems = desired
            .iter()
            .filter(|path| !path.multicast)
            .map(|path| path.gem_id)
            .collect::<std::collections::BTreeSet<_>>();
        if distinct_gems.len() == 1 {
            /* A single upstream GEM accepts every VLAN and keeps multicast GEMs as separate hardware paths. */
            let mut unicast = desired
                .iter()
                .find(|path| !path.multicast)
                .cloned()
                .expect("one unicast GEM must have one path");
            unicast.vlan_id = DATA_PATH_VLAN_ANY;
            unicast.pbit_mask = u8::MAX;
            desired.retain(|path| path.multicast);
            desired.insert(0, unicast);
        } else if distinct_gems.len() > 1 {
            /*
             * VID_ANY matches untagged frames and tagged frames after concrete VID lookup.
             * The kernel ranks a concrete VID first; equal VID and P-bit coverage creates ambiguity.
             */
            for (index, path) in desired.iter().enumerate() {
                if path.multicast {
                    continue;
                }
                let conflict = desired[..index].iter().any(|other| {
                    !other.multicast
                        && path.pbit_mask & other.pbit_mask != 0
                        && path.vlan_id == other.vlan_id
                });
                if path.pbit_mask == 0 || conflict {
                    if let Err(error) = self.clear_applied() {
                        self.publish_service_ready(false);
                        self.status.state = "clear-failed";
                        self.status.error = Some(error.to_string());
                        return;
                    }
                    self.status.state = "ambiguous-gem-mapping";
                    self.publish_service_ready(false);
                    self.status.error = Some(format!(
                        "{} bidirectional GEM paths contain overlapping VLAN/802.1p rules",
                        distinct_gems.len()
                    ));
                    return;
                }
            }
        }
        let service_ready = desired.iter().any(|path| !path.multicast);

        if desired.is_empty() {
            if let Err(error) = self.clear_applied() {
                self.publish_service_ready(false);
                self.status.state = "clear-failed";
                self.status.error = Some(error.to_string());
                return;
            }
            self.status.state = "waiting-for-gem";
            self.publish_service_ready(false);
            return;
        }
        if self.applied == desired {
            if let Some(since) = waiting_since {
                println!(
                    "OMCI data paths: kernel applied the deferred paths after {} s",
                    since.elapsed().as_secs()
                );
            }
            self.status.state = "applied";
            self.publish_service_ready(service_ready);
            return;
        }

        let mut value = String::from("replace");
        for path in &desired {
            value.push_str(&format!(
                " {}:{}:{}:{:02x}:{}",
                path.alloc_id,
                path.gem_id,
                path.vlan_id,
                path.pbit_mask,
                u8::from(path.multicast)
            ));
        }
        value.push('\n');
        match fs::write(&self.data_path, value) {
            Ok(()) => {
                self.applied = desired;
                self.status.state = "applied";
                self.status.active_alloc_id = self
                    .applied
                    .iter()
                    .find(|path| !path.multicast)
                    .map(|path| path.alloc_id);
                self.status.active_gem_id = self
                    .applied
                    .iter()
                    .find(|path| !path.multicast)
                    .map(|path| path.gem_id);
                self.publish_service_ready(service_ready);
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                /*
                 * EAGAIN: the kernel holds the request until PLOAM assigns every unicast
                 * Alloc-ID. A timeout only reports the failure; a late assignment still applies.
                 */
                self.publish_service_ready(false);
                let since = waiting_since.unwrap_or_else(Instant::now);
                self.waiting_since = Some(since);
                match self.alloc_id_timeout {
                    Some(timeout) if since.elapsed() >= timeout => {
                        if self.status.state != "alloc-id-timeout" {
                            println!(
                                "OMCI data paths: PLOAM has not assigned every Alloc-ID after {} s",
                                timeout.as_secs()
                            );
                        }
                        self.status.state = "alloc-id-timeout";
                        self.status.error = Some(format!(
                            "PLOAM has not assigned every Alloc-ID after {} s",
                            timeout.as_secs()
                        ));
                    }
                    _ => self.status.state = "waiting-for-alloc-id",
                }
            }
            Err(error) => {
                self.publish_service_ready(false);
                self.status.state = "apply-failed";
                self.status.error = Some(error.to_string());
            }
        }
    }

    pub fn status(&self) -> BackendStatus {
        let mut status = self.status.clone();
        status.all_vlans = self.applied.iter().any(|path| !path.multicast)
            && self
                .applied
                .iter()
                .filter(|path| !path.multicast)
                .all(|path| path.vlan_id == DATA_PATH_VLAN_ANY);
        status
    }

    pub fn install_master_session_key(&self, key: &[u8; 16]) -> io::Result<()> {
        /* The sysfs ABI accepts the session key as exactly 32 hexadecimal characters. */
        let mut encoded = String::with_capacity(33);
        for byte in key {
            use std::fmt::Write;
            write!(&mut encoded, "{byte:02x}").expect("writing to String cannot fail");
        }
        encoded.push('\n');
        fs::write(&self.omci_msk, encoded)
    }

    pub fn active_serial_number(&self) -> io::Result<Option<[u8; 8]>> {
        let text = fs::read_to_string(&self.active_serial_number)?;
        let text = text.trim();
        if text == "none" {
            return Ok(None);
        }
        let bytes =
            decode_hex(text).map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
        let serial = bytes.try_into().map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "active PON serial number is not eight bytes",
            )
        })?;
        Ok(Some(serial))
    }

    fn clear_applied(&mut self) -> io::Result<()> {
        if self.applied.is_empty() {
            return Ok(());
        }
        fs::write(&self.data_path, "clear\n")?;
        self.applied.clear();
        self.status.active_alloc_id = None;
        self.status.active_gem_id = None;
        Ok(())
    }

    fn publish_service_ready(&self, ready: bool) {
        let Some(path) = &self.service_ready else {
            return;
        };
        if let Err(error) = fs::write(path, if ready { "1\n" } else { "0\n" }) {
            println!("PON service LED state update failed: {error}");
        }
    }
}

fn read_current_mapping(path: &Path) -> io::Result<Vec<DataPathCandidate>> {
    let status = fs::read_to_string(path)?;
    let value = |key: &str| {
        status.split_whitespace().find_map(|field| {
            let (name, value) = field.split_once('=')?;
            (name == key).then_some(value)
        })
    };
    if value("configured") != Some("1") {
        return Ok(Vec::new());
    }
    let mut paths = Vec::new();
    for field in status.split_whitespace() {
        let Some(path) = field.strip_prefix("path=") else {
            continue;
        };
        let mut parts = path.split(':');
        let alloc_id = parts
            .next()
            .and_then(|part| part.parse::<u16>().ok())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid alloc_id"))?;
        let gem_id = parts
            .next()
            .and_then(|part| part.parse::<u16>().ok())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid gem_id"))?;
        let vlan_id = parts
            .next()
            .and_then(|part| part.parse::<u16>().ok())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid vlan_id"))?;
        let pbit_mask = parts
            .next()
            .and_then(|part| u8::from_str_radix(part, 16).ok())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid pbit mask"))?;
        /* Four-field records emitted through r52 represent regular unicast GEM paths. */
        let multicast = match parts.next() {
            None => false,
            Some("0") => false,
            Some("1") => true,
            Some(_) => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "invalid GEM type",
                ));
            }
        };
        if parts.next().is_some() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "data path has too many fields",
            ));
        }
        paths.push(DataPathCandidate {
            alloc_id,
            gem_id,
            vlan_id,
            pbit_mask,
            multicast,
        });
    }
    if paths.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "configured data path contains no path entries",
        ));
    }
    Ok(paths)
}
