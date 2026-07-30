//! Publishing finished analysis results to the UI without blocking either side.
//!
//! Three buffers. The producer always writes into one nobody is reading and then
//! atomically publishes it; the consumer atomically takes the most recent. Neither
//! ever waits, so the analysis thread cannot be stalled by a slow redraw and the
//! UI cannot observe a half-written frame.
//!
//! It also resolves a rate mismatch for free. Analysis produces a few hundred
//! frames a second while the display runs at 60–120 Hz, so most frames are
//! overwritten before anyone looks at them. That is the correct outcome: nobody
//! can see a spectrum update that was never drawn.

use std::fmt;

/// Producer end. Lives on the analysis thread.
pub struct SnapshotPublisher<T: Send> {
    input: triple_buffer::Input<T>,
}

impl<T: Send> SnapshotPublisher<T> {
    /// Mutate the pending buffer in place, then publish it.
    ///
    /// In place is the point. Publishing by value would move or clone the whole
    /// snapshot — and a snapshot holds `Vec`s of bins, so a clone would allocate
    /// on every frame.
    ///
    /// The buffer retains whatever was written two publishes ago, not the value
    /// most recently published, so `f` must overwrite every field it cares about
    /// rather than assuming a starting state.
    pub fn publish_with(&mut self, f: impl FnOnce(&mut T)) {
        f(self.input.input_buffer_mut());
        self.input.publish();
    }
}

impl<T: Send> fmt::Debug for SnapshotPublisher<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SnapshotPublisher").finish_non_exhaustive()
    }
}

/// Consumer end. Lives on the UI thread.
pub struct SnapshotReader<T: Send> {
    output: triple_buffer::Output<T>,
}

impl<T: Send> SnapshotReader<T> {
    /// The most recently published value, or the previous one if nothing new
    /// arrived. Never blocks.
    pub fn read(&mut self) -> &T {
        self.output.read()
    }

    /// Whether a new value is waiting, without consuming it.
    ///
    /// Useful for skipping a redraw entirely when nothing changed — which is how
    /// the idle-CPU target gets met, since a free-running timer redrawing
    /// identical data is the real battery cost.
    pub fn has_update(&self) -> bool {
        self.output.updated()
    }
}

impl<T: Send> fmt::Debug for SnapshotReader<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SnapshotReader").finish_non_exhaustive()
    }
}

/// Create a publisher/reader pair, with all three buffers initialised from
/// `initial`.
///
/// Allocation happens here, once, so publishing never does.
pub fn snapshot_channel<T: Clone + Send>(initial: T) -> (SnapshotPublisher<T>, SnapshotReader<T>) {
    let (input, output) = triple_buffer::triple_buffer(&initial);
    (SnapshotPublisher { input }, SnapshotReader { output })
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;

    #[derive(Debug, Clone, PartialEq, Default)]
    struct Frame {
        sequence: u64,
        bins: Vec<f32>,
    }

    #[test]
    fn reader_sees_the_initial_value_before_anything_is_published() {
        let (_publisher, mut reader) = snapshot_channel(Frame {
            sequence: 7,
            bins: vec![1.0, 2.0],
        });
        assert_eq!(reader.read().sequence, 7);
    }

    #[test]
    fn published_values_become_visible() {
        let (mut publisher, mut reader) = snapshot_channel(Frame::default());

        publisher.publish_with(|frame| {
            frame.sequence = 1;
            frame.bins.clear();
            frame.bins.extend_from_slice(&[0.5, 0.25]);
        });

        assert!(reader.has_update());
        let frame = reader.read();
        assert_eq!(frame.sequence, 1);
        assert_eq!(frame.bins, vec![0.5, 0.25]);
    }

    #[test]
    fn has_update_is_false_until_something_is_published() {
        let (mut publisher, mut reader) = snapshot_channel(Frame::default());
        assert!(!reader.has_update());

        publisher.publish_with(|frame| frame.sequence = 1);
        assert!(reader.has_update());

        reader.read();
        assert!(!reader.has_update(), "consumed update should clear");
    }

    /// The rate-mismatch behaviour: a fast producer must not queue up work for a
    /// slow consumer, it must overwrite.
    #[test]
    fn a_slow_reader_sees_only_the_newest_value() {
        let (mut publisher, mut reader) = snapshot_channel(Frame::default());

        for sequence in 1..=100 {
            publisher.publish_with(|frame| frame.sequence = sequence);
        }

        assert_eq!(reader.read().sequence, 100);
    }

    #[test]
    fn repeated_reads_without_a_publish_are_stable() {
        let (mut publisher, mut reader) = snapshot_channel(Frame::default());
        publisher.publish_with(|frame| frame.sequence = 42);

        assert_eq!(reader.read().sequence, 42);
        assert_eq!(reader.read().sequence, 42);
        assert_eq!(reader.read().sequence, 42);
    }

    /// Documents the buffer-recycling surprise: the pending buffer holds an older
    /// value, not the last published one. Code that assumes otherwise reads stale
    /// fields, so `publish_with` must overwrite everything it cares about.
    #[test]
    fn pending_buffer_is_recycled_not_freshly_copied() {
        let (mut publisher, mut reader) = snapshot_channel(Frame::default());

        publisher.publish_with(|frame| frame.sequence = 1);
        publisher.publish_with(|frame| frame.sequence = 2);
        publisher.publish_with(|frame| {
            // Whatever is here is a recycled buffer, and specifically not 2.
            assert_ne!(frame.sequence, 2, "must not be the value just published");
            frame.sequence = 3;
        });

        assert_eq!(reader.read().sequence, 3);
    }

    #[test]
    fn crosses_threads() {
        let (mut publisher, mut reader) = snapshot_channel(Frame::default());

        let writer = std::thread::spawn(move || {
            for sequence in 1..=1000 {
                publisher.publish_with(|frame| {
                    frame.sequence = sequence;
                    frame.bins.clear();
                    frame.bins.push(sequence as f32);
                });
            }
        });

        let mut last = 0;
        let mut observations = 0;
        while last < 1000 {
            let frame = reader.read();
            // Sequence must never go backwards, and the payload must always match
            // the sequence - that is what proves a torn read never happens.
            assert!(frame.sequence >= last, "sequence went backwards");
            if let Some(first) = frame.bins.first() {
                assert_eq!(*first, frame.sequence as f32, "torn snapshot observed");
            }
            last = frame.sequence;
            observations += 1;
            if observations > 10_000_000 {
                panic!("reader never saw the final value");
            }
        }

        writer.join().unwrap();
    }
}
