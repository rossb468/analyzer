// Headless harness: drives the analysis chain from a file, a synthesised
// signal, or live hardware, and writes results as text.
//
// This is the Milestone 0 deliverable and the validation vehicle for
// everything after it. It proves numerical correctness before any UI exists,
// and exercises every module end to end - audio backend, allocation trap,
// capture ring, analysis engine and spectrum analyzer - so an integration
// mistake surfaces here rather than in the app.
//
// Offline analysis is deliberately single-threaded and reproducible; live
// capture runs the real threaded engine against real hardware.
//
// Everything except main() is in this library so it can be tested: run() takes
// the arguments and the two output streams, and returns the exit status.

#pragma once

#include <iosfwd>
#include <span>
#include <string>

namespace analyzer::cli {

// Run the harness with `argv` (without the program name).
//
// Reports go to `out`. Diagnostics go to `err`, and a failure is a line
// "analyzer-cli: <message>" there plus a status of 1; success is 0. Nothing
// escapes as an exception.
int run(std::span<const std::string> argv, std::ostream& out, std::ostream& err);

}  // namespace analyzer::cli
