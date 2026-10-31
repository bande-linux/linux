// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Support for Apple RTKit coprocessors.
//!
//! C header: [`include/linux/soc/apple/rtkit.h`](../../../../include/linux/gpio/driver.h)

use crate::{
    bindings,
    device,
    error::{
        from_err_ptr,
        from_result,
        to_result, //
    },
    io::{
        register,
        Io,
        IoSysMap,
        Region, //
    },
    prelude::*,
    str::CStrExt,
};

use core::marker::PhantomPinned;
use core::ptr;
use core::sync::atomic::{
    AtomicPtr,
    Ordering, //
};
use macros::vtable;

/// Size of the ASC control MMIO region.
pub const ASC_CTL_SIZE: usize = 0x4000;

register! {
    base: Region<ASC_CTL_SIZE>;

    /// ASC CPU control register
    pub ASC_CPU_CONTROL(u32) @ 0x44 {
        /// CPU run bit.
        4:4 cpu_run;
    }
}

/// Trait to represent allocatable buffers for the RTKit core.
///
/// Users must implement this trait for their own representation of those allocations.
pub trait Buffer {
    /// Returns the IOVA (virtual address) of the buffer from RTKit's point of view, or an error if
    /// unavailable.
    fn iova(&self) -> Result<usize>;

    /// Returns a mutable byte slice of the buffer contents, or an
    /// error if unavailable.
    fn buf(&mut self) -> Result<IoSysMap<'_, Region>>;
}

/// Callback operations for an RTKit client.
#[vtable]
pub trait Operations: Sized {
    /// Arbitrary user context type.
    type Data: Send + Sync;

    /// Type representing an allocated buffer for RTKit.
    type Buffer: Buffer;

    /// Called when RTKit crashes.
    fn crashed(_rtk: &RtKit<Self>, _crashlog: Option<&[u8]>) {}

    /// Called when a message was received on a non-system endpoint. Called in non-IRQ context.
    fn recv_message(
        _rtk: &RtKit<Self>,
        _endpoint: u8,
        _message: u64,
    ) {
    }

    /// Called in IRQ context when a message was received on a non-system endpoint.
    ///
    /// Must return `true` if the message is handled, or `false` to process it in
    /// the handling thread.
    fn recv_message_early(
        _rtk: &RtKit<Self>,
        _endpoint: u8,
        _message: u64,
    ) -> bool {
        false
    }

    /// Allocate a buffer for use by RTKit.
    fn shmem_alloc(
        _rtk: &RtKit<Self>,
        _size: usize,
    ) -> Result<Self::Buffer> {
        Err(EINVAL)
    }

    /// Map an existing buffer used by RTKit at a device-specified virtual address.
    fn shmem_map(
        _rtk: &RtKit<Self>,
        _iova: usize,
        _size: usize,
    ) -> Result<Self::Buffer> {
        Err(EINVAL)
    }
}

/// Represents `struct apple_rtkit *`.
///
/// # Invariants
///
/// Once [`RtKit::new`] has completed, `rtk` is a valid client pointer that does not change
/// until the client is freed on drop. While [`RtKit::new`] runs, callbacks may already see
/// `rtk` as null.
#[pin_data(PinnedDrop)]
pub struct RtKit<T: Operations> {
    // `apple_rtkit_init()` starts the mailbox before it returns, so callbacks can run before the
    // client pointer is known. It is published once `apple_rtkit_init()` succeeds.
    rtk: AtomicPtr<bindings::apple_rtkit>,
    #[pin]
    data: T::Data,
    #[pin]
    _pin: PhantomPinned,
}

unsafe extern "C" fn crashed_callback<T: Operations>(
    cookie: *mut core::ffi::c_void,
    crashlog: *const core::ffi::c_void,
    crashlog_size: usize,
) {
    let crashlog = if !crashlog.is_null() && crashlog_size > 0 {
        // SAFETY: The crashlog is either missing or a byte buffer of the specified size
        Some(unsafe { core::slice::from_raw_parts(crashlog as *const u8, crashlog_size) })
    } else {
        None
    };
    // SAFETY: The cookie is the pinned `RtKit<T>` that registered this callback.
    T::crashed(unsafe { &*cookie.cast::<RtKit<T>>() }, crashlog);
}

unsafe extern "C" fn recv_message_callback<T: Operations>(
    cookie: *mut core::ffi::c_void,
    endpoint: u8,
    message: u64,
) {
    // SAFETY: The cookie is the pinned `RtKit<T>` that registered this callback.
    T::recv_message(unsafe { &*cookie.cast::<RtKit<T>>() }, endpoint, message);
}

unsafe extern "C" fn recv_message_early_callback<T: Operations>(
    cookie: *mut core::ffi::c_void,
    endpoint: u8,
    message: u64,
) -> bool {
    // SAFETY: The cookie is the pinned `RtKit<T>` that registered this callback.
    T::recv_message_early(unsafe { &*cookie.cast::<RtKit<T>>() }, endpoint, message)
}

unsafe extern "C" fn shmem_setup_callback<T: Operations>(
    cookie: *mut core::ffi::c_void,
    bfr: *mut bindings::apple_rtkit_shmem,
) -> core::ffi::c_int {
    // SAFETY: `bfr` is a valid buffer
    let bfr_mut = unsafe { &mut *bfr };

    from_result(|| {
        let mut buf = if bfr_mut.iova != 0 {
            bfr_mut.is_mapped = true;
            T::shmem_map(
                // SAFETY: The cookie is the pinned `RtKit<T>` that registered this callback.
                unsafe { &*cookie.cast::<RtKit<T>>() },
                bfr_mut.iova as usize,
                bfr_mut.size,
            )?
        } else {
            bfr_mut.is_mapped = false;
            // SAFETY: The cookie is the pinned `RtKit<T>` that registered this callback.
            T::shmem_alloc(unsafe { &*cookie.cast::<RtKit<T>>() }, bfr_mut.size)?
        };

        let iova = buf.iova()?;
        let IoSysMap::Sys(sysmem) = buf.buf()? else {
            return Err(EINVAL);
        };

        if sysmem.size() < bfr_mut.size {
            return Err(ENOMEM);
        }

        bfr_mut.iova = iova as u64;
        bfr_mut.buffer = sysmem.as_ptr() as *mut _;

        // Now box the returned buffer type and stash it in the private pointer of the
        // `apple_rtkit_shmem` struct for safekeeping.
        let boxed = KBox::new(buf, GFP_KERNEL)?;
        bfr_mut.private = KBox::into_raw(boxed) as *mut _;
        Ok(0)
    })
}

unsafe extern "C" fn shmem_destroy_callback<T: Operations>(
    _cookie: *mut core::ffi::c_void,
    bfr: *mut bindings::apple_rtkit_shmem,
) {
    // SAFETY: `bfr` is a valid buffer
    let bfr_mut = unsafe { &mut *bfr };
    if !bfr_mut.private.is_null() {
        // SAFETY: Per shmem_setup_callback, this has to be a pointer to a Buffer if it is set.
        unsafe {
            core::mem::drop(KBox::from_raw(bfr_mut.private as *mut T::Buffer));
        }
        bfr_mut.private = core::ptr::null_mut();
    }
}

impl<T: Operations> RtKit<T> {
    const VTABLE: bindings::apple_rtkit_ops = bindings::apple_rtkit_ops {
        crashed: Some(crashed_callback::<T>),
        recv_message: Some(recv_message_callback::<T>),
        recv_message_early: Some(recv_message_early_callback::<T>),
        shmem_setup: if T::HAS_SHMEM_ALLOC || T::HAS_SHMEM_MAP {
            Some(shmem_setup_callback::<T>)
        } else {
            None
        },
        shmem_destroy: if T::HAS_SHMEM_ALLOC || T::HAS_SHMEM_MAP {
            Some(shmem_destroy_callback::<T>)
        } else {
            None
        },
    };

    /// Creates a new RTKit client for a given device and optional mailbox name or index.
    ///
    /// The client owns `data`; the callbacks of `T` receive the client and reach `data`
    /// through [`RtKit::data`].
    pub fn new<'a>(
        dev: &'a device::Device,
        mbox_name: Option<&'static CStr>,
        mbox_idx: usize,
        data: impl PinInit<T::Data, Error> + 'a,
    ) -> impl PinInit<Self, Error> + 'a
    where
        T: 'a,
    {
        try_pin_init!(&this in Self {
            rtk: AtomicPtr::new(ptr::null_mut()),
            data <- data,
            _pin: {
                let mbox_idx = mbox_idx.try_into()?;
                // SAFETY: `dev` is valid. The cookie is the pinned address of `Self`, which
                // outlives the C client: `PinnedDrop` frees the client before `data` is dropped.
                // All fields with storage (`rtk` and `data`) are initialized, so callbacks can
                // reference `Self` as soon as the mailbox starts.
                let client = unsafe {
                    from_err_ptr(bindings::apple_rtkit_init(
                        dev.as_raw(),
                        this.as_ptr().cast(),
                        match mbox_name {
                            Some(s) => s.as_char_ptr(),
                            None => ptr::null(),
                        },
                        mbox_idx,
                        &Self::VTABLE,
                    ))
                }?;
                // SAFETY: The `rtk` field was initialized above and `this` is valid for the
                // duration of the initializer.
                unsafe { (*this.as_ptr()).rtk.store(client, Ordering::Release) };
                PhantomPinned
            },
        }? Error)
    }

    /// Returns the data owned by this client.
    pub fn data(&self) -> &T::Data {
        &self.data
    }

    /// Returns the C client, or `EBUSY` if a callback runs before [`RtKit::new`] has completed.
    fn raw(&self) -> Result<*mut bindings::apple_rtkit> {
        let rtk = self.rtk.load(Ordering::Acquire);
        if rtk.is_null() {
            Err(EBUSY)
        } else {
            Ok(rtk)
        }
    }

    /// Boots (wakes up) the RTKit coprocessor.
    pub fn wake(&self) -> Result {
        let rtk = self.raw()?;
        // SAFETY: `rtk` is non-null, so valid per the type invariant.
        to_result(unsafe { bindings::apple_rtkit_wake(rtk) })
    }

    /// Waits for the RTKit coprocessor to finish booting.
    pub fn boot(&self) -> Result {
        let rtk = self.raw()?;
        // SAFETY: `rtk` is non-null, so valid per the type invariant.
        to_result(unsafe { bindings::apple_rtkit_boot(rtk) })
    }

    /// Starts a non-system endpoint.
    pub fn start_endpoint(&self, endpoint: u8) -> Result {
        let rtk = self.raw()?;
        // SAFETY: `rtk` is non-null, so valid per the type invariant.
        to_result(unsafe { bindings::apple_rtkit_start_ep(rtk, endpoint) })
    }

    /// Sends a message to a given endpoint.
    pub fn send_message(&self, endpoint: u8, message: u64) -> Result {
        let rtk = self.raw()?;
        // SAFETY: `rtk` is non-null, so valid per the type invariant.
        to_result(unsafe {
            bindings::apple_rtkit_send_message(rtk, endpoint, message, ptr::null_mut(), false)
        })
    }
}

// SAFETY: The C client serialises messages through its mailbox, and `T::Data` is `Sync`.
unsafe impl<T: Operations> Sync for RtKit<T> {}

// SAFETY: The C client is not tied to a thread, and `T::Data` is `Send`.
unsafe impl<T: Operations> Send for RtKit<T> {}

#[pinned_drop]
impl<T: Operations> PinnedDrop for RtKit<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: `new` has completed, so the pointer is valid by the type invariant. After this
        // returns no callback runs, so `data` can be dropped.
        unsafe { bindings::apple_rtkit_free(self.rtk.load(Ordering::Acquire)) };
    }
}
