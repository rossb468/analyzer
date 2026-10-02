#include "model/store.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace analyzer::model {

MeasurementId MeasurementStore::add(Measurement measurement) {
    const MeasurementId id{next_id_};
    ++next_id_;
    measurement.id = id;
    measurements_.push_back(std::move(measurement));
    return id;
}

MeasurementId MeasurementStore::insert_loaded(Measurement measurement) {
    // The largest value is refused too: there would be no next identifier to
    // hand out after it.
    constexpr std::uint64_t kLargest = std::numeric_limits<std::uint64_t>::max();
    if (measurement.id.value == 0 || measurement.id.value == kLargest || contains(measurement.id)) {
        return add(std::move(measurement));
    }
    const MeasurementId id = measurement.id;
    next_id_ = std::max(next_id_, id.value + 1);
    measurements_.push_back(std::move(measurement));
    return id;
}

bool MeasurementStore::contains(MeasurementId id) const noexcept {
    return get(id) != nullptr;
}

const Measurement* MeasurementStore::get(MeasurementId id) const noexcept {
    const auto found = std::ranges::find(measurements_, id, &Measurement::id);
    return found == measurements_.end() ? nullptr : &*found;
}

Measurement* MeasurementStore::get(MeasurementId id) noexcept {
    const auto found = std::ranges::find(measurements_, id, &Measurement::id);
    return found == measurements_.end() ? nullptr : &*found;
}

std::optional<Measurement> MeasurementStore::remove(MeasurementId id) {
    const auto found = std::ranges::find(measurements_, id, &Measurement::id);
    if (found == measurements_.end()) {
        return std::nullopt;
    }
    std::optional<Measurement> removed(std::move(*found));
    measurements_.erase(found);
    return removed;
}

bool MeasurementStore::reorder(MeasurementId id, std::size_t to) {
    const auto found = std::ranges::find(measurements_, id, &Measurement::id);
    if (found == measurements_.end()) {
        return false;
    }
    const auto from = static_cast<std::size_t>(found - measurements_.begin());
    to = std::min(to, measurements_.size() - 1);
    if (from == to) {
        return true;
    }
    Measurement moved = std::move(*found);
    measurements_.erase(found);
    measurements_.insert(measurements_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    return true;
}

void MeasurementStore::clear() noexcept {
    measurements_.clear();
    next_id_ = 1;
}

std::string MeasurementStore::unique_name(std::string_view base) const {
    const auto taken = [this](std::string_view candidate) {
        return std::ranges::any_of(measurements_,
                                   [&](const Measurement& m) { return m.name == candidate; });
    };
    if (!taken(base)) {
        return std::string(base);
    }
    for (int suffix = 2; suffix < 1000; ++suffix) {
        std::string candidate = std::string(base) + " (" + std::to_string(suffix) + ")";
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return std::string(base);
}

}  // namespace analyzer::model
