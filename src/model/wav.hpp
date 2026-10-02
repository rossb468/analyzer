// Reading and writing signals as WAV.
//
// Two things need this and they turn out to be the same thing. The parity run
// against REW needs test signals as files, because that is the only way to put
// *identical* input through two applications. And a measurement tool should be
// able to hand you the stimulus it just played, so you can take it to another
// machine, another room, or another analyser.
//
// File I/O does not belong in a platform layer, and both the harness and the
// clients need it, so it lives here with the other formats.
//
// Depth, and why the default is float
//
// A generated signal has no reason to be quantised. Sixteen-bit output of a
// test tone adds dither noise at -96 dBFS to a measurement whose whole point is
// measuring a noise floor, so SampleDepth::Float32 is the default and the
// integer depths exist for tools that will not read float.
//
// What is written
//
// Mono, with the headers the Rust core's WAV writer produced: the classic
// 44-byte PCM header for 16-bit integer, and WAVE_FORMAT_EXTENSIBLE (a 68-byte
// header, no fact chunk) for 24-bit integer and 32-bit float. Files written here
// are byte-for-byte the files the Rust core wrote.
//
// What is read
//
// 16-, 24- and 32-bit integer PCM and 32-bit float, any channel count, each
// normalised to float in -1..1 so nothing downstream has to care how the file
// was stored. Integers divide by 2^(bits-1), so full-scale positive is a hair
// under 1 and full-scale negative is exactly -1.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "dsp/generator.hpp"

namespace analyzer::model {

// Sample format to write.
enum class SampleDepth {
    // 16-bit integer. Lossy for a generated signal; here for compatibility.
    Int16,
    // 24-bit integer.
    Int24,
    // 32-bit float, the native form of everything upstream.
    Float32,
};

// The depth to use when nothing says otherwise: float, for the reason above.
inline constexpr SampleDepth kDefaultSampleDepth = SampleDepth::Float32;

// The token a command line uses: "i16", "i24" or "f32".
std::string_view as_key(SampleDepth depth) noexcept;

// Parse a token, or nullopt if it names no depth. Also accepts "16", "24",
// "32" and "float".
std::optional<SampleDepth> depth_from_key(std::string_view key) noexcept;

// Render `signal` for `seconds` at `sample_rate`.
//
// Returns the samples rather than writing them, so a caller can inspect,
// analyse or play what it is about to save. Throws BadParameterError for a sample
// rate or duration that is not a positive number, or more audio than a WAV file
// can hold.
std::vector<float> render(dsp::Signal signal, float sample_rate, float seconds);

// Write mono samples to `path`.
//
// Integer depths scale by 2^(bits-1) - 1 and round half away from zero, and
// are clamped rather than wrapped. A sample a hair over full scale is a
// rounding artefact and should saturate; wrapping would turn it into a
// full-scale excursion of the opposite sign, which is the loudest possible click
// at exactly the moment the signal was already at its peak.
//
// Throws BadParameterError for a sample rate that is not a positive number or
// more audio than a WAV file can hold, and IoError if the file cannot be
// written.
void write_wav(const std::filesystem::path& path, std::span<const float> samples, float sample_rate,
               SampleDepth depth);

// Render and write in one step. Returns how many frames were written.
std::size_t write_signal(const std::filesystem::path& path, dsp::Signal signal, float sample_rate,
                         float seconds, SampleDepth depth);

// A WAV file, read.
struct WavFile {
    // Interleaved, normalised to -1..1.
    std::vector<float> samples;
    std::size_t channels = 0;
    double sample_rate = 0.0;
    // How the file stored its samples: 16, 24 or 32 bits, as float or as
    // integers. (32-bit integer is read but never written, so SampleDepth has no
    // name for it.)
    std::uint16_t bits_per_sample = 0;
    bool is_float = false;

    // Frames, which is samples divided among the channels.
    std::size_t frames() const noexcept { return channels == 0 ? 0 : samples.size() / channels; }
};

// Read a WAV file.
//
// Throws IoError if the file cannot be opened, is not a WAV file or is shorter
// than its header says, and UnsupportedFormatError for a sample format outside
// the list above.
WavFile read_wav(const std::filesystem::path& path);

}  // namespace analyzer::model
