#include "cli/text.hpp"

#include <cmath>

#include "base/number_text.hpp"

namespace analyzer::cli {

template <std::floating_point Float>
std::string debug_float(Float value) {
    std::string out = text::shortest(value);
    if (std::isfinite(value) && out.find('.') == std::string::npos) {
        out += ".0";
    }
    return out;
}

template std::string debug_float<float>(float);
template std::string debug_float<double>(double);

}  // namespace analyzer::cli
