//! Real-time safe analysis graph, lock-free buffering and snapshot publication.
//!
//! This crate owns the thread topology, which is where the performance actually
//! comes from:
//!
//! ```text
//!   audio thread          ring          analysis thread     triple buffer     UI thread
//!   ────────────                        ───────────────                       ─────────
//!   hard deadline    ──▶  [ring]  ──▶   heavy FFT work  ──▶  [snapshot]  ──▶  draws
//!   deinterleave                        no deadline,          newest wins      never
//!   and return                          must keep up                          blocks
//! ```
//!
//! Each hand-off is lock-free for a specific reason. The audio thread cannot take
//! a lock, because a lower-priority thread holding it would invert priority and
//! blow the deadline. The UI must never be able to stall analysis. And nothing
//! anywhere may allocate on the audio side.
//!
//! - [`ring`] carries interleaved frames from the callback, all-or-nothing, with
//!   dropped blocks counted rather than hidden.
//! - [`snapshot`] publishes finished results to the UI, newest-wins.
//! - [`rt`] enforces allocation-freedom instead of trusting it.

pub mod engine;
pub mod ring;
pub mod rt;
pub mod snapshot;

pub use engine::{AnalysisMode, Engine, EngineConfig, SpectrumFrame};
pub use ring::{CaptureSink, CaptureSource, capture_ring, deinterleave};
pub use rt::{AllocTrap, permit_alloc, rt_section};
pub use snapshot::{SnapshotPublisher, SnapshotReader, snapshot_channel};

// The guard in `rt` is inert unless the process registers the tracking
// allocator, and only a binary can do that. Registering it for this crate's own
// test binary is what lets the trap be tested at all.
#[cfg(test)]
#[global_allocator]
static ALLOC_TRAP: AllocTrap = AllocTrap;
