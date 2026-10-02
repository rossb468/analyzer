// Frequency weighting curves from IEC 61672-1.
//
// Weighting exists because the ear is not a flat measuring instrument.
// A-weighting approximates the 40-phon equal-loudness contour and is what noise
// regulations are written against; C-weighting is much flatter and is used for
// peak levels and low-frequency work; Z is no weighting at all.
//
// The formulae here are the standard pole-based approximations, each normalised
// to exactly 0 dB at 1 kHz - which is the definition, not a convenience.

#pragma once

#include <string_view>

namespace analyzer::cal {

// Which weighting to apply.
enum class Weighting {
    // No weighting. The honest choice for measurement work, and the default
    // (the first enumerator, so a value-initialised Weighting is Z).
    Z,
    // A-weighting: rolls off hard below 500 Hz, matching how insensitive
    // hearing is to bass at moderate levels.
    A,
    // C-weighting: nearly flat through the audio band, rolling off outside it.
    C,
};

// Weighting in decibels at `hz`.
//
// Returns a large negative value (-200) at or below zero hertz rather than a
// NaN, so a caller summing weighted bins cannot poison its total with DC.
float db_at(Weighting weighting, float hz) noexcept;

// Short label for display.
std::string_view label(Weighting weighting) noexcept;

}  // namespace analyzer::cal
