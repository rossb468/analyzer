//! The lock-free crossing from the audio thread to the analysis thread.
//!
//! # Why interleaved, and why all-or-nothing
//!
//! One ring carries interleaved frames rather than one ring per channel. That is
//! a correctness decision, not a convenience: with separate rings a partial write
//! could advance one channel and not another, and channels that drift apart by
//! even one sample destroy a transfer-function phase reading. Interleaving makes
//! frame alignment structural.
//!
//! For the same reason writes are all-or-nothing. If a whole block will not fit,
//! the block is dropped and an overrun is counted. A half-written block would
//! desynchronise every channel after it.
//!
//! # Overruns are not silent
//!
//! The producer cannot block — it is on a hard deadline — and it cannot allocate.
//! Dropping is the only option left. But a measurement taken across dropped audio
//! is wrong rather than merely degraded, so the count is published and the UI is
//! expected to surface it. Silent data loss is the worst failure mode a
//! measurement tool has.

#![deny(clippy::indexing_slicing)]

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use rtrb::RingBuffer;

/// Audio-thread end of the ring.
///
/// Holds the only producer. `rtrb` makes this `Send` but not `Clone`, so a second
/// producer is a compile error rather than a documented precondition — the
/// single-producer requirement is enforced by the type system.
#[derive(Debug)]
pub struct CaptureSink {
    producer: rtrb::Producer<f32>,
    channels: usize,
    overruns: Arc<AtomicU64>,
}

impl CaptureSink {
    /// Push one block of interleaved frames.
    ///
    /// Returns `true` if the block was accepted. On `false` the block was dropped
    /// whole and [`CaptureSink::overruns`] has advanced.
    ///
    /// Real-time safe: no allocation, no locks, one atomic store to publish.
    pub fn write_interleaved(&mut self, samples: &[f32]) -> bool {
        if samples.is_empty() {
            return true;
        }
        // A partial frame means the caller's channel count is wrong. Refuse it
        // rather than corrupting alignment for everything that follows.
        if !samples.len().is_multiple_of(self.channels) {
            self.overruns.fetch_add(1, Ordering::Relaxed);
            return false;
        }
        if self.producer.slots() < samples.len() {
            self.overruns.fetch_add(1, Ordering::Relaxed);
            return false;
        }

        match self.producer.write_chunk_uninit(samples.len()) {
            Ok(chunk) => {
                let written = chunk.fill_from_iter(samples.iter().copied());
                debug_assert_eq!(written, samples.len(), "slots() promised the space");
                true
            }
            Err(_) => {
                self.overruns.fetch_add(1, Ordering::Relaxed);
                false
            }
        }
    }

    /// Blocks dropped because the analysis thread fell behind.
    pub fn overruns(&self) -> u64 {
        self.overruns.load(Ordering::Relaxed)
    }

    /// Channels per frame.
    pub fn channels(&self) -> usize {
        self.channels
    }

    /// Frames that would currently fit.
    pub fn frames_free(&self) -> usize {
        self.producer.slots() / self.channels
    }
}

/// Analysis-thread end of the ring.
#[derive(Debug)]
pub struct CaptureSource {
    consumer: rtrb::Consumer<f32>,
    channels: usize,
    overruns: Arc<AtomicU64>,
}

impl CaptureSource {
    /// Read whole frames into `dst`, interleaved.
    ///
    /// Reads `min(available, dst.len() / channels)` frames and returns that
    /// count. Never partial frames.
    pub fn read_interleaved(&mut self, dst: &mut [f32]) -> usize {
        let capacity_frames = dst.len() / self.channels;
        let frames = capacity_frames.min(self.frames_available());
        if frames == 0 {
            return 0;
        }
        let samples = frames * self.channels;

        match self.consumer.read_chunk(samples) {
            Ok(chunk) => {
                let (first, second) = chunk.as_slices();
                // The chunk wraps the end of the buffer, so it arrives in two
                // pieces that have to be copied separately.
                if let Some(head) = dst.get_mut(..first.len()) {
                    head.copy_from_slice(first);
                }
                if let Some(tail) = dst.get_mut(first.len()..first.len() + second.len()) {
                    tail.copy_from_slice(second);
                }
                chunk.commit_all();
                frames
            }
            Err(_) => 0,
        }
    }

    /// Whole frames currently readable.
    pub fn frames_available(&self) -> usize {
        self.consumer.slots() / self.channels
    }

    /// Blocks the producer had to drop.
    pub fn overruns(&self) -> u64 {
        self.overruns.load(Ordering::Relaxed)
    }

    /// Channels per frame.
    pub fn channels(&self) -> usize {
        self.channels
    }
}

/// Create a capture ring sized for `capacity_frames`.
///
/// # Sizing
///
/// Capacity is dictated by worst-case scheduling latency, not by throughput. The
/// analysis thread consumes faster than the audio thread produces on average, so
/// the only reason to be large is to absorb the analysis thread being
/// descheduled. Roughly 8192 frames is ~170 ms of runway at 48 kHz and costs
/// 32 KB per channel — memory is not the constraint here, overruns are.
///
/// # Panics
///
/// Panics if `channels` or `capacity_frames` is zero.
pub fn capture_ring(channels: usize, capacity_frames: usize) -> (CaptureSink, CaptureSource) {
    assert!(channels > 0, "ring must carry at least one channel");
    assert!(capacity_frames > 0, "ring must hold at least one frame");

    let (producer, consumer) = RingBuffer::new(channels * capacity_frames);
    let overruns = Arc::new(AtomicU64::new(0));

    (
        CaptureSink {
            producer,
            channels,
            overruns: Arc::clone(&overruns),
        },
        CaptureSource {
            consumer,
            channels,
            overruns,
        },
    )
}

/// Split interleaved frames into per-channel slices.
///
/// Allocation-free; `channels` must already be sized. Extra samples in `dst`
/// beyond the available frames are left untouched.
///
/// # Panics
///
/// Panics if `src` is not a whole number of frames for `dst.len()` channels.
pub fn deinterleave(src: &[f32], dst: &mut [&mut [f32]]) {
    let channel_count = dst.len();
    assert!(channel_count > 0, "need at least one destination channel");
    assert!(
        src.len().is_multiple_of(channel_count),
        "source must be a whole number of frames"
    );

    let frames = src.len() / channel_count;
    for (channel, out) in dst.iter_mut().enumerate() {
        let take = frames.min(out.len());
        for frame in 0..take {
            if let (Some(slot), Some(sample)) =
                (out.get_mut(frame), src.get(frame * channel_count + channel))
            {
                *slot = *sample;
            }
        }
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_interleaved_frames() {
        let (mut sink, mut source) = capture_ring(2, 16);
        let block = [1.0, -1.0, 2.0, -2.0];

        assert!(sink.write_interleaved(&block));
        assert_eq!(source.frames_available(), 2);

        let mut out = [0.0; 4];
        assert_eq!(source.read_interleaved(&mut out), 2);
        assert_eq!(out, block);
        assert_eq!(source.frames_available(), 0);
    }

    #[test]
    fn full_ring_drops_whole_blocks_and_counts_them() {
        let (mut sink, source) = capture_ring(1, 4);

        assert!(sink.write_interleaved(&[1.0, 2.0, 3.0, 4.0]));
        assert_eq!(sink.overruns(), 0);

        // No room: the block must be refused entirely.
        assert!(!sink.write_interleaved(&[5.0, 6.0]));
        assert_eq!(sink.overruns(), 1);
        assert_eq!(source.overruns(), 1, "count is visible from both ends");
    }

    /// The property that matters: a refused write must not leave a partial block
    /// behind, because everything after it would be shifted by a channel.
    #[test]
    fn refused_write_leaves_no_partial_data() {
        let (mut sink, mut source) = capture_ring(2, 2);
        assert!(sink.write_interleaved(&[1.0, 2.0]));

        // Two frames wanted, one frame of room.
        assert!(!sink.write_interleaved(&[3.0, 4.0, 5.0, 6.0]));

        let mut out = [0.0; 4];
        assert_eq!(source.read_interleaved(&mut out), 1);
        assert_eq!(&out[..2], &[1.0, 2.0], "only the accepted frame is present");
    }

    #[test]
    fn partial_frame_write_is_refused() {
        let (mut sink, _source) = capture_ring(2, 16);
        // Three samples is one and a half frames.
        assert!(!sink.write_interleaved(&[1.0, 2.0, 3.0]));
        assert_eq!(sink.overruns(), 1);
    }

    #[test]
    fn empty_write_succeeds_without_counting_an_overrun() {
        let (mut sink, _source) = capture_ring(2, 4);
        assert!(sink.write_interleaved(&[]));
        assert_eq!(sink.overruns(), 0);
    }

    #[test]
    fn read_returns_only_whole_frames() {
        let (mut sink, mut source) = capture_ring(3, 8);
        sink.write_interleaved(&[1.0, 2.0, 3.0, 4.0, 5.0, 6.0]);

        // Room for one frame and a bit: only one frame comes out.
        let mut out = [0.0; 5];
        assert_eq!(source.read_interleaved(&mut out), 1);
        assert_eq!(&out[..3], &[1.0, 2.0, 3.0]);
        assert_eq!(source.frames_available(), 1);
    }

    #[test]
    fn read_from_empty_ring_yields_nothing() {
        let (_sink, mut source) = capture_ring(2, 4);
        let mut out = [0.0; 4];
        assert_eq!(source.read_interleaved(&mut out), 0);
    }

    /// Exercises the wrap-around path, where rtrb hands back two slices.
    #[test]
    fn survives_wrapping_the_buffer_many_times() {
        let (mut sink, mut source) = capture_ring(2, 3);
        let mut expected = 0.0_f32;
        let mut out = [0.0; 4];

        for _ in 0..50 {
            let block = [expected, expected + 0.5];
            assert!(sink.write_interleaved(&block), "should always have room");
            assert_eq!(source.read_interleaved(&mut out), 1);
            assert_eq!(out[0], expected);
            assert_eq!(out[1], expected + 0.5);
            expected += 1.0;
        }
        assert_eq!(sink.overruns(), 0);
    }

    #[test]
    fn free_and_available_frames_track_each_other() {
        let (mut sink, mut source) = capture_ring(2, 8);
        assert_eq!(sink.frames_free(), 8);
        assert_eq!(source.frames_available(), 0);

        sink.write_interleaved(&[0.0; 6]);
        assert_eq!(sink.frames_free(), 5);
        assert_eq!(source.frames_available(), 3);

        let mut out = [0.0; 6];
        source.read_interleaved(&mut out);
        assert_eq!(source.frames_available(), 0);
        assert_eq!(sink.frames_free(), 8);
    }

    #[test]
    fn deinterleave_splits_channels() {
        let src = [1.0, -1.0, 2.0, -2.0, 3.0, -3.0];
        let mut left = [0.0; 3];
        let mut right = [0.0; 3];
        deinterleave(&src, &mut [&mut left, &mut right]);
        assert_eq!(left, [1.0, 2.0, 3.0]);
        assert_eq!(right, [-1.0, -2.0, -3.0]);
    }

    #[test]
    fn deinterleave_stops_at_the_shorter_destination() {
        let src = [1.0, -1.0, 2.0, -2.0];
        let mut left = [0.0; 1];
        let mut right = [0.0; 1];
        deinterleave(&src, &mut [&mut left, &mut right]);
        assert_eq!(left, [1.0]);
        assert_eq!(right, [-1.0]);
    }

    #[test]
    #[should_panic(expected = "whole number of frames")]
    fn deinterleave_rejects_ragged_input() {
        let src = [1.0, 2.0, 3.0];
        let mut a = [0.0; 2];
        let mut b = [0.0; 2];
        deinterleave(&src, &mut [&mut a, &mut b]);
    }

    #[test]
    #[should_panic(expected = "at least one channel")]
    fn zero_channels_is_rejected() {
        let _ = capture_ring(0, 16);
    }

    #[test]
    fn crossing_threads_preserves_order() {
        let (mut sink, mut source) = capture_ring(1, 1024);

        let producer = std::thread::spawn(move || {
            for n in 0..500 {
                while !sink.write_interleaved(&[n as f32]) {
                    std::thread::yield_now();
                }
            }
            sink.overruns()
        });

        let mut received = Vec::new();
        let mut out = [0.0; 64];
        while received.len() < 500 {
            let frames = source.read_interleaved(&mut out);
            received.extend_from_slice(&out[..frames]);
            if frames == 0 {
                std::thread::yield_now();
            }
        }

        assert_eq!(producer.join().unwrap(), 0);
        let expected: Vec<f32> = (0..500).map(|n| n as f32).collect();
        assert_eq!(received, expected);
    }
}
