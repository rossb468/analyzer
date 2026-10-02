// What can go wrong in the model module, as exceptions.
//
// Everything here is setup or file code - parsing a measurement, writing a WAV,
// reading one back - never anything on the audio or analysis thread, so a
// failure throws rather than returning a status. See "Errors" in
// docs/CPP-CONVENTIONS.md. The C ABI catches ModelError at the boundary.
//
// One class per case a caller might want to handle differently, which is the
// Rust error enums' variants. Each message carries the same information the
// Rust Display did; the structured pieces (the field that did not parse, the
// byte counts of a truncated block) are also available through accessors so a
// caller need not take a message apart.

#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace analyzer::model {

// Base of everything this module throws.
class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A measurement file (.anlz) that could not be read. The subclasses below say
// why.
class FormatError : public ModelError {
public:
    using ModelError::ModelError;
};

// Not one of our files: the first line was not the magic.
class BadMagicError : public FormatError {
public:
    explicit BadMagicError(std::string found);

    // What the first line said instead.
    const std::string& found() const noexcept { return found_; }

private:
    std::string found_;
};

// The header ended without a separator line.
class MissingSeparatorError : public FormatError {
public:
    MissingSeparatorError();
};

// A required header key was absent.
class MissingFieldError : public FormatError {
public:
    explicit MissingFieldError(std::string field);

    const std::string& field() const noexcept { return field_; }

private:
    std::string field_;
};

// A header value did not parse.
class BadValueError : public FormatError {
public:
    BadValueError(std::string field, std::string value);

    // Which key.
    const std::string& field() const noexcept { return field_; }
    // What it said.
    const std::string& value() const noexcept { return value_; }

private:
    std::string field_;
    std::string value_;
};

// The binary block was shorter than the header promised.
class TruncatedError : public FormatError {
public:
    TruncatedError(std::size_t expected, std::size_t found);

    // Bytes the header said to expect.
    std::size_t expected() const noexcept { return expected_; }
    // Bytes actually present.
    std::size_t found() const noexcept { return found_; }

private:
    std::size_t expected_;
    std::size_t found_;
};

// An unrecognised measurement kind.
class UnknownKindError : public FormatError {
public:
    explicit UnknownKindError(std::string kind);

    const std::string& kind() const noexcept { return kind_; }

private:
    std::string kind_;
};

// A file could not be created, written, opened or read. The message names the
// path and says what the operating system reported.
class IoError : public ModelError {
public:
    using ModelError::ModelError;
};

// A parameter made no sense: a sample rate of zero, a duration that is not a
// number, more audio than a WAV file can hold.
class BadParameterError : public ModelError {
public:
    using ModelError::ModelError;
};

// A WAV file that opens but is in a sample format this project does not read.
class UnsupportedFormatError : public ModelError {
public:
    using ModelError::ModelError;
};

}  // namespace analyzer::model
