// What the live paths do on a platform with no backend.
//
// The flags still exist and still parse, and saying so plainly beats a usage
// message that silently differs between platforms. Implementing them is the
// port work, not a missing feature of the harness.

#include "cli/error.hpp"
#include "cli/live.hpp"

namespace analyzer::cli {

namespace {

[[noreturn]] void unsupported() {
    throw CliError(
        "live capture needs a platform audio backend, and this build has "
        "none. CoreAudio is implemented; WASAPI and ALSA/PipeWire arrive with the Windows and "
        "Linux clients. Everything else in this harness runs here: analyse a WAV file, or use "
        "--bench, --measure or --measure-demo.");
}

}  // namespace

std::string list_devices() {
    unsupported();
}

engine::SpectrumFrame capture(const LiveOptions& /*options*/) {
    unsupported();
}

}  // namespace analyzer::cli
