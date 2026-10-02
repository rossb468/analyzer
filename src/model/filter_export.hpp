// Filter export, in the formats other equalisers import.
//
// An equaliser you cannot get out of the application is half a feature: the
// point of designing a correction against a measurement is to load it into the
// thing that will actually apply it. Three formats cover most of what people
// own.
//
// REW and Equalizer APO are both parametric: they take the band description -
// shape, frequency, gain, Q - and design the filter themselves. Their line
// formats are near-identical, which is not a coincidence; APO's syntax was
// chosen to be what REW already exported.
//
// miniDSP is not. It takes raw biquad coefficients, which means this module has
// to design the sections and then deal with the fact that miniDSP's sign
// convention for the feedback coefficients is the negative of everyone else's
// (https://www.minidsp.com/applications/advanced-tools/advanced-biquad-programming).
// Getting that wrong produces a filter that is not merely mistuned but
// unstable, and it is invisible in the file.
//
// Shelves and pass filters carry a Q
//
// The shelving and pass filters here are the RBJ designs, specified by Q at the
// corner. REW and APO both distinguish that from a shelf specified by slope:
// LS and HS are the slope forms, LSC and HSC the Q forms, and likewise LP/HP
// against LPQ/HPQ. Emitting the slope tokens for a Q-designed filter would
// import as a different curve, so the Q tokens are what get written.
//
// Number formatting is part of the format. REW and APO text is compared byte
// for byte against what the Rust core wrote, so gains and frequencies print the
// way Rust prints them (see base/number_text.hpp).

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dsp/eq.hpp"

namespace analyzer::model {

// A format the equaliser can be written as.
enum class FilterFormat {
    // REW's own filter settings text.
    Rew,
    // An Equalizer APO config.txt.
    EqualizerApo,
    // miniDSP biquad coefficients.
    MiniDsp,
};

// Name for test failure messages and logs.
const char* to_string(FilterFormat format) noexcept;

// The token REW and Equalizer APO use for a shape.
//
// Returns nullopt for a shape neither can express, which is nothing today but
// would be the honest answer if a new one were added: writing an approximation
// under a token that means something else is worse than omitting the band.
std::optional<std::string_view> parametric_token(dsp::FilterKind kind) noexcept;

// Render the equaliser in `format`.
//
// `sample_rate` is only consulted for FilterFormat::MiniDsp, which exports
// designed coefficients rather than a description; the parametric formats
// redesign at whatever rate the importing device runs at.
std::string to_text(FilterFormat format, std::span<const dsp::FilterBand> bands, float preamp_db,
                    float sample_rate);

// Render as REW filter settings text.
//
// No date line is written. REW puts one there and its own parser ignores it,
// and a timestamp would make this function's output depend on when it ran,
// which costs a testable property for nothing.
std::string to_rew(std::span<const dsp::FilterBand> bands, float preamp_db);

// Render as an Equalizer APO configuration.
std::string to_equalizer_apo(std::span<const dsp::FilterBand> bands, float preamp_db);

// Render as miniDSP biquad coefficients.
//
// Two things happen here that the file itself cannot record.
//
// The feedback coefficients are negated. miniDSP's difference equation adds the
// feedback terms where the standard form subtracts them, so a1 and a2 go out
// with their signs flipped.
//
// The preamp is folded into the first section. A biquad file has nowhere to put
// a global trim, and dropping it would export a cascade that clips exactly
// where the trim existed to stop it. When every band is transparent the trim
// becomes a single gain-only section, so it is never silently lost.
std::string to_minidsp(std::span<const dsp::FilterBand> bands, float preamp_db, float sample_rate);

}  // namespace analyzer::model
