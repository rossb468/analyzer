// Finding the loudest bin, the way the reported answer depends on.
//
// std::max_element returns the *first* of several equal maxima. That is the
// wrong answer for a spectrum: when a flat-topped or all-silent spectrum ties,
// the harness's reports, the golden files and the live meter all name the
// *last* bin, so which one is chosen is observable behaviour. This is the one
// place that rule is written.

#pragma once

#include <compare>
#include <cstddef>
#include <span>

#include "base/contract.hpp"

namespace analyzer {

// Index of the largest `projection(value)` over `values`, the last one when
// several tie. Use a projection to rank by something other than the value
// itself, such as its magnitude.
//
// Ordered by std::strong_order, the IEEE total order: -0.0 sorts below +0.0, and
// a NaN sorts above every number, so a NaN in the data is reported as the peak
// rather than silently skipped. `values` must not be empty.
template <class Projection>
std::size_t last_max_index(std::span<const float> values, Projection projection) noexcept {
    ANALYZER_EXPECTS(!values.empty(), "cannot take the peak of nothing");
    std::size_t best = 0;
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (std::is_gteq(std::strong_order(projection(values[i]), projection(values[best])))) {
            best = i;
        }
    }
    return best;
}

// Index of the largest of `values`, the last one when several tie.
inline std::size_t last_max_index(std::span<const float> values) noexcept {
    return last_max_index(values, [](float value) { return value; });
}

}  // namespace analyzer
