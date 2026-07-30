//! An in-memory collection of measurements.
//!
//! Deliberately a plain owned collection rather than anything clever. Sessions
//! hold tens of measurements, not millions, and the operations that matter are
//! add, remove, rename and iterate in a stable order. A user who reorders their
//! measurement list and finds it reshuffled on reload will not trust the tool
//! with anything else, so insertion order is preserved rather than being an
//! accident of a hash map.

use crate::measurement::{Measurement, MeasurementId};

/// A session's measurements, in the order the user sees them.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct MeasurementStore {
    measurements: Vec<Measurement>,
    next_id: u64,
}

impl MeasurementStore {
    /// An empty store.
    pub fn new() -> Self {
        Self {
            measurements: Vec::new(),
            next_id: 1,
        }
    }

    /// Add a measurement, assigning it a fresh identifier.
    ///
    /// The caller's `id` is overwritten. Identifiers are the store's to hand
    /// out; letting a caller choose invites two measurements sharing one.
    pub fn add(&mut self, mut measurement: Measurement) -> MeasurementId {
        let id = MeasurementId(self.next_id);
        self.next_id += 1;
        measurement.id = id;
        self.measurements.push(measurement);
        id
    }

    /// Add a measurement loaded from a file, keeping its stored identifier
    /// unless that would collide.
    ///
    /// Loading should preserve identity where it can, since notes and cross
    /// references may point at it — but never at the cost of two measurements
    /// answering to the same id.
    pub fn insert_loaded(&mut self, measurement: Measurement) -> MeasurementId {
        if measurement.id.0 == 0 || self.contains(measurement.id) {
            return self.add(measurement);
        }
        let id = measurement.id;
        self.next_id = self.next_id.max(id.0 + 1);
        self.measurements.push(measurement);
        id
    }

    /// Whether an identifier is in use.
    pub fn contains(&self, id: MeasurementId) -> bool {
        self.measurements.iter().any(|m| m.id == id)
    }

    /// Borrow a measurement.
    pub fn get(&self, id: MeasurementId) -> Option<&Measurement> {
        self.measurements.iter().find(|m| m.id == id)
    }

    /// Borrow a measurement mutably.
    pub fn get_mut(&mut self, id: MeasurementId) -> Option<&mut Measurement> {
        self.measurements.iter_mut().find(|m| m.id == id)
    }

    /// Remove a measurement, returning it.
    pub fn remove(&mut self, id: MeasurementId) -> Option<Measurement> {
        let index = self.measurements.iter().position(|m| m.id == id)?;
        Some(self.measurements.remove(index))
    }

    /// Move a measurement to a new position, for drag-to-reorder.
    ///
    /// Returns false if the identifier is unknown. The target is clamped, so a
    /// drag past the end lands at the end rather than failing.
    pub fn reorder(&mut self, id: MeasurementId, to: usize) -> bool {
        let Some(from) = self.measurements.iter().position(|m| m.id == id) else {
            return false;
        };
        let to = to.min(self.measurements.len().saturating_sub(1));
        if from == to {
            return true;
        }
        let measurement = self.measurements.remove(from);
        self.measurements.insert(to, measurement);
        true
    }

    /// Measurements in display order.
    pub fn iter(&self) -> impl Iterator<Item = &Measurement> {
        self.measurements.iter()
    }

    /// How many measurements there are.
    pub fn len(&self) -> usize {
        self.measurements.len()
    }

    /// Whether the store is empty.
    pub fn is_empty(&self) -> bool {
        self.measurements.is_empty()
    }

    /// Remove everything.
    pub fn clear(&mut self) {
        self.measurements.clear();
        self.next_id = 1;
    }

    /// A name not already taken, derived from `base`.
    ///
    /// Duplicate names are not an error — two measurements of the same speaker
    /// legitimately share one — but offering "Left (2)" by default saves the
    /// user from a list of identical rows.
    pub fn unique_name(&self, base: &str) -> String {
        if !self.measurements.iter().any(|m| m.name == base) {
            return base.to_owned();
        }
        for suffix in 2..1000 {
            let candidate = format!("{base} ({suffix})");
            if !self.measurements.iter().any(|m| m.name == candidate) {
                return candidate;
            }
        }
        base.to_owned()
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::measurement::MeasurementData;

    fn measurement(name: &str) -> Measurement {
        Measurement::new(
            MeasurementId(0),
            name,
            48_000.0,
            MeasurementData::ImpulseResponse {
                samples: vec![1.0, 2.0, 3.0],
                time_zero_samples: 0.0,
            },
        )
    }

    #[test]
    fn added_measurements_get_unique_identifiers() {
        let mut store = MeasurementStore::new();
        let a = store.add(measurement("a"));
        let b = store.add(measurement("b"));
        assert_ne!(a, b);
        assert_eq!(store.get(a).unwrap().name, "a");
        assert_eq!(store.get(b).unwrap().name, "b");
    }

    /// Reusing an id after a removal would let a stale reference resolve to the
    /// wrong measurement.
    #[test]
    fn identifiers_are_not_reused_after_removal() {
        let mut store = MeasurementStore::new();
        let a = store.add(measurement("a"));
        store.remove(a);
        let b = store.add(measurement("b"));
        assert_ne!(a, b, "an identifier must not be handed out twice");
    }

    #[test]
    fn insertion_order_is_preserved() {
        let mut store = MeasurementStore::new();
        for name in ["first", "second", "third"] {
            store.add(measurement(name));
        }
        let names: Vec<&str> = store.iter().map(|m| m.name.as_str()).collect();
        assert_eq!(names, vec!["first", "second", "third"]);
    }

    #[test]
    fn reorder_moves_a_measurement() {
        let mut store = MeasurementStore::new();
        store.add(measurement("a"));
        let b = store.add(measurement("b"));
        store.add(measurement("c"));

        assert!(store.reorder(b, 0));
        let names: Vec<&str> = store.iter().map(|m| m.name.as_str()).collect();
        assert_eq!(names, vec!["b", "a", "c"]);
    }

    #[test]
    fn reorder_clamps_past_the_end_and_rejects_unknown_ids() {
        let mut store = MeasurementStore::new();
        let a = store.add(measurement("a"));
        store.add(measurement("b"));

        assert!(store.reorder(a, 99));
        let names: Vec<&str> = store.iter().map(|m| m.name.as_str()).collect();
        assert_eq!(names, vec!["b", "a"]);

        assert!(!store.reorder(MeasurementId(999), 0));
    }

    #[test]
    fn a_loaded_measurement_keeps_its_identifier() {
        let mut store = MeasurementStore::new();
        let mut loaded = measurement("from disk");
        loaded.id = MeasurementId(42);

        let id = store.insert_loaded(loaded);
        assert_eq!(id, MeasurementId(42));
        // And the next fresh id must not collide with it.
        assert!(store.add(measurement("new")).0 > 42);
    }

    #[test]
    fn a_colliding_loaded_identifier_is_reassigned() {
        let mut store = MeasurementStore::new();
        let existing = store.add(measurement("existing"));

        let mut loaded = measurement("loaded");
        loaded.id = existing;
        let id = store.insert_loaded(loaded);

        assert_ne!(id, existing, "two measurements must not share an id");
        assert_eq!(store.len(), 2);
    }

    #[test]
    fn unique_name_suffixes_only_when_needed() {
        let mut store = MeasurementStore::new();
        assert_eq!(store.unique_name("Left"), "Left");

        store.add(measurement("Left"));
        assert_eq!(store.unique_name("Left"), "Left (2)");

        store.add(measurement("Left (2)"));
        assert_eq!(store.unique_name("Left"), "Left (3)");
        assert_eq!(store.unique_name("Right"), "Right");
    }

    #[test]
    fn mutation_through_the_store_sticks() {
        let mut store = MeasurementStore::new();
        let id = store.add(measurement("before"));
        store.get_mut(id).unwrap().name = "after".into();
        assert_eq!(store.get(id).unwrap().name, "after");
    }

    #[test]
    fn removing_returns_the_measurement_and_shrinks_the_store() {
        let mut store = MeasurementStore::new();
        let id = store.add(measurement("gone"));
        assert_eq!(store.len(), 1);

        let removed = store.remove(id).unwrap();
        assert_eq!(removed.name, "gone");
        assert!(store.is_empty());
        assert!(store.get(id).is_none());
        assert!(store.remove(id).is_none(), "removing twice yields nothing");
    }

    #[test]
    fn clearing_resets_identifiers() {
        let mut store = MeasurementStore::new();
        store.add(measurement("a"));
        store.clear();
        assert!(store.is_empty());
        assert_eq!(store.add(measurement("b")), MeasurementId(1));
    }
}
