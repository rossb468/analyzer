//! Mechanical enforcement of the no-allocation rule.
//!
//! Every real-time audio codebase has the rule that the callback must not
//! allocate. Almost all of them enforce it by convention, code review and scar
//! tissue, which means violations ship and surface as a click roughly once an
//! hour on somebody else's machine. That bug class is close to undebuggable: it
//! does not reproduce, and for a measurement tool it does not even announce
//! itself — it silently corrupts the data.
//!
//! So it is enforced instead. [`rt_section`] wraps the callback body in a guard
//! that aborts the process on any allocation, and the same guard runs in CI.
//!
//! # Binaries must register the allocator
//!
//! The guard only works if the process installed the tracking allocator, which
//! only a binary can do:
//!
//! ```ignore
//! #[cfg(debug_assertions)]
//! #[global_allocator]
//! static ALLOC_TRAP: analyzer_engine::rt::AllocTrap = analyzer_engine::rt::AllocTrap;
//! ```
//!
//! Without it [`rt_section`] degrades to calling the closure directly. That
//! degradation is silent by design — a library cannot force a binary's hand — so
//! every binary in this workspace registers it.

/// The tracking allocator. Register it with `#[global_allocator]` in a binary.
pub use assert_no_alloc::AllocDisabler as AllocTrap;

/// Run `f` with allocation forbidden.
///
/// Wrap the body of an audio callback in this. In release builds without the
/// relevant feature flags the guard compiles away, so there is no cost in
/// shipped code — the point is to fail loudly in development and CI.
pub fn rt_section<R>(f: impl FnOnce() -> R) -> R {
    assert_no_alloc::assert_no_alloc(f)
}

/// Permit allocation inside an [`rt_section`].
///
/// An escape hatch for the rare genuinely-safe case, and for diagnostics while
/// tracking a violation down. Reaching for this in the audio path is a design
/// smell: if something on that path needs to allocate, the allocation belongs
/// somewhere else, not behind a waiver.
pub fn permit_alloc<R>(f: impl FnOnce() -> R) -> R {
    assert_no_alloc::permit_alloc(f)
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    const CHILD_MARKER: &str = "ANALYZER_ALLOC_TRAP_CHILD";

    #[test]
    fn rt_section_passes_values_through() {
        let sum = rt_section(|| (1..=10).sum::<u32>());
        assert_eq!(sum, 55);
    }

    #[test]
    fn preallocated_buffers_are_fine_to_use() {
        // The realistic shape: allocate up front, then only write.
        let mut buffer = vec![0.0_f32; 128];
        rt_section(|| {
            for (n, slot) in buffer.iter_mut().enumerate() {
                *slot = n as f32;
            }
        });
        assert_eq!(buffer.last(), Some(&127.0));
    }

    #[test]
    fn permit_alloc_opens_a_hole_in_the_guard() {
        let value = rt_section(|| permit_alloc(|| vec![1_u8, 2, 3].len()));
        assert_eq!(value, 3);
    }

    /// Proves the trap actually fires, which is the whole point of the module.
    ///
    /// A violation aborts the process, so it cannot be caught in-process with
    /// `#[should_panic]`. Instead this re-executes the test binary with a marker
    /// variable set; the child allocates inside a section and must die.
    #[test]
    fn allocating_inside_an_rt_section_aborts_the_process() {
        if std::env::var(CHILD_MARKER).is_ok() {
            rt_section(|| {
                let doomed: Vec<u8> = Vec::with_capacity(64);
                std::hint::black_box(doomed);
            });
            // Reaching here means the guard did not fire. Exit successfully so
            // the parent's assertion fails and says so.
            std::process::exit(0);
        }

        let exe = std::env::current_exe().unwrap();
        let status = std::process::Command::new(exe)
            .args([
                "rt::tests::allocating_inside_an_rt_section_aborts_the_process",
                "--exact",
                "--quiet",
            ])
            .env(CHILD_MARKER, "1")
            .output()
            .unwrap()
            .status;

        assert!(
            !status.success(),
            "allocation inside rt_section should have aborted, but the child exited cleanly"
        );
    }
}
