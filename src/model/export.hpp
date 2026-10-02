// Text export, in the format REW imports.
//
// This is the interoperability bridge the plan settles on. REW's own .mdat is
// Java object serialisation and not worth reading from another language, but
// its text import is a documented three-column format that every acoustics tool
// understands. Emitting it exactly means measurements taken here can be opened
// in REW, compared with a decade of existing files, and checked against an
// independent implementation - which is also how the accuracy parity test works.
//
//   * Measurement data
//   * Freq(Hz) SPL(dB) Phase(degrees)
//   20.000000 72.4310 -14.2100
//
// Phase is emitted only when it is real. Padding the column with zeros for data
// that has no phase reference would be fabricating it, and a reader has no way
// to tell the difference.

#pragma once

#include <string>

#include "model/measurement.hpp"

namespace analyzer::model {

// Render a measurement as REW-compatible text.
//
// The measurement's own SPL offset, if it has one, converts dBFS to dB SPL on
// the way out; without one the level column stays in dBFS and the header says
// so, rather than labelling dBFS as SPL.
//
// Frequency is printed to six places, levels and phase to four, an impulse
// response's time and amplitude to nine. Newlines in the name are flattened to
// spaces so they cannot break the comment header.
std::string to_rew_text(const Measurement& measurement);

}  // namespace analyzer::model
