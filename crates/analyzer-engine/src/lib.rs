//! Real-time safe analysis graph, lock-free buffering and snapshot publication.
//!
//! This crate owns the thread topology, which is where the performance comes
//! from. Each hand-off between threads is lock-free for a specific reason: the
//! audio thread cannot take a lock, because a lower-priority thread holding it
//! would invert priority and blow the deadline, and the UI must never be able to
//! stall analysis.

pub mod ring;
pub mod snapshot;

pub use ring::{CaptureSink, CaptureSource, capture_ring, deinterleave};
pub use snapshot::{SnapshotPublisher, SnapshotReader, snapshot_channel};
