#include "model/measurement.hpp"

#include <cmath>
#include <utility>

#include "base/number_text.hpp"

namespace analyzer::model {

namespace {

std::vector<double> magnitude_db_of(const std::vector<Complex64>& bins) {
    std::vector<double> out;
    out.reserve(bins.size());
    for (const Complex64& bin : bins) {
        const double magnitude = std::abs(bin);
        out.push_back(magnitude > 0.0 ? 20.0 * std::log10(magnitude) : -200.0);
    }
    return out;
}

std::vector<double> phase_degrees_of(const std::vector<Complex64>& bins) {
    std::vector<double> out;
    out.reserve(bins.size());
    for (const Complex64& bin : bins) {
        out.push_back(text::to_degrees(std::arg(bin)));
    }
    return out;
}

}  // namespace

std::string to_string(MeasurementId id) {
    return "#" + std::to_string(id.value);
}

std::string_view kind(const MeasurementData& data) noexcept {
    struct Visitor {
        std::string_view operator()(const SpectrumData&) const noexcept { return "spectrum"; }
        std::string_view operator()(const PowerSpectrumData&) const noexcept {
            return "power_spectrum";
        }
        std::string_view operator()(const ImpulseResponseData&) const noexcept {
            return "impulse_response";
        }
        std::string_view operator()(const TransferFunctionData&) const noexcept {
            return "transfer_function";
        }
    };
    return std::visit(Visitor{}, data);
}

std::size_t point_count(const MeasurementData& data) noexcept {
    struct Visitor {
        std::size_t operator()(const SpectrumData& d) const noexcept { return d.bins.size(); }
        std::size_t operator()(const PowerSpectrumData& d) const noexcept {
            return d.magnitude_db.size();
        }
        std::size_t operator()(const ImpulseResponseData& d) const noexcept {
            return d.samples.size();
        }
        std::size_t operator()(const TransferFunctionData& d) const noexcept {
            return d.bins.size();
        }
    };
    return std::visit(Visitor{}, data);
}

bool is_empty(const MeasurementData& data) noexcept {
    return point_count(data) == 0;
}

std::optional<double> bin_spacing_hz(const MeasurementData& data) noexcept {
    struct Visitor {
        std::optional<double> operator()(const SpectrumData& d) const noexcept {
            return d.bin_spacing_hz;
        }
        std::optional<double> operator()(const PowerSpectrumData& d) const noexcept {
            return d.bin_spacing_hz;
        }
        std::optional<double> operator()(const TransferFunctionData& d) const noexcept {
            return d.bin_spacing_hz;
        }
        std::optional<double> operator()(const ImpulseResponseData&) const noexcept {
            return std::nullopt;
        }
    };
    return std::visit(Visitor{}, data);
}

std::optional<std::vector<double>> magnitude_db(const MeasurementData& data) {
    struct Visitor {
        std::optional<std::vector<double>> operator()(const SpectrumData& d) const {
            return magnitude_db_of(d.bins);
        }
        std::optional<std::vector<double>> operator()(const TransferFunctionData& d) const {
            return magnitude_db_of(d.bins);
        }
        // Already in decibels; nothing to derive.
        std::optional<std::vector<double>> operator()(const PowerSpectrumData& d) const {
            return d.magnitude_db;
        }
        std::optional<std::vector<double>> operator()(const ImpulseResponseData&) const {
            return std::nullopt;
        }
    };
    return std::visit(Visitor{}, data);
}

std::optional<std::vector<double>> phase_degrees(const MeasurementData& data) {
    struct Visitor {
        std::optional<std::vector<double>> operator()(const SpectrumData& d) const {
            return phase_degrees_of(d.bins);
        }
        std::optional<std::vector<double>> operator()(const TransferFunctionData& d) const {
            return phase_degrees_of(d.bins);
        }
        // No phase was ever measured, so none is reported.
        std::optional<std::vector<double>> operator()(const PowerSpectrumData&) const {
            return std::nullopt;
        }
        std::optional<std::vector<double>> operator()(const ImpulseResponseData&) const {
            return std::nullopt;
        }
    };
    return std::visit(Visitor{}, data);
}

Measurement::Measurement(MeasurementId identifier, std::string display_name, double rate,
                         MeasurementData payload)
    : id(identifier), name(std::move(display_name)), sample_rate(rate), data(std::move(payload)) {}

std::optional<double> Measurement::bin_frequency(std::size_t index) const noexcept {
    const std::optional<double> spacing = bin_spacing_hz(data);
    if (!spacing) {
        return std::nullopt;
    }
    return static_cast<double>(index) * *spacing;
}

std::optional<double> Measurement::duration_seconds() const noexcept {
    if (const auto* ir = std::get_if<ImpulseResponseData>(&data);
        ir != nullptr && sample_rate > 0.0) {
        return static_cast<double>(ir->samples.size()) / sample_rate;
    }
    return std::nullopt;
}

}  // namespace analyzer::model
