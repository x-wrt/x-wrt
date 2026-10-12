// SPDX-License-Identifier: GPL-2.0-only

//! Airoha PON userspace protocol implementation.
//!
//! `omci` and `oam` contain the ITU-T OMCI and IEEE 802.3ah/CTC engines.
//! `runtime` and `transport` provide their shared Linux integration.

pub mod agent;
pub mod config;
pub mod control;
pub mod oam;
pub mod omci;
pub mod runtime;
pub mod transport;
pub mod wire;
