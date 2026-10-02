// End-to-end swept measurement.
//
// Takes a stimulus and a recorded response, deconvolves them into an impulse
// response, and reports what falls out: arrival time and distance,
// reverberation time, and the gated frequency response.
//
// The demo mode builds a synthetic room with a known geometry and decay, runs
// the whole chain over it, and prints both what was constructed and what was
// measured. That makes the Milestone 2 chain verifiable end to end with no
// files, no hardware and no microphone permission - which is the same reason
// the offline harness exists for Milestone 1.

#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace analyzer::cli {

// How a measurement was obtained.
struct MeasureOptions {
    // Sweep duration for the synthetic demo.
    float seconds = 1.0f;
    // Gate length in milliseconds for the quasi-anechoic response.
    float gate_ms = 5.0f;
    // Transform size for the gated response.
    std::size_t fft = 4096;
    // Rows of frequency response to print.
    std::size_t response_rows = 24;
};

// Run the synthetic demo and return its report. Throws CliError if the
// measurement fails.
std::string demo(const MeasureOptions& options);

// Measure from a stimulus and response already in memory. Throws CliError when
// either is silent or the gate keeps no samples.
std::string analyse(std::span<const float> stimulus, std::span<const float> response,
                    float sample_rate, const MeasureOptions& options);

// Measure from two WAV files. Throws CliError on a sample-rate mismatch, and
// the model's exceptions when a file cannot be read.
std::string from_files(const std::filesystem::path& stimulus_path,
                       const std::filesystem::path& response_path, const MeasureOptions& options);

}  // namespace analyzer::cli
