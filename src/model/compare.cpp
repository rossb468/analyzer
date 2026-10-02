#include "model/compare.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

#include "base/lines.hpp"
#include "base/number_text.hpp"
#include "model/text.hpp"

namespace analyzer::model {

namespace {

struct Difference {
    double hz;
    double db;
};

// The (frequency, value) with the largest magnitude. Of several equally large
// the last wins, because which frequency is reported when two tie is part of
// the report (std::max_element would pick the first).
template <class Select>
Difference worst(const std::vector<Difference>& differences, Select select) {
    Difference best{0.0, 0.0};
    bool first = true;
    for (const Difference& difference : differences) {
        const double value = select(difference.db);
        if (first || std::abs(value) >= std::abs(best.db)) {
            best = {difference.hz, value};
            first = false;
        }
    }
    return best;
}

template <class Select>
double rms(const std::vector<Difference>& differences, Select select) {
    double sum = 0.0;
    for (const Difference& difference : differences) {
        const double value = select(difference.db);
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(differences.size()));
}

bool is_comment(char first) noexcept {
    return first == '*' || first == '#' || first == ';' || first == '/';
}

bool is_column_separator(char c) noexcept {
    return c == '\t' || c == ',' || c == ';' || c == ' ';
}

using text::append_line;

}  // namespace

Response Response::parse(std::string_view contents) {
    Response response;

    for (const std::string_view raw : detail::lines(contents)) {
        const std::string_view row = detail::trim(raw);
        if (row.empty() || is_comment(row.front())) {
            continue;
        }

        // The first two non-empty fields; empty ones are what adjacent
        // separators leave behind.
        std::string_view fields[2];
        std::size_t found = 0;
        std::string_view rest = row;
        while (found < 2 && !rest.empty()) {
            const auto end = static_cast<std::size_t>(
                std::ranges::find_if(rest, is_column_separator) - rest.begin());
            if (end > 0) {
                fields[found++] = rest.substr(0, end);
            }
            rest.remove_prefix(std::min(end + 1, rest.size()));
        }
        if (found < 2) {
            continue;
        }
        const auto hz = text::parse_f64(fields[0]);
        const auto db = text::parse_f64(fields[1]);
        if (!hz || !db) {
            continue;
        }
        // A log axis cannot represent DC, and a non-finite level is not a
        // measurement. Both appear in real exports.
        if (*hz > 0.0 && std::isfinite(*hz) && std::isfinite(*db)) {
            response.points.push_back({*hz, *db});
        }
    }

    std::ranges::stable_sort(response.points, {}, &ResponsePoint::hz);
    return response;
}

std::optional<double> Response::level_at(double hz) const {
    if (points.empty()) {
        return std::nullopt;
    }
    if (points.size() == 1) {
        return points[0].hz == hz ? std::optional(points[0].db) : std::nullopt;
    }

    const ResponsePoint first = points.front();
    const ResponsePoint last = points.back();
    if (hz < first.hz || hz > last.hz) {
        return std::nullopt;
    }
    if (hz == first.hz) {
        return first.db;
    }

    const auto index = static_cast<std::size_t>(
        std::ranges::partition_point(points, [hz](const ResponsePoint& p) { return p.hz < hz; }) -
        points.begin());
    const ResponsePoint high = points[index];
    if (high.hz == hz) {
        return high.db;
    }
    const ResponsePoint low = points[index - 1];
    if (high.hz <= low.hz) {
        return low.db;
    }

    const double t = std::log(hz / low.hz) / std::log(high.hz / low.hz);
    return low.db + t * (high.db - low.db);
}

bool Response::has_exact(double hz) const {
    return std::ranges::binary_search(points, hz, {}, &ResponsePoint::hz);
}

std::string Comparison::report() const {
    std::string out;
    append_line(out, "# spectrum comparison");
    append_line(out,
                "# band: " + text::fixed(from_hz, 1) + " Hz to " + text::fixed(to_hz, 1) + " Hz");
    append_line(out, "# points: " + std::to_string(compared) + " (" + std::to_string(interpolated) +
                         " interpolated)");
    if (compared == 0) {
        append_line(out, "#");
        append_line(out, "# nothing overlapped. Check the two files cover the same band.");
        return out;
    }
    append_line(out, "#");
    append_line(out, "# as measured");
    append_line(out, "#   max deviation   " + text::signed_fixed(max_deviation, 4) + " dB at " +
                         text::fixed(max_deviation_hz, 1) + " Hz");
    append_line(out, "#   rms deviation   " + text::fixed(rms_deviation, 4) + " dB");
    append_line(out, "#");
    append_line(out, "# constant offset  " + text::signed_fixed(mean_offset, 4) +
                         " dB  (a reference convention, not a defect)");
    append_line(out, "#");
    append_line(out, "# with that offset removed - the number that matters");
    append_line(out, "#   max deviation   " + text::signed_fixed(max_deviation_after_offset, 4) +
                         " dB at " + text::fixed(max_deviation_after_offset_hz, 1) + " Hz");
    append_line(out, "#   rms deviation   " + text::fixed(rms_deviation_after_offset, 4) + " dB");
    return out;
}

Comparison compare(const Response& subject, const Response& reference, double from_hz,
                   double to_hz) {
    std::vector<Difference> differences;
    std::size_t interpolated = 0;

    for (const ResponsePoint& point : subject.points) {
        if (point.hz < from_hz || point.hz > to_hz) {
            continue;
        }
        const std::optional<double> other = reference.level_at(point.hz);
        if (!other) {
            continue;
        }
        if (!reference.has_exact(point.hz)) {
            ++interpolated;
        }
        differences.push_back({point.hz, point.db - *other});
    }

    Comparison result;
    result.from_hz = from_hz;
    result.to_hz = to_hz;
    result.compared = differences.size();
    if (differences.empty()) {
        return result;
    }

    double total = 0.0;
    for (const Difference& difference : differences) {
        total += difference.db;
    }
    const double mean_offset = total / static_cast<double>(differences.size());

    const auto as_measured = [](double d) { return d; };
    const auto offset_removed = [mean_offset](double d) { return d - mean_offset; };

    const Difference max_as_measured = worst(differences, as_measured);
    const Difference max_after = worst(differences, offset_removed);

    result.interpolated = interpolated;
    result.max_deviation = max_as_measured.db;
    result.max_deviation_hz = max_as_measured.hz;
    result.rms_deviation = rms(differences, as_measured);
    result.mean_offset = mean_offset;
    result.max_deviation_after_offset = std::abs(max_after.db);
    result.max_deviation_after_offset_hz = max_after.hz;
    result.rms_deviation_after_offset = rms(differences, offset_removed);
    return result;
}

}  // namespace analyzer::model
