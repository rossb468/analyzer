#include "model/format.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "base/number_text.hpp"
#include "model/error.hpp"
#include "model/text.hpp"

namespace analyzer::model {

namespace {

// Line separating the header from the binary block.
constexpr std::string_view kSeparator = "---";

// A newline, the separator, a newline: the separator is a line of its own, so
// "---" inside a value never ends the header early.
constexpr std::string_view kSeparatorLine = "\n---\n";

// Where the header ends and the data begins.
struct SeparatorAt {
    std::size_t header_end;
    std::size_t data_start;
};

std::optional<SeparatorAt> find_separator(std::span<const std::byte> bytes) {
    const auto needle = std::as_bytes(std::span(kSeparatorLine));
    const auto found = std::ranges::search(bytes, needle);
    if (found.empty()) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(found.begin() - bytes.begin());
    return SeparatorAt{index + 1, index + kSeparatorLine.size()};
}

void append_line(std::string& header, std::string_view key, std::string_view value) {
    header += key;
    header += ": ";
    header += value;
    header += '\n';
}

void write_optional(std::string& header, std::string_view name, std::optional<double> value) {
    if (value) {
        append_line(header, name, text::shortest(*value));
    }
    // Absent means unknown. Writing a placeholder would turn "not measured"
    // into "measured as zero" on the next read.
}

void push_u64_le(std::vector<std::byte>& out, std::uint64_t bits) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::byte>((bits >> shift) & 0xFF));
    }
}

void push_real(std::vector<std::byte>& out, std::span<const double> values) {
    out.reserve(out.size() + values.size() * 8);
    for (const double value : values) {
        push_u64_le(out, std::bit_cast<std::uint64_t>(value));
    }
}

void push_complex(std::vector<std::byte>& out, std::span<const Complex64> values) {
    out.reserve(out.size() + values.size() * 16);
    for (const Complex64& value : values) {
        push_u64_le(out, std::bit_cast<std::uint64_t>(value.real()));
        push_u64_le(out, std::bit_cast<std::uint64_t>(value.imag()));
    }
}

double f64_le_at(std::span<const std::byte> data, std::size_t offset) {
    std::uint64_t bits = 0;
    for (int i = 7; i >= 0; --i) {
        bits = (bits << 8) |
               std::to_integer<std::uint64_t>(data[offset + static_cast<std::size_t>(i)]);
    }
    return std::bit_cast<double>(bits);
}

// `count * size + offset`, or the largest size_t if that overflows, in which
// case no data block is long enough and the file reads as truncated.
std::size_t bytes_needed(std::size_t count, std::size_t size, std::size_t offset) {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    if (count > (kMax - offset) / size) {
        return kMax;
    }
    return offset + count * size;
}

std::vector<double> read_real(std::span<const std::byte> data, std::size_t count,
                              std::size_t offset) {
    const std::size_t needed = bytes_needed(count, 8, offset);
    if (data.size() < needed) {
        throw TruncatedError(needed, data.size());
    }
    std::vector<double> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(f64_le_at(data, offset + i * 8));
    }
    return out;
}

std::vector<Complex64> read_complex(std::span<const std::byte> data, std::size_t count) {
    const std::size_t needed = bytes_needed(count, 16, 0);
    if (data.size() < needed) {
        throw TruncatedError(needed, data.size());
    }
    std::vector<Complex64> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.emplace_back(f64_le_at(data, i * 16), f64_le_at(data, i * 16 + 8));
    }
    return out;
}

// Newlines in free text would break the line-oriented header, so they are
// escaped rather than rejected - a user should be able to write a paragraph of
// notes without the file becoming unreadable.
std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') {
            out += text[i];
            continue;
        }
        if (i + 1 == text.size()) {
            out += '\\';
            break;
        }
        const char next = text[++i];
        if (next == 'n') {
            out += '\n';
        } else if (next == '\\') {
            out += '\\';
        } else {
            out += '\\';
            out += next;
        }
    }
    return out;
}

}  // namespace

std::vector<std::byte> write_measurement(const Measurement& measurement) {
    std::string header;
    header += kMagic;
    header += '\n';
    append_line(header, "name", escape(measurement.name));
    append_line(header, "notes", escape(measurement.notes));
    append_line(header, "id", std::to_string(measurement.id.value));
    append_line(header, "captured_at", std::to_string(measurement.captured_at));
    append_line(header, "sample_rate", text::shortest(measurement.sample_rate));
    append_line(header, "channels", std::to_string(measurement.channels));
    append_line(header, "kind", kind(measurement.data));
    append_line(header, "points", std::to_string(point_count(measurement.data)));

    if (const std::optional<double> spacing = bin_spacing_hz(measurement.data)) {
        append_line(header, "bin_spacing_hz", text::shortest(*spacing));
    } else if (const auto* ir = std::get_if<ImpulseResponseData>(&measurement.data)) {
        append_line(header, "time_zero_samples", text::shortest(ir->time_zero_samples));
    }

    const References& references = measurement.references;
    write_optional(header, "spl_offset_db", references.spl_offset_db);
    write_optional(header, "full_scale_input_volts", references.full_scale_input_volts);
    write_optional(header, "full_scale_output_volts", references.full_scale_output_volts);
    write_optional(header, "reference_resistance_ohms", references.reference_resistance_ohms);
    write_optional(header, "propagation_delay_seconds", references.propagation_delay_seconds);

    header += kSeparator;
    header += '\n';

    std::vector<std::byte> out;
    out.reserve(header.size() + point_count(measurement.data) * 16);
    const auto header_bytes = std::as_bytes(std::span(header));
    out.assign(header_bytes.begin(), header_bytes.end());

    if (const auto* spectrum = std::get_if<SpectrumData>(&measurement.data)) {
        push_complex(out, spectrum->bins);
    } else if (const auto* power = std::get_if<PowerSpectrumData>(&measurement.data)) {
        push_real(out, power->magnitude_db);
    } else if (const auto* transfer = std::get_if<TransferFunctionData>(&measurement.data)) {
        push_complex(out, transfer->bins);
        push_real(out, transfer->coherence);
    } else if (const auto* ir = std::get_if<ImpulseResponseData>(&measurement.data)) {
        push_real(out, ir->samples);
    }
    return out;
}

Measurement read_measurement(std::span<const std::byte> bytes) {
    const std::optional<SeparatorAt> separator = find_separator(bytes);
    if (!separator) {
        throw MissingSeparatorError();
    }
    // Lossy rather than strict: a name with a stray byte in it is still a
    // measurement worth opening.
    const std::string header_text = detail::utf8_lossy(bytes.first(separator->header_end));
    const std::span<const std::byte> data = bytes.subspan(separator->data_start);

    const std::vector<std::string_view> lines = detail::lines(header_text);
    const std::string_view magic = lines.empty() ? std::string_view{} : detail::trim(lines[0]);
    if (magic != kMagic) {
        throw BadMagicError(std::string(magic));
    }

    std::vector<std::pair<std::string_view, std::string_view>> fields;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto split = detail::split_once(lines[i], ':');
        if (!split) {
            // Unknown shapes are skipped rather than fatal: a file from a newer
            // version should still load whatever this version understands.
            continue;
        }
        fields.emplace_back(detail::trim(split->first), detail::trim(split->second));
    }

    const auto get = [&fields](std::string_view name) -> std::optional<std::string_view> {
        const auto found = std::ranges::find(fields, name, &decltype(fields)::value_type::first);
        if (found == fields.end()) {
            return std::nullopt;
        }
        return found->second;
    };
    const auto number = [&get](std::string_view name) -> double {
        const std::optional<std::string_view> raw = get(name);
        if (!raw) {
            throw MissingFieldError(std::string(name));
        }
        const std::optional<double> value = text::parse_f64(*raw);
        if (!value) {
            throw BadValueError(std::string(name), std::string(*raw));
        }
        return *value;
    };
    const auto optional_number = [&get](std::string_view name) -> std::optional<double> {
        const std::optional<std::string_view> raw = get(name);
        return raw ? text::parse_f64(*raw) : std::nullopt;
    };

    const std::optional<std::string_view> kind_text = get("kind");
    if (!kind_text) {
        throw MissingFieldError("kind");
    }
    const auto points = analyzer::saturating_cast<std::size_t>(number("points"));
    const double sample_rate = number("sample_rate");

    // The data block is read before the key that goes with it, so a file with
    // both problems reports the truncation.
    MeasurementData payload;
    if (*kind_text == "spectrum") {
        SpectrumData spectrum;
        spectrum.bins = read_complex(data, points);
        spectrum.bin_spacing_hz = number("bin_spacing_hz");
        payload = std::move(spectrum);
    } else if (*kind_text == "power_spectrum") {
        PowerSpectrumData power;
        power.magnitude_db = read_real(data, points, 0);
        power.bin_spacing_hz = number("bin_spacing_hz");
        payload = std::move(power);
    } else if (*kind_text == "impulse_response") {
        ImpulseResponseData ir;
        ir.samples = read_real(data, points, 0);
        ir.time_zero_samples = number("time_zero_samples");
        payload = std::move(ir);
    } else if (*kind_text == "transfer_function") {
        TransferFunctionData transfer;
        transfer.bins = read_complex(data, points);
        transfer.coherence = read_real(data, points, bytes_needed(points, 16, 0));
        transfer.bin_spacing_hz = number("bin_spacing_hz");
        payload = std::move(transfer);
    } else {
        throw UnknownKindError(std::string(*kind_text));
    }

    Measurement measurement(MeasurementId{analyzer::saturating_cast<std::uint64_t>(
                                optional_number("id").value_or(0.0))},
                            unescape(get("name").value_or("")), sample_rate, std::move(payload));
    measurement.notes = unescape(get("notes").value_or(""));
    measurement.captured_at =
        analyzer::saturating_cast<std::int64_t>(optional_number("captured_at").value_or(0.0));
    measurement.channels =
        analyzer::saturating_cast<std::size_t>(optional_number("channels").value_or(1.0));
    measurement.references.spl_offset_db = optional_number("spl_offset_db");
    measurement.references.full_scale_input_volts = optional_number("full_scale_input_volts");
    measurement.references.full_scale_output_volts = optional_number("full_scale_output_volts");
    measurement.references.reference_resistance_ohms = optional_number("reference_resistance_ohms");
    measurement.references.propagation_delay_seconds = optional_number("propagation_delay_seconds");
    return measurement;
}

}  // namespace analyzer::model
