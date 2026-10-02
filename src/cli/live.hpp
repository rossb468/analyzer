// Live capture, and what it looks like where there is no backend for it.
//
// The settings live here and the implementation does not. Capturing from real
// hardware needs a platform backend, and only CoreAudio exists so far, so the
// macOS implementation sits in live_coreaudio.cpp and everything else gets
// live_unsupported.cpp. CMake picks one; this header is what both implement.
//
// Everything else the harness does - WAV analysis, the bench, swept
// measurement against a pair of files - is pure computation and runs anywhere.
// Keeping the platform-bound part behind one build condition is what lets the
// core be built and tested on Linux and Windows without an audio stack at all.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "dsp/spectrum.hpp"
#include "engine/engine.hpp"

namespace analyzer::cli {

// Settings for a live capture run.
//
// Every field is read by the CoreAudio implementation and none of them by the
// stub. It is kept whole rather than trimmed per platform: the settings
// describe what a live capture *is*, and a second backend will want all of them.
struct LiveOptions {
    // Device UID, or nullopt for the system default input.
    std::optional<std::string> device;
    // How long to capture.
    double seconds = 0.0;
    // Channel of the device to analyse.
    std::size_t channel = 0;
    // Requested callback size.
    std::uint32_t block = 128;
    // Spectrum settings; sample_rate is overwritten with what the device grants.
    dsp::SpectrumConfig spectrum;
    // Print a running meter to stderr while capturing.
    bool meter = true;
};

// A description of every device the backend can see. Throws CliError when
// there is no backend or enumeration fails.
std::string list_devices();

// Capture for a while and return the final spectrum. Throws CliError when
// there is no backend, the device cannot be opened, blocks were dropped, or
// nothing but silence arrived.
//
// Rendering belongs to the caller so the live and offline paths share one
// formatter and cannot drift apart.
engine::SpectrumFrame capture(const LiveOptions& options);

}  // namespace analyzer::cli
