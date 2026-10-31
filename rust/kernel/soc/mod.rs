// SPDX-License-Identifier: GPL-2.0

//! SoC drivers

pub mod apple;
#[cfg(CONFIG_SOC_BUS)]
pub mod bus;

#[cfg(CONFIG_SOC_BUS)]
pub use bus::{
    Attributes,
    Registration, //
};
