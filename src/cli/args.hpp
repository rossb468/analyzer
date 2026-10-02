// The command line, parsed.
//
// Hand-written rather than table-driven: the flags interact
// (--bench and --live take an optional value, --sine and --generate share
// --hz, --out means a report destination for one mode and the audio file for
// another), and the error messages and the order in which problems are noticed
// are part of the interface. The golden tests compare them word for word.

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "dsp/generator.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/window.hpp"
#include "model/wav.hpp"

namespace analyzer::cli {

// The usage text --help prints. Running with no arguments is an error that
// points at it instead.
std::string_view usage() noexcept;

// Analyse a WAV file.
struct WavInput {
    std::filesystem::path path;
};

// Synthesise a sine instead of reading a file.
struct SineInput {
    double hz;
    double rate;
    double seconds;
    float amplitude;
};

// Capture from hardware.
struct LiveInput {
    std::optional<std::string> device;
    double seconds;
};

// Show every audio device and exit.
struct ListDevicesInput {};

// Measure analysis throughput and ring behaviour.
struct BenchInput {
    double seconds;
};

// Build a synthetic room and measure it end to end.
struct MeasureDemoInput {};

// Deconvolve a recorded response against its stimulus.
struct MeasureInput {
    std::filesystem::path stimulus;
    std::filesystem::path response;
};

// Write a test signal to a WAV file.
struct GenerateInput {
    dsp::Signal signal;
    double rate;
    double seconds;
    model::SampleDepth depth;
    std::filesystem::path out;
};

// Compare two frequency/level exports.
struct CompareInput {
    std::filesystem::path subject;
    std::filesystem::path reference;
};

// What the run is for. The parser guarantees exactly one.
using Input = std::variant<WavInput, SineInput, LiveInput, ListDevicesInput, BenchInput,
                           MeasureDemoInput, MeasureInput, GenerateInput, CompareInput>;

// Every setting, with its default.
struct Args {
    Input input;
    std::size_t fft = 4096;
    dsp::WindowKind window = dsp::WindowKind::hann();
    dsp::Overlap overlap = dsp::Overlap::ThreeQuarters;
    dsp::Averaging average = dsp::Averaging::infinite();
    std::size_t channel = 0;
    std::size_t block = 128;
    std::optional<float> min_db;
    bool peak_only = false;
    bool meter = true;
    float gate_ms = 5.0f;
    std::optional<std::filesystem::path> out;
    double from_hz = 20.0;
    double to_hz = 20'000.0;
    std::optional<double> tolerance;

    // The analysis settings as a spectrum configuration at `sample_rate`.
    dsp::SpectrumConfig spectrum(float sample_rate) const;
};

// Parse `argv` (without the program name). Returns nullopt when help was
// asked for, and throws CliError, with the message to print, for anything
// else that is wrong.
std::optional<Args> parse_args(std::span<const std::string> argv);

}  // namespace analyzer::cli
