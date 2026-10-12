// SPDX-License-Identifier: GPL-2.0-only

//! Service configuration model derived from the OMCI MIB.
//!
//! `mib` stores OLT-provisioned managed entities. This snapshot forms the
//! boundary between the MIB and the AN7581 kernel data path and is shared by
//! the control interface and kernel backend.

use std::collections::BTreeSet;

/// Match value for untagged upstream frames and tagged frames after concrete VID lookup.
pub const DATA_PATH_VLAN_ANY: u16 = u16::MAX;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DataPathCandidate {
    pub alloc_id: u16,
    pub gem_id: u16,
    pub vlan_id: u16,
    pub pbit_mask: u8,
    /// A Class 281 GEM carries downstream multicast traffic.
    pub multicast: bool,
}

#[derive(Clone, Debug, Default)]
pub struct ProvisioningSnapshot {
    /// OLT-G text fields after fixed-width wire padding is removed.
    pub olt_vendor_id: String,
    pub olt_equipment_id: String,
    pub olt_version: String,
    pub configured_tconts: usize,
    pub gem_ports: usize,
    pub gem_interworking_tps: usize,
    pub vlan_rules: usize,
    /// VLANs used by unicast service data paths.
    pub vlan_ids: BTreeSet<u16>,
    /// VLANs mapped to Class 281 downstream multicast GEMs.
    pub multicast_vlan_ids: BTreeSet<u16>,
    /// IGMP upstream VLANs selected by Class 309 profiles referenced from Class 310.
    pub igmp_upstream_vlan_ids: BTreeSet<u16>,
    /// Class 309 attribute 5: 0 transparent, 1 add, 2 replace TCI, 3 replace VID.
    pub igmp_upstream_tag_controls: BTreeSet<u8>,
    /// Class 332 is present in the MIB.
    pub enhanced_security: bool,
    /// Key indexes with at least one Class 332 attribute 11 broadcast key fragment.
    pub broadcast_key_indexes: BTreeSet<u8>,
    pub data_paths: Vec<DataPathCandidate>,
}

#[derive(Clone, Debug)]
pub struct DataPathTableRow {
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}

#[derive(Clone, Debug)]
pub struct DataPathAttribute {
    pub index: u8,
    pub value: Vec<u8>,
    pub table_rows: Vec<DataPathTableRow>,
}

#[derive(Clone, Debug)]
pub struct DataPathEntity {
    pub class_id: u16,
    pub entity_id: u16,
    pub attributes: Vec<DataPathAttribute>,
}

#[derive(Clone, Debug, Default)]
pub struct DataPathGraph {
    pub candidates: Vec<DataPathCandidate>,
    pub entities: Vec<DataPathEntity>,
}
