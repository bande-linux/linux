// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DMA-buf subsystem abstractions.

mod buf;
pub mod dma_fence;

pub use self::buf::DmaBuf;

pub use self::dma_fence::{
    DriverFence,
    Fence,
    FenceCallback,
    FenceCallbackRegistration,
    FenceContext,
    FenceContextOps, //
};
