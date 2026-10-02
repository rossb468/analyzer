// An in-memory collection of measurements.
//
// Deliberately a plain owned collection rather than anything clever. Sessions
// hold tens of measurements, not millions, and the operations that matter are
// add, remove, rename and iterate in a stable order. A user who reorders their
// measurement list and finds it reshuffled on reload will not trust the tool
// with anything else, so insertion order is preserved rather than being an
// accident of a hash map.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "model/measurement.hpp"

namespace analyzer::model {

// A session's measurements, in the order the user sees them.
//
// Not thread-safe; one thread owns it, normally the UI's.
class MeasurementStore {
public:
    // An empty store.
    MeasurementStore() = default;

    // Add a measurement, assigning it a fresh identifier.
    //
    // The caller's id is overwritten. Identifiers are the store's to hand out;
    // letting a caller choose invites two measurements sharing one.
    MeasurementId add(Measurement measurement);

    // Add a measurement loaded from a file, keeping its stored identifier
    // unless that would collide.
    //
    // Loading should preserve identity where it can, since notes and cross
    // references may point at it - but never at the cost of two measurements
    // answering to the same id.
    MeasurementId insert_loaded(Measurement measurement);

    // Whether an identifier is in use.
    bool contains(MeasurementId id) const noexcept;

    // The measurement with this identifier, or nullptr. The pointer is valid
    // until the store is next modified.
    const Measurement* get(MeasurementId id) const noexcept;

    // As above, for changing the measurement in place.
    Measurement* get(MeasurementId id) noexcept;

    // Remove a measurement, returning it, or nullopt if the identifier is
    // unknown.
    std::optional<Measurement> remove(MeasurementId id);

    // Move a measurement to a new position, for drag-to-reorder.
    //
    // Returns false if the identifier is unknown. The target is clamped, so a
    // drag past the end lands at the end rather than failing.
    bool reorder(MeasurementId id, std::size_t to);

    // Measurements in display order.
    std::span<const Measurement> measurements() const noexcept { return measurements_; }

    // How many measurements there are.
    std::size_t size() const noexcept { return measurements_.size(); }

    // Whether the store is empty.
    bool empty() const noexcept { return measurements_.empty(); }

    // Remove everything. Identifiers start again from 1.
    void clear() noexcept;

    // A name not already taken, derived from `base`.
    //
    // Duplicate names are not an error - two measurements of the same speaker
    // legitimately share one - but offering "Left (2)" by default saves the
    // user from a list of identical rows.
    std::string unique_name(std::string_view base) const;

    friend bool operator==(const MeasurementStore&, const MeasurementStore&) = default;

private:
    std::vector<Measurement> measurements_;
    std::uint64_t next_id_ = 1;
};

}  // namespace analyzer::model
