// The state behind a session handle: the parts of its behaviour that more than
// one entry point needs.

#include "ffi/session.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/numeric.hpp"

namespace analyzer::ffi {

AnalyzerTarget default_target() noexcept {
    const dsp::TargetShape room = dsp::TargetShape::room();
    AnalyzerTarget target{};
    target.shape = AnalyzerTargetShape_Flat;
    target.shelf_db = room.shelf_db;
    target.transition_hz = room.transition_hz;
    target.db_per_octave = room.db_per_octave;
    target.offset_db = 0.0f;
    target.has_custom = false;
    return target;
}

}  // namespace analyzer::ffi

using analyzer::ffi::default_target;

AnalyzerSession::AnalyzerSession(
    analyzer::engine::Engine running_engine,
    std::unique_ptr<analyzer::audio::AudioStream> open_stream, float sample_rate,
    std::shared_ptr<analyzer::ffi::SignalState> generator_state,
    analyzer::engine::SnapshotPublisher<analyzer::ffi::EqCoefficients> publisher,
    std::string device)
    : engine(std::move(running_engine)),
      stream(std::move(open_stream)),
      // Placeholder geometry. The UI calls analyzer_session_set_plot with the
      // real drawable size before drawing anything.
      frequency(analyzer::plot::FrequencyAxis::audible(1000.0f)),
      level(analyzer::plot::LevelAxis::full_scale(600.0f)),
      phase(-180.0f, 180.0f, 600.0f),
      coherence(0.0f, 1.0f, 600.0f),
      signal(std::move(generator_state)),
      graphic(analyzer::dsp::Equaliser::graphic(sample_rate)),
      parametric(analyzer::dsp::Equaliser::parametric(sample_rate)),
      eq_publisher(std::move(publisher)),
      spectrogram_axis(analyzer::plot::FrequencyAxis::audible(600.0f)),
      device_name(std::move(device)) {}

void AnalyzerSession::stop() noexcept {
    // Stream first, so the callback has stopped feeding the ring before the
    // thread that drains it goes. Neither failing is worth reporting: the
    // session is going away either way.
    try {
        if (stream) {
            stream->stop();
        }
    } catch (...) {
    }
    try {
        engine.stop();
    } catch (...) {
    }
}

AnalyzerTarget AnalyzerSession::target_description() const {
    AnalyzerTarget out = default_target();
    out.offset_db = target.offset_db();
    out.has_custom = has_custom_target();

    const analyzer::dsp::TargetShape& shape = target.shape();
    switch (shape.kind) {
        case analyzer::dsp::TargetShape::Kind::Flat: out.shape = AnalyzerTargetShape_Flat; break;
        case analyzer::dsp::TargetShape::Kind::Tilt:
            out.shape = AnalyzerTargetShape_Tilt;
            out.db_per_octave = shape.db_per_octave;
            break;
        case analyzer::dsp::TargetShape::Kind::Room:
            out.shape = AnalyzerTargetShape_Room;
            out.shelf_db = shape.shelf_db;
            out.transition_hz = shape.transition_hz;
            out.db_per_octave = shape.db_per_octave;
            break;
        case analyzer::dsp::TargetShape::Kind::Custom:
            out.shape = AnalyzerTargetShape_Custom;
            break;
    }
    return out;
}

bool AnalyzerSession::has_custom_target() const noexcept {
    // The loaded points are kept so switching shapes and back does not discard
    // a file the user chose.
    const analyzer::dsp::TargetShape& shape = target.shape();
    return shape.kind == analyzer::dsp::TargetShape::Kind::Custom && !shape.points.empty();
}

void AnalyzerSession::apply_target(const AnalyzerTarget& wanted) {
    using analyzer::dsp::TargetShape;
    TargetShape shape = TargetShape::flat();
    switch (wanted.shape) {
        case AnalyzerTargetShape_Flat: break;
        case AnalyzerTargetShape_Tilt: shape = TargetShape::tilt(wanted.db_per_octave); break;
        case AnalyzerTargetShape_Room:
            shape = TargetShape::room(wanted.shelf_db, wanted.transition_hz, wanted.db_per_octave);
            break;
        case AnalyzerTargetShape_Custom:
            // Selecting Custom without a loaded file would evaluate to nothing.
            // Keeping the points already there, or falling back to flat, is
            // more honest than drawing a curve that is silently absent.
            if (target.shape().kind == TargetShape::Kind::Custom) {
                shape = target.shape();
            }
            break;
    }
    target.set_shape(std::move(shape));
    target.set_offset_db(wanted.offset_db);
}

std::optional<analyzer::ffi::MeasuredColumns> AnalyzerSession::measured_at_columns() {
    const std::size_t wanted = columns;
    columns_hz(wanted);

    const analyzer::engine::SpectrumFrame& frame = engine.latest();
    if (frame.bins.empty()) {
        return std::nullopt;
    }

    analyzer::ffi::MeasuredColumns out;
    const std::size_t count = std::min(wanted, column_hz.size());
    out.frequencies.assign(column_hz.begin(),
                           column_hz.begin() + static_cast<std::ptrdiff_t>(count));
    out.levels.reserve(count);
    for (const float hz : out.frequencies) {
        const auto bin =
            analyzer::saturating_cast<std::size_t>(std::round(hz / frame.bin_spacing_hz));
        out.levels.push_back(bin < frame.bins.size() ? frame.bins[bin]
                                                     : std::numeric_limits<float>::quiet_NaN());
    }
    return out;
}

bool AnalyzerSession::align_target() {
    const auto measured_columns = measured_at_columns();
    if (!measured_columns) {
        return false;
    }
    target.align_to(measured_columns->frequencies, measured_columns->levels,
                    analyzer::dsp::kAlignFromHz, analyzer::dsp::kAlignToHz);
    return true;
}

const analyzer::dsp::Equaliser* AnalyzerSession::equaliser() const noexcept {
    switch (eq_mode) {
        case AnalyzerEqMode_Off: return nullptr;
        case AnalyzerEqMode_Graphic: return &graphic;
        case AnalyzerEqMode_Parametric: return &parametric;
    }
    return nullptr;
}

analyzer::dsp::Equaliser* AnalyzerSession::equaliser() noexcept {
    return const_cast<analyzer::dsp::Equaliser*>(std::as_const(*this).equaliser());
}

void AnalyzerSession::publish_eq() {
    ++eq_generation;
    analyzer::ffi::EqCoefficients coefficients;
    if (const analyzer::dsp::Equaliser* eq = equaliser()) {
        coefficients = analyzer::ffi::EqCoefficients::from_equaliser(eq_generation, *eq);
    } else {
        coefficients.generation = eq_generation;
    }
    eq_publisher.publish_with(
        [&coefficients](analyzer::ffi::EqCoefficients& slot) { slot = coefficients; });
}

std::span<const float> AnalyzerSession::columns_hz(std::size_t wanted) {
    if (column_hz.size() != wanted) {
        column_hz.clear();
        const float width = frequency.width();
        for (std::size_t index = 0; index < wanted; ++index) {
            // The column centre, matching where `reduce` samples, so an
            // equaliser curve and a measured curve line up.
            const float x = (static_cast<float>(index) + 0.5f) * width / static_cast<float>(wanted);
            column_hz.push_back(frequency.x_to_freq(x));
        }
    }
    return column_hz;
}

std::size_t AnalyzerSession::copy_reduced(float* out, std::size_t capacity,
                                          analyzer::ffi::Which which) {
    const std::size_t wanted = std::min(columns, capacity);

    // Copy the bins out before reducing, so the reduction works from the
    // session's own scratch rather than from the frame the engine may replace.
    const analyzer::engine::SpectrumFrame& frame = engine.latest();
    const std::vector<float>& source =
        which == analyzer::ffi::Which::Live ? frame.bins : frame.average_bins;
    bins.assign(source.begin(), source.end());
    const float spacing = frame.bin_spacing_hz;

    analyzer::plot::Trace& destination =
        which == analyzer::ffi::Which::Live ? trace : average_trace;
    analyzer::plot::reduce(bins, spacing, frequency, wanted, reduction, destination);

    const std::size_t written = std::min(destination.points.size(), capacity);
    std::copy_n(destination.points.begin(), written, out);
    return written;
}

namespace analyzer::ffi {

model::Measurement live_measurement(AnalyzerSession& session, std::string name) {
    const engine::SpectrumFrame& frame = session.engine.latest();
    model::PowerSpectrumData data;
    data.magnitude_db.assign(frame.bins.begin(), frame.bins.end());
    data.bin_spacing_hz = static_cast<double>(frame.bin_spacing_hz);
    return model::Measurement(model::MeasurementId{0}, std::move(name),
                              static_cast<double>(frame.sample_rate), std::move(data));
}

std::string name_or(const char* name, std::string_view fallback) {
    if (name == nullptr) {
        return std::string(fallback);
    }
    const std::string_view view(name);
    return is_valid_utf8(view) ? std::string(view) : std::string(fallback);
}

}  // namespace analyzer::ffi
