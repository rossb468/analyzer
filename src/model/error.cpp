#include "model/error.hpp"

#include <utility>

#include "model/text.hpp"

namespace analyzer::model {

BadMagicError::BadMagicError(std::string found)
    : FormatError("not an analyzer measurement file (magic was " + detail::debug_quote(found) +
                  ")"),
      found_(std::move(found)) {}

MissingSeparatorError::MissingSeparatorError() : FormatError("header has no '---' separator") {}

MissingFieldError::MissingFieldError(std::string field)
    : FormatError("header is missing '" + field + "'"), field_(std::move(field)) {}

BadValueError::BadValueError(std::string field, std::string value)
    : FormatError("cannot parse '" + field + "' from " + detail::debug_quote(value)),
      field_(std::move(field)),
      value_(std::move(value)) {}

TruncatedError::TruncatedError(std::size_t expected, std::size_t found)
    : FormatError("data block truncated: expected " + std::to_string(expected) + " bytes, found " +
                  std::to_string(found)),
      expected_(expected),
      found_(found) {}

UnknownKindError::UnknownKindError(std::string kind)
    : FormatError("unknown measurement kind " + detail::debug_quote(kind)),
      kind_(std::move(kind)) {}

}  // namespace analyzer::model
