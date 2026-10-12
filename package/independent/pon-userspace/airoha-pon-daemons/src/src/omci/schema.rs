// SPDX-License-Identifier: GPL-2.0-only

//! Declarative OMCI managed-entity definitions.

use super::protocol::{
    ACTION_CREATE, ACTION_DELETE, ACTION_GET, ACTION_GET_ALL_ALARMS, ACTION_GET_ALL_ALARMS_NEXT,
    ACTION_GET_CURRENT_DATA, ACTION_GET_NEXT, ACTION_MIB_RESET, ACTION_MIB_UPLOAD,
    ACTION_MIB_UPLOAD_NEXT, ACTION_SET, ACTION_SET_TABLE, ACTION_SYNCHRONIZE_TIME,
};

pub const CLASS_ONU_DATA: u16 = 2;
pub const CLASS_CARDHOLDER: u16 = 5;
pub const CLASS_CIRCUIT_PACK: u16 = 6;
pub const CLASS_SOFTWARE_IMAGE: u16 = 7;
pub const CLASS_PPTP_ETHERNET_UNI: u16 = 11;
pub const CLASS_ETHERNET_PM_HISTORY_DATA: u16 = 24;
pub const CLASS_MAC_BRIDGE_SERVICE_PROFILE: u16 = 45;
pub const CLASS_MAC_BRIDGE_PORT_CONFIG_DATA: u16 = 47;
pub const CLASS_MAC_BRIDGE_PORT_PM_HISTORY_DATA: u16 = 52;
pub const CLASS_MAC_BRIDGE_PORT_FILTER_PREASSIGN_DATA: u16 = 79;
pub const CLASS_VLAN_TAGGING_FILTER: u16 = 84;
pub const CLASS_ETHERNET_PM_HISTORY_DATA_2: u16 = 89;
pub const CLASS_IEEE_8021P_MAPPER: u16 = 130;
pub const CLASS_OLT_G: u16 = 131;
pub const CLASS_ONU_POWER_SHEDDING: u16 = 133;
pub const CLASS_EXTENDED_VLAN_TAGGING: u16 = 171;
pub const CLASS_VENDOR_247: u16 = 247;
pub const CLASS_ONU_G: u16 = 256;
pub const CLASS_ONU2_G: u16 = 257;
pub const CLASS_TCONT: u16 = 262;
pub const CLASS_ANI_G: u16 = 263;
pub const CLASS_UNI_G: u16 = 264;
pub const CLASS_GEM_INTERWORKING_TP: u16 = 266;
pub const CLASS_GEM_PORT_PM_HISTORY_DATA: u16 = 267;
pub const CLASS_GEM_PORT_NETWORK_CTP: u16 = 268;
pub const CLASS_GAL_ETHERNET_PROFILE: u16 = 272;
pub const CLASS_THRESHOLD_DATA_1: u16 = 273;
pub const CLASS_THRESHOLD_DATA_2: u16 = 274;
pub const CLASS_PRIORITY_QUEUE: u16 = 277;
pub const CLASS_TRAFFIC_SCHEDULER: u16 = 278;
pub const CLASS_TRAFFIC_DESCRIPTOR: u16 = 280;
pub const CLASS_MULTICAST_GEM_INTERWORKING_TP: u16 = 281;
pub const CLASS_OMCI: u16 = 287;
pub const CLASS_ETHERNET_PM_HISTORY_DATA_3: u16 = 296;
pub const CLASS_MULTICAST_OPERATIONS_PROFILE: u16 = 309;
pub const CLASS_MULTICAST_SUBSCRIBER_CONFIG: u16 = 310;
pub const CLASS_MULTICAST_SUBSCRIBER_MONITOR: u16 = 311;
pub const CLASS_FEC_PM_HISTORY_DATA: u16 = 312;
pub const CLASS_VEIP: u16 = 329;
pub const CLASS_ENHANCED_SECURITY_CONTROL: u16 = 332;
pub const CLASS_ETHERNET_FRAME_EXTENDED_PM: u16 = 334;
pub const CLASS_VENDOR_351: u16 = 351;
pub const CLASS_CTC_LOID_AUTH: u16 = 0xfffa;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ManagedEntityOrigin {
    Onu,
    Olt,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AttributeAccess {
    ReadOnly,
    ReadWrite,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AttributeKind {
    Scalar { length: usize },
    Table { row_length: usize },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CreateSource {
    None,
    Required { offset: usize },
    Optional { offset: usize },
}

#[derive(Clone, Copy, Debug)]
pub struct AttributeDefinition {
    pub index: u8,
    pub access: AttributeAccess,
    pub kind: AttributeKind,
    pub create: CreateSource,
    pub default: &'static [u8],
    pub upload: bool,
}

impl AttributeDefinition {
    pub const fn read_only(index: u8, length: usize) -> Self {
        Self {
            index,
            access: AttributeAccess::ReadOnly,
            kind: AttributeKind::Scalar { length },
            create: CreateSource::None,
            default: &[],
            upload: true,
        }
    }

    pub const fn read_write(index: u8, length: usize) -> Self {
        Self {
            index,
            access: AttributeAccess::ReadWrite,
            kind: AttributeKind::Scalar { length },
            create: CreateSource::None,
            default: &[],
            upload: true,
        }
    }

    pub const fn read_only_table(index: u8, row_length: usize) -> Self {
        Self {
            index,
            access: AttributeAccess::ReadOnly,
            kind: AttributeKind::Table { row_length },
            create: CreateSource::None,
            default: &[],
            upload: false,
        }
    }

    pub const fn read_write_table(index: u8, row_length: usize) -> Self {
        Self {
            index,
            access: AttributeAccess::ReadWrite,
            kind: AttributeKind::Table { row_length },
            create: CreateSource::None,
            default: &[],
            upload: false,
        }
    }

    pub const fn required(mut self, offset: usize) -> Self {
        self.create = CreateSource::Required { offset };
        self
    }

    pub const fn optional(mut self, offset: usize) -> Self {
        self.create = CreateSource::Optional { offset };
        self
    }

    pub const fn with_default(mut self, value: &'static [u8]) -> Self {
        self.default = value;
        self
    }

    pub const fn runtime_only(mut self) -> Self {
        self.upload = false;
        self
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ManagedEntityDefinition {
    pub class_id: u16,
    pub name: &'static str,
    pub origin: ManagedEntityOrigin,
    pub actions: u32,
    pub attributes: &'static [AttributeDefinition],
}

impl ManagedEntityDefinition {
    pub const fn supports_action(&self, action: u8) -> bool {
        self.actions & action_bit(action) != 0
    }
}

const fn action_bit(action: u8) -> u32 {
    1u32 << action
}

const ACTIONS_GET: u32 = action_bit(ACTION_GET);
const ACTIONS_ONU_RW: u32 = action_bit(ACTION_GET) | action_bit(ACTION_SET);
const ACTIONS_OLT_RW: u32 = action_bit(ACTION_CREATE)
    | action_bit(ACTION_DELETE)
    | action_bit(ACTION_GET)
    | action_bit(ACTION_SET);
const ACTIONS_OLT_TABLE: u32 =
    ACTIONS_OLT_RW | action_bit(ACTION_GET_NEXT) | action_bit(ACTION_SET_TABLE);
const ACTIONS_PM: u32 = ACTIONS_OLT_RW | action_bit(ACTION_GET_CURRENT_DATA);

const UNUSED_U16: &[u8] = &[0xff, 0xff];
const DEFAULT_TPID: &[u8] = &[0x81, 0x00];
const SECURITY_KEY_LENGTH: &[u8] = &[0, 128];

const ONU_DATA_ATTRIBUTES: &[AttributeDefinition] = &[AttributeDefinition::read_write(1, 1)];

const CARDHOLDER_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_write(2, 1),
    AttributeDefinition::read_write(3, 1),
    AttributeDefinition::read_write(4, 20),
    AttributeDefinition::read_only(5, 20),
    AttributeDefinition::read_write(6, 1),
    AttributeDefinition::read_write(7, 1),
    AttributeDefinition::read_write(8, 1),
    AttributeDefinition::read_write(9, 1),
];

const CIRCUIT_PACK_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_only(2, 1),
    AttributeDefinition::read_only(3, 8),
    AttributeDefinition::read_only(4, 14),
    AttributeDefinition::read_only(5, 4),
    AttributeDefinition::read_write(6, 1),
    AttributeDefinition::read_only(7, 1),
    AttributeDefinition::read_only(8, 1),
    AttributeDefinition::read_only(9, 20),
    AttributeDefinition::read_write(10, 1),
    AttributeDefinition::read_only(11, 1),
    AttributeDefinition::read_only(12, 1),
    AttributeDefinition::read_only(13, 1),
    AttributeDefinition::read_write(14, 4),
];

const SOFTWARE_IMAGE_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 14),
    AttributeDefinition::read_only(2, 1),
    AttributeDefinition::read_only(3, 1),
    AttributeDefinition::read_only(4, 1),
];

const ETHERNET_UNI_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 1),
    AttributeDefinition::read_only(2, 1),
    AttributeDefinition::read_write(3, 1),
    AttributeDefinition::read_write(4, 1),
    AttributeDefinition::read_write(5, 1),
    AttributeDefinition::read_only(6, 1),
    AttributeDefinition::read_only(7, 1),
    AttributeDefinition::read_write(8, 2),
    AttributeDefinition::read_write(9, 1),
    AttributeDefinition::read_write(10, 2),
    AttributeDefinition::read_write(11, 1),
    AttributeDefinition::read_write(12, 1),
    AttributeDefinition::read_write(13, 1),
    AttributeDefinition::read_write(14, 1),
    AttributeDefinition::read_write(15, 1),
];

const MAC_BRIDGE_SERVICE_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 1).required(0),
    AttributeDefinition::read_write(2, 1).required(1),
    AttributeDefinition::read_write(3, 1).required(2),
    AttributeDefinition::read_write(4, 2).required(3),
    AttributeDefinition::read_write(5, 2).required(5),
    AttributeDefinition::read_write(6, 2).required(7),
    AttributeDefinition::read_write(7, 2).required(9),
    AttributeDefinition::read_write(8, 1).required(11),
    AttributeDefinition::read_write(9, 1).required(12),
    AttributeDefinition::read_write(10, 4).required(13),
];

const MAC_BRIDGE_PORT_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 2).required(0),
    AttributeDefinition::read_write(2, 1).required(2),
    AttributeDefinition::read_write(3, 1).required(3),
    AttributeDefinition::read_write(4, 2).required(4),
    AttributeDefinition::read_write(5, 2).required(6),
    AttributeDefinition::read_write(6, 2).required(8),
    AttributeDefinition::read_write(7, 1).required(10),
    AttributeDefinition::read_write(8, 1).required(11),
    AttributeDefinition::read_write(9, 1).required(12),
    AttributeDefinition::read_only(10, 6),
    AttributeDefinition::read_write(11, 2).with_default(UNUSED_U16),
    AttributeDefinition::read_write(12, 2).with_default(UNUSED_U16),
    AttributeDefinition::read_write(13, 1).required(13),
];

const FILTER_PREASSIGN_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 1),
    AttributeDefinition::read_write(2, 1),
    AttributeDefinition::read_write(3, 1),
    AttributeDefinition::read_write(4, 1),
    AttributeDefinition::read_write(5, 1),
    AttributeDefinition::read_write(6, 1),
    AttributeDefinition::read_write(7, 1),
    AttributeDefinition::read_write(8, 1),
    AttributeDefinition::read_write(9, 1),
    AttributeDefinition::read_write(10, 1),
];

const VLAN_FILTER_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 24).required(0),
    AttributeDefinition::read_write(2, 1).required(24),
    AttributeDefinition::read_write(3, 1).required(25),
];

const IEEE_8021P_MAPPER_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 2).required(0),
    AttributeDefinition::read_write(2, 2).required(2),
    AttributeDefinition::read_write(3, 2).required(4),
    AttributeDefinition::read_write(4, 2).required(6),
    AttributeDefinition::read_write(5, 2).required(8),
    AttributeDefinition::read_write(6, 2).required(10),
    AttributeDefinition::read_write(7, 2).required(12),
    AttributeDefinition::read_write(8, 2).required(14),
    AttributeDefinition::read_write(9, 2).required(16),
    AttributeDefinition::read_write(10, 1).required(18),
    AttributeDefinition::read_write(11, 24),
    AttributeDefinition::read_write(12, 1).required(19),
    AttributeDefinition::read_write(13, 1).optional(20),
];

const OLT_G_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 4),
    AttributeDefinition::read_write(2, 20),
    AttributeDefinition::read_write(3, 14),
    AttributeDefinition::read_write(4, 14),
];

const POWER_SHEDDING_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 2),
    AttributeDefinition::read_write(2, 2),
    AttributeDefinition::read_write(4, 2),
    AttributeDefinition::read_write(5, 2),
    AttributeDefinition::read_write(6, 2),
    AttributeDefinition::read_write(7, 2),
    AttributeDefinition::read_write(8, 2),
    AttributeDefinition::read_write(9, 2),
    AttributeDefinition::read_write(10, 2),
    AttributeDefinition::read_write(11, 2),
    AttributeDefinition::read_only(12, 1),
];

const EXTENDED_VLAN_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 1).required(0),
    AttributeDefinition::read_only(2, 2).with_default(&[0, 32]),
    AttributeDefinition::read_write(3, 2).with_default(DEFAULT_TPID),
    AttributeDefinition::read_write(4, 2).with_default(DEFAULT_TPID),
    AttributeDefinition::read_write(5, 1),
    AttributeDefinition::read_write_table(6, 16),
    AttributeDefinition::read_write(7, 2).required(1),
    AttributeDefinition::read_write(8, 24),
];

const ONU_G_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 4),
    AttributeDefinition::read_only(2, 14),
    AttributeDefinition::read_only(3, 8),
    AttributeDefinition::read_only(4, 1),
    AttributeDefinition::read_only(5, 1),
    AttributeDefinition::read_write(6, 1),
    AttributeDefinition::read_write(7, 1),
    AttributeDefinition::read_only(8, 1),
    AttributeDefinition::read_only(9, 1),
    AttributeDefinition::read_only(10, 24),
    AttributeDefinition::read_only(11, 12),
    AttributeDefinition::read_write(12, 1),
    AttributeDefinition::read_only(13, 1),
];

const ONU2_G_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 20),
    AttributeDefinition::read_only(2, 1),
    AttributeDefinition::read_only(3, 2),
    AttributeDefinition::read_only(4, 1),
    AttributeDefinition::read_write(5, 1),
    AttributeDefinition::read_only(6, 2),
    AttributeDefinition::read_only(7, 1),
    AttributeDefinition::read_only(8, 1),
    AttributeDefinition::read_only(9, 2),
    AttributeDefinition::read_only(10, 4),
    AttributeDefinition::read_only(11, 2),
    AttributeDefinition::read_write(12, 1),
    AttributeDefinition::read_only(13, 2),
    AttributeDefinition::read_write(14, 2),
];

const TCONT_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 2).with_default(UNUSED_U16),
    AttributeDefinition::read_only(2, 1).with_default(&[1]),
    AttributeDefinition::read_write(3, 1),
];

const ANI_G_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_only(2, 2),
    AttributeDefinition::read_write(3, 2),
    AttributeDefinition::read_only(4, 1),
    AttributeDefinition::read_only(5, 1),
    AttributeDefinition::read_write(6, 1),
    AttributeDefinition::read_write(7, 1),
    AttributeDefinition::read_write(8, 1),
    AttributeDefinition::read_write(9, 1),
    AttributeDefinition::read_only(10, 2),
    AttributeDefinition::read_write(11, 1),
    AttributeDefinition::read_write(12, 1),
    AttributeDefinition::read_only(13, 2),
    AttributeDefinition::read_only(14, 2),
    AttributeDefinition::read_write(15, 1),
    AttributeDefinition::read_write(16, 1),
    /* The OMCI attribute mask addresses these first 16 attributes. */
];

const UNI_G_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 2),
    AttributeDefinition::read_write(2, 1),
    AttributeDefinition::read_only(3, 1),
    AttributeDefinition::read_write(4, 2),
    AttributeDefinition::read_write(5, 2),
];

const GEM_INTERWORKING_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 2).required(0),
    AttributeDefinition::read_write(2, 1).required(2),
    AttributeDefinition::read_write(3, 2).required(3),
    AttributeDefinition::read_write(4, 2).required(5),
    AttributeDefinition::read_only(5, 1),
    AttributeDefinition::read_only(6, 1),
    AttributeDefinition::read_write(7, 2).required(7),
    AttributeDefinition::read_write(8, 1),
];

const GEM_PORT_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 2).required(0),
    AttributeDefinition::read_write(2, 2).required(2),
    AttributeDefinition::read_write(3, 1).required(4),
    AttributeDefinition::read_write(4, 2).required(5),
    AttributeDefinition::read_write(5, 2).required(7),
    AttributeDefinition::read_only(6, 1),
    AttributeDefinition::read_write(7, 2).required(9),
    AttributeDefinition::read_only(8, 1),
    AttributeDefinition::read_write(9, 2).required(11),
    AttributeDefinition::read_write(10, 1),
];

const GAL_PROFILE_ATTRIBUTES: &[AttributeDefinition] =
    &[AttributeDefinition::read_write(1, 2).required(0)];

const PRIORITY_QUEUE_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_only(2, 2).with_default(UNUSED_U16),
    AttributeDefinition::read_write(3, 2).with_default(&[0, 4]),
    AttributeDefinition::read_write(6, 4),
    AttributeDefinition::read_write(7, 2),
    AttributeDefinition::read_write(8, 1).with_default(&[1]),
    AttributeDefinition::read_write(9, 2),
    AttributeDefinition::read_write(10, 4),
    AttributeDefinition::read_write(11, 2).with_default(UNUSED_U16),
    AttributeDefinition::read_write(12, 2),
];

const TRAFFIC_SCHEDULER_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 2),
    AttributeDefinition::read_only(2, 2),
    AttributeDefinition::read_write(3, 1).with_default(&[1]),
    AttributeDefinition::read_write(4, 1),
];

const TRAFFIC_DESCRIPTOR_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 4).required(0),
    AttributeDefinition::read_write(2, 4).required(4),
    AttributeDefinition::read_write(3, 4).required(8),
    AttributeDefinition::read_write(4, 4).required(12),
    AttributeDefinition::read_write(5, 1).required(16),
    AttributeDefinition::read_write(6, 1).required(17),
    AttributeDefinition::read_write(7, 1).required(18),
    AttributeDefinition::read_only(8, 1).required(19),
];

const MULTICAST_GEM_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 2).required(0),
    AttributeDefinition::read_write(2, 1).required(2),
    AttributeDefinition::read_write(3, 2).required(3),
    AttributeDefinition::read_write(4, 2).required(5),
    AttributeDefinition::read_only(5, 1),
    AttributeDefinition::read_only(6, 1),
    AttributeDefinition::read_write(7, 2).required(7),
    AttributeDefinition::read_write(8, 1).required(9),
    AttributeDefinition::read_write_table(9, 12),
];

const OMCI_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only_table(1, 2),
    AttributeDefinition::read_only_table(2, 1),
];

const MULTICAST_OPERATIONS_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 1).required(0),
    AttributeDefinition::read_write(2, 1).required(1),
    AttributeDefinition::read_write(3, 1).required(2),
    AttributeDefinition::read_write(4, 2).required(3),
    AttributeDefinition::read_write(5, 1).required(5),
    AttributeDefinition::read_write(6, 4).required(6),
    AttributeDefinition::read_write_table(7, 24),
    AttributeDefinition::read_write_table(8, 24),
    AttributeDefinition::read_only_table(9, 1),
    AttributeDefinition::read_write(10, 1).required(10),
    AttributeDefinition::read_write(11, 4).required(11),
    AttributeDefinition::read_write(12, 4).required(15),
    AttributeDefinition::read_write(13, 4).required(19),
    AttributeDefinition::read_write(14, 4),
    AttributeDefinition::read_write(15, 1),
    AttributeDefinition::read_write(16, 3).required(23),
];

const MULTICAST_SUBSCRIBER_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 1).required(0),
    AttributeDefinition::read_write(2, 2).required(1),
    AttributeDefinition::read_write(3, 2).required(3),
    AttributeDefinition::read_write(4, 4).required(5),
    AttributeDefinition::read_write(5, 1).required(9),
];

const MULTICAST_MONITOR_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 1).required(0),
    AttributeDefinition::read_only(2, 4),
    AttributeDefinition::read_only(3, 4),
    AttributeDefinition::read_only(4, 4),
    AttributeDefinition::read_only_table(5, 1),
    AttributeDefinition::read_only_table(6, 1),
];

const VEIP_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 1),
    AttributeDefinition::read_only(2, 1),
    AttributeDefinition::read_write(3, 25),
    AttributeDefinition::read_write(4, 2).with_default(UNUSED_U16),
    AttributeDefinition::read_only(5, 2),
];

const ENHANCED_SECURITY_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 16),
    AttributeDefinition::read_write_table(2, 17),
    AttributeDefinition::read_write(3, 1),
    AttributeDefinition::read_only(4, 1).with_default(&[1]),
    AttributeDefinition::read_only_table(5, 16),
    AttributeDefinition::read_only_table(6, 16),
    AttributeDefinition::read_write_table(7, 17),
    AttributeDefinition::read_write(8, 1),
    AttributeDefinition::read_only(9, 1),
    AttributeDefinition::read_only(10, 16),
    AttributeDefinition::read_write_table(11, 18),
    AttributeDefinition::read_only(12, 2).with_default(SECURITY_KEY_LENGTH),
];

const CTC_LOID_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 4),
    AttributeDefinition::read_only(2, 24),
    AttributeDefinition::read_only(3, 12),
    AttributeDefinition::read_write(4, 1),
];

const THRESHOLD_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_write(1, 4).required(0),
    AttributeDefinition::read_write(2, 4).required(4),
    AttributeDefinition::read_write(3, 4).required(8),
    AttributeDefinition::read_write(4, 4).required(12),
    AttributeDefinition::read_write(5, 4).required(16),
    AttributeDefinition::read_write(6, 4).required(20),
    AttributeDefinition::read_write(7, 4).required(24),
];

const ETHERNET_PM_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_write(2, 2).required(0),
    AttributeDefinition::read_only(3, 4).runtime_only(),
    AttributeDefinition::read_only(4, 4).runtime_only(),
    AttributeDefinition::read_only(5, 4).runtime_only(),
    AttributeDefinition::read_only(6, 4).runtime_only(),
    AttributeDefinition::read_only(7, 4).runtime_only(),
    AttributeDefinition::read_only(8, 4).runtime_only(),
    AttributeDefinition::read_only(9, 4).runtime_only(),
    AttributeDefinition::read_only(10, 4).runtime_only(),
    AttributeDefinition::read_only(11, 4).runtime_only(),
    AttributeDefinition::read_only(12, 4).runtime_only(),
    AttributeDefinition::read_only(13, 4).runtime_only(),
    AttributeDefinition::read_only(14, 4).runtime_only(),
    AttributeDefinition::read_only(15, 4).runtime_only(),
    AttributeDefinition::read_only(16, 4).runtime_only(),
];

const ETHERNET_PM_2_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_write(2, 2).required(0),
    AttributeDefinition::read_only(3, 4).runtime_only(),
];

const GEM_PORT_PM_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_write(2, 2).required(0),
    AttributeDefinition::read_only(3, 4).runtime_only(),
    AttributeDefinition::read_only(4, 4).runtime_only(),
    AttributeDefinition::read_only(5, 8).runtime_only(),
    AttributeDefinition::read_only(6, 8).runtime_only(),
    AttributeDefinition::read_only(7, 8).runtime_only(),
    AttributeDefinition::read_only(8, 4).runtime_only(),
    AttributeDefinition::read_only(9, 8).runtime_only(),
];

const FEC_PM_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_write(2, 2).required(0),
    AttributeDefinition::read_only(3, 4).runtime_only(),
    AttributeDefinition::read_only(4, 4).runtime_only(),
    AttributeDefinition::read_only(5, 4).runtime_only(),
    AttributeDefinition::read_only(6, 4).runtime_only(),
    AttributeDefinition::read_only(7, 2).runtime_only(),
];

const MAC_BRIDGE_PORT_PM_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_write(2, 2).required(0),
    AttributeDefinition::read_only(3, 4).runtime_only(),
    AttributeDefinition::read_only(4, 4).runtime_only(),
    AttributeDefinition::read_only(5, 4).runtime_only(),
    AttributeDefinition::read_only(6, 4).runtime_only(),
    AttributeDefinition::read_only(7, 4).runtime_only(),
];

/* Attribute 2 is the 16-byte control block: threshold data, parent ME, accumulation and direction controls. */
const ETHERNET_FRAME_EXTENDED_PM_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_write(2, 16).required(0),
    AttributeDefinition::read_only(3, 4).runtime_only(),
    AttributeDefinition::read_only(4, 4).runtime_only(),
    AttributeDefinition::read_only(5, 4).runtime_only(),
    AttributeDefinition::read_only(6, 4).runtime_only(),
    AttributeDefinition::read_only(7, 4).runtime_only(),
    AttributeDefinition::read_only(8, 4).runtime_only(),
    AttributeDefinition::read_only(9, 4).runtime_only(),
    AttributeDefinition::read_only(10, 4).runtime_only(),
    AttributeDefinition::read_only(11, 4).runtime_only(),
    AttributeDefinition::read_only(12, 4).runtime_only(),
    AttributeDefinition::read_only(13, 4).runtime_only(),
    AttributeDefinition::read_only(14, 4).runtime_only(),
    AttributeDefinition::read_only(15, 4).runtime_only(),
    AttributeDefinition::read_only(16, 4).runtime_only(),
];

/* Huawei OLTs create vendor class 351 like a PM ME whose only create field is the threshold data pointer; its counters are not public. */
const VENDOR_351_ATTRIBUTES: &[AttributeDefinition] = &[
    AttributeDefinition::read_only(1, 1),
    AttributeDefinition::read_write(2, 2).required(0),
];

pub static MANAGED_ENTITIES: &[ManagedEntityDefinition] = &[
    ManagedEntityDefinition {
        class_id: CLASS_ONU_DATA,
        name: "ONU data",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW
            | action_bit(ACTION_MIB_UPLOAD)
            | action_bit(ACTION_MIB_UPLOAD_NEXT)
            | action_bit(ACTION_MIB_RESET)
            | action_bit(ACTION_GET_ALL_ALARMS)
            | action_bit(ACTION_GET_ALL_ALARMS_NEXT),
        attributes: ONU_DATA_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_CARDHOLDER,
        name: "Cardholder",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: CARDHOLDER_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_CIRCUIT_PACK,
        name: "Circuit pack",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: CIRCUIT_PACK_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_SOFTWARE_IMAGE,
        name: "Software image",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_GET,
        attributes: SOFTWARE_IMAGE_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_PPTP_ETHERNET_UNI,
        name: "PPTP Ethernet UNI",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: ETHERNET_UNI_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_ETHERNET_PM_HISTORY_DATA,
        name: "Ethernet PM history data",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_PM,
        attributes: ETHERNET_PM_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_MAC_BRIDGE_SERVICE_PROFILE,
        name: "MAC bridge service profile",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: MAC_BRIDGE_SERVICE_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_MAC_BRIDGE_PORT_CONFIG_DATA,
        name: "MAC bridge port configuration data",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: MAC_BRIDGE_PORT_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_MAC_BRIDGE_PORT_PM_HISTORY_DATA,
        name: "MAC bridge port PM history data",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_PM,
        attributes: MAC_BRIDGE_PORT_PM_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_MAC_BRIDGE_PORT_FILTER_PREASSIGN_DATA,
        name: "MAC bridge port filter preassign table",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: FILTER_PREASSIGN_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_VLAN_TAGGING_FILTER,
        name: "VLAN tagging filter data",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: VLAN_FILTER_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_ETHERNET_PM_HISTORY_DATA_2,
        name: "Ethernet PM history data 2",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_PM,
        attributes: ETHERNET_PM_2_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_IEEE_8021P_MAPPER,
        name: "IEEE 802.1p mapper service profile",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: IEEE_8021P_MAPPER_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_OLT_G,
        name: "OLT-G",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: OLT_G_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_ONU_POWER_SHEDDING,
        name: "ONU power shedding",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: POWER_SHEDDING_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_EXTENDED_VLAN_TAGGING,
        name: "Extended VLAN tagging operation configuration data",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_TABLE,
        attributes: EXTENDED_VLAN_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_VENDOR_247,
        name: "Vendor class 247",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_GET,
        attributes: &[],
    },
    ManagedEntityDefinition {
        class_id: CLASS_ONU_G,
        name: "ONU-G",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW | action_bit(ACTION_SYNCHRONIZE_TIME),
        attributes: ONU_G_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_ONU2_G,
        name: "ONU2-G",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: ONU2_G_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_TCONT,
        name: "T-CONT",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: TCONT_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_ANI_G,
        name: "ANI-G",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: ANI_G_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_UNI_G,
        name: "UNI-G",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: UNI_G_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_GEM_INTERWORKING_TP,
        name: "GEM interworking termination point",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: GEM_INTERWORKING_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_GEM_PORT_PM_HISTORY_DATA,
        name: "GEM port PM history data",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_PM,
        attributes: GEM_PORT_PM_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_GEM_PORT_NETWORK_CTP,
        name: "GEM port network CTP",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: GEM_PORT_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_GAL_ETHERNET_PROFILE,
        name: "GAL Ethernet profile",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: GAL_PROFILE_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_THRESHOLD_DATA_1,
        name: "Threshold data 1",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: THRESHOLD_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_THRESHOLD_DATA_2,
        name: "Threshold data 2",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: THRESHOLD_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_PRIORITY_QUEUE,
        name: "Priority queue",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: PRIORITY_QUEUE_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_TRAFFIC_SCHEDULER,
        name: "Traffic scheduler",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: TRAFFIC_SCHEDULER_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_TRAFFIC_DESCRIPTOR,
        name: "GEM traffic descriptor",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: TRAFFIC_DESCRIPTOR_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_MULTICAST_GEM_INTERWORKING_TP,
        name: "Multicast GEM interworking termination point",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_TABLE,
        attributes: MULTICAST_GEM_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_OMCI,
        name: "OMCI",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_GET | action_bit(ACTION_GET_NEXT),
        attributes: OMCI_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_ETHERNET_PM_HISTORY_DATA_3,
        name: "Ethernet PM history data 3",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_PM,
        attributes: ETHERNET_PM_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_MULTICAST_OPERATIONS_PROFILE,
        name: "Multicast operations profile",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_TABLE,
        attributes: MULTICAST_OPERATIONS_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_MULTICAST_SUBSCRIBER_CONFIG,
        name: "Multicast subscriber configuration info",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_RW,
        attributes: MULTICAST_SUBSCRIBER_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_MULTICAST_SUBSCRIBER_MONITOR,
        name: "Multicast subscriber monitor",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_OLT_TABLE,
        attributes: MULTICAST_MONITOR_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_FEC_PM_HISTORY_DATA,
        name: "FEC PM history data",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_PM,
        attributes: FEC_PM_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_VEIP,
        name: "Virtual Ethernet interface point",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: VEIP_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_ENHANCED_SECURITY_CONTROL,
        name: "Enhanced security control",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW | action_bit(ACTION_GET_NEXT),
        attributes: ENHANCED_SECURITY_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_ETHERNET_FRAME_EXTENDED_PM,
        name: "Ethernet frame extended PM",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_PM,
        attributes: ETHERNET_FRAME_EXTENDED_PM_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_VENDOR_351,
        name: "Vendor class 351",
        origin: ManagedEntityOrigin::Olt,
        actions: ACTIONS_PM,
        attributes: VENDOR_351_ATTRIBUTES,
    },
    ManagedEntityDefinition {
        class_id: CLASS_CTC_LOID_AUTH,
        name: "CTC LOID authentication",
        origin: ManagedEntityOrigin::Onu,
        actions: ACTIONS_ONU_RW,
        attributes: CTC_LOID_ATTRIBUTES,
    },
];

pub fn managed_entity(class_id: u16) -> Option<&'static ManagedEntityDefinition> {
    MANAGED_ENTITIES
        .binary_search_by_key(&class_id, |definition| definition.class_id)
        .ok()
        .map(|index| &MANAGED_ENTITIES[index])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn managed_entities_are_sorted_and_unique() {
        for pair in MANAGED_ENTITIES.windows(2) {
            assert!(pair[0].class_id < pair[1].class_id);
        }
    }

    #[test]
    fn attribute_definitions_are_wire_safe() {
        for definition in MANAGED_ENTITIES {
            let mut previous = 0;
            for attribute in definition.attributes {
                assert!((1..=16).contains(&attribute.index));
                assert!(attribute.index > previous);
                previous = attribute.index;
                match attribute.kind {
                    AttributeKind::Scalar { length } => {
                        assert!(length > 0);
                        assert!(attribute.default.is_empty() || attribute.default.len() == length);
                    }
                    AttributeKind::Table { row_length } => {
                        assert!(row_length > 0);
                        assert!(attribute.default.is_empty());
                    }
                }
            }
        }
    }
}
