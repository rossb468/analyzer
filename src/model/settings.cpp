#include "model/settings.hpp"

#include <algorithm>
#include <cmath>

#include "base/number_text.hpp"
#include "model/text.hpp"

namespace analyzer::model {

std::string_view as_key(WindowChoice window) noexcept {
    switch (window) {
        case WindowChoice::Rectangular: return "rectangular";
        case WindowChoice::Hann: return "hann";
        case WindowChoice::BlackmanHarris: return "blackman_harris";
        case WindowChoice::FlatTop: return "flat_top";
    }
    return "hann";
}

std::optional<WindowChoice> window_from_key(std::string_view key) noexcept {
    if (key == "rectangular") {
        return WindowChoice::Rectangular;
    }
    if (key == "hann") {
        return WindowChoice::Hann;
    }
    if (key == "blackman_harris") {
        return WindowChoice::BlackmanHarris;
    }
    if (key == "flat_top") {
        return WindowChoice::FlatTop;
    }
    return std::nullopt;
}

std::string_view as_key(AveragingChoice averaging) noexcept {
    switch (averaging) {
        case AveragingChoice::None: return "none";
        case AveragingChoice::Fast: return "fast";
        case AveragingChoice::Infinite: return "infinite";
        case AveragingChoice::PeakHold: return "peak_hold";
    }
    return "fast";
}

std::optional<AveragingChoice> averaging_from_key(std::string_view key) noexcept {
    if (key == "none") {
        return AveragingChoice::None;
    }
    if (key == "fast") {
        return AveragingChoice::Fast;
    }
    if (key == "infinite") {
        return AveragingChoice::Infinite;
    }
    if (key == "peak_hold") {
        return AveragingChoice::PeakHold;
    }
    return std::nullopt;
}

std::string Settings::to_text() const {
    std::string out;
    const auto line = [&out](std::string_view key, std::string_view value) {
        out += key;
        out += ": ";
        out += value;
        out += '\n';
    };

    out += kSettingsMagic;
    out += '\n';
    line("fft_size", std::to_string(fft_size));
    line("window", as_key(window));
    line("averaging", as_key(averaging));
    line("start_on_launch", start_on_launch ? "true" : "false");
    line("min_hz", text::shortest(min_hz));
    line("max_hz", text::shortest(max_hz));
    line("min_db", text::shortest(min_db));
    line("max_db", text::shortest(max_db));
    line("level_grid_step", text::shortest(level_grid_step));
    if (spl_offset_db) {
        line("spl_offset_db", text::shortest(*spl_offset_db));
    }
    if (mic_cal_path) {
        // A newline in a path would forge a new key; there is no escaping
        // scheme and this format does not need one, so such a path is dropped
        // rather than written as something that reads back wrong.
        if (mic_cal_path->find('\n') == std::string::npos) {
            line("mic_cal_path", *mic_cal_path);
        }
    }
    return out;
}

Settings Settings::from_text(std::string_view contents) {
    Settings settings;

    for (const std::string_view raw : detail::lines(contents)) {
        const std::string_view line = detail::trim(raw);
        if (line.empty() || line == kSettingsMagic || line.starts_with('#')) {
            continue;
        }
        const auto split = detail::split_once(line, ':');
        if (!split) {
            continue;
        }
        const std::string_view key = detail::trim(split->first);
        const std::string_view value = detail::trim(split->second);

        if (key == "fft_size") {
            if (const auto size = text::parse_u32(value)) {
                settings.fft_size = *size;
            }
        } else if (key == "window") {
            if (const auto window = window_from_key(value)) {
                settings.window = *window;
            }
        } else if (key == "averaging") {
            if (const auto averaging = averaging_from_key(value)) {
                settings.averaging = *averaging;
            }
        } else if (key == "start_on_launch") {
            if (const auto flag = text::parse_bool(value)) {
                settings.start_on_launch = *flag;
            }
        } else if (key == "min_hz") {
            if (const auto hz = text::parse_f32(value)) {
                settings.min_hz = *hz;
            }
        } else if (key == "max_hz") {
            if (const auto hz = text::parse_f32(value)) {
                settings.max_hz = *hz;
            }
        } else if (key == "min_db") {
            if (const auto db = text::parse_f32(value)) {
                settings.min_db = *db;
            }
        } else if (key == "max_db") {
            if (const auto db = text::parse_f32(value)) {
                settings.max_db = *db;
            }
        } else if (key == "level_grid_step") {
            if (const auto step = text::parse_f32(value)) {
                settings.level_grid_step = *step;
            }
        } else if (key == "spl_offset_db") {
            if (const auto offset = text::parse_f32(value)) {
                settings.spl_offset_db = *offset;
            }
        } else if (key == "mic_cal_path") {
            // An empty value leaves the path unset, which is what "no
            // correction file" means.
            if (!value.empty()) {
                settings.mic_cal_path = std::string(value);
            }
        }
    }

    return settings.validated();
}

Settings Settings::validated() const {
    Settings result = *this;
    const Settings defaults;

    if (std::ranges::find(kFftSizes, result.fft_size) == kFftSizes.end()) {
        result.fft_size = defaults.fft_size;
    }

    // A log frequency axis cannot start at or below zero, and an inverted or
    // empty span makes every position NaN.
    if (!std::isfinite(result.min_hz) || result.min_hz <= 0.0f) {
        result.min_hz = defaults.min_hz;
    }
    if (!std::isfinite(result.max_hz) || result.max_hz <= result.min_hz) {
        result.max_hz = std::max(result.min_hz * 2.0f, defaults.max_hz);
    }

    if (!std::isfinite(result.min_db)) {
        result.min_db = defaults.min_db;
    }
    if (!std::isfinite(result.max_db) || result.max_db <= result.min_db) {
        result.max_db = result.min_db + (defaults.max_db - defaults.min_db);
    }

    if (!std::isfinite(result.level_grid_step) || result.level_grid_step <= 0.0f) {
        result.level_grid_step = defaults.level_grid_step;
    }

    if (result.spl_offset_db && !std::isfinite(*result.spl_offset_db)) {
        result.spl_offset_db.reset();
    }
    if (result.mic_cal_path && result.mic_cal_path->empty()) {
        result.mic_cal_path.reset();
    }

    return result;
}

}  // namespace analyzer::model
