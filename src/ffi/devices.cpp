// Device enumeration.

#include <algorithm>

#include "audio/platform.hpp"
#include "ffi/internal.hpp"
#include "ffi/session.hpp"

namespace {

// An identifier or name as a C string. One with an embedded NUL cannot be one,
// and is carried as empty rather than silently cut short.
std::string as_c_string(const std::string& text) {
    return text.find('\0') == std::string::npos ? text : std::string();
}

}  // namespace

extern "C" AnalyzerDeviceList* analyzer_device_list_create(void) noexcept {
    return analyzer::ffi::guard<AnalyzerDeviceList*>(nullptr, []() -> AnalyzerDeviceList* {
        auto list = std::make_unique<AnalyzerDeviceList>();
        try {
            list->devices = analyzer::audio::default_backend()->devices();
        } catch (const std::exception&) {
            // A backend that cannot enumerate has no devices to offer; the list
            // is honestly empty rather than the call failing.
            list->devices.clear();
        }
        list->strings.reserve(list->devices.size());
        for (const analyzer::audio::DeviceInfo& device : list->devices) {
            list->strings.emplace_back(as_c_string(device.id.str()), as_c_string(device.name));
        }
        return list.release();
    });
}

extern "C" void analyzer_device_list_destroy(AnalyzerDeviceList* list) noexcept {
    if (list == nullptr) {
        return;
    }
    analyzer::ffi::guard([&] { delete list; });
}

extern "C" uintptr_t analyzer_device_list_count(const AnalyzerDeviceList* list) noexcept {
    if (list == nullptr) {
        return 0;
    }
    return analyzer::ffi::guard<std::uintptr_t>(0, [&] { return list->devices.size(); });
}

extern "C" bool analyzer_device_list_get(const AnalyzerDeviceList* list, uintptr_t index,
                                         AnalyzerDevice* out) noexcept {
    if (list == nullptr || out == nullptr) {
        return false;
    }
    return analyzer::ffi::guard(false, [&] {
        if (index >= list->devices.size() || index >= list->strings.size()) {
            return false;
        }
        const analyzer::audio::DeviceInfo& device = list->devices[index];
        const auto& [uid, name] = list->strings[index];
        out->uid = uid.c_str();
        out->name = name.c_str();
        out->input_channels = device.input_channels;
        out->output_channels = device.output_channels;
        out->sample_rate = device.default_sample_rate;
        out->is_default_input = device.is_default_input;
        return true;
    });
}
