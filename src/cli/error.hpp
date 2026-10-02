// The one exception the harness throws for itself.
//
// Everything that can go wrong in the CLI - a flag that does not parse, a file
// that will not open, a measurement that fails - ends the same way: a message
// on stderr behind "analyzer-cli: " and exit status 1. The Rust passed a String
// up through Result; this is the C++ spelling of the same thing. Errors from
// the other modules (model::ModelError, audio::AudioError) are caught alongside
// it in run() and reported by their what() text.

#pragma once

#include <stdexcept>

namespace analyzer::cli {

// A failure the user can act on, described in the words the CLI should print.
class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace analyzer::cli
