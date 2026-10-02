// Ported from crates/analyzer-ffi/src/lib.rs.

#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "audio/target.hpp"
#include "ffi/internal.hpp"

namespace analyzer::ffi {
namespace {

TEST(Devices, EnumerationWorksOverTheBoundary) {
    AnalyzerDeviceList* list = analyzer_device_list_create();
    ASSERT_NE(list, nullptr);

    const std::size_t count = analyzer_device_list_count(list);
    AnalyzerDevice device{nullptr, nullptr, 0, 0, 0.0, false};

#if !ANALYZER_AUDIO_COREAUDIO && !ANALYZER_AUDIO_IOS
    // Where there is no platform backend the list is honestly empty, and must
    // still behave: no entry to read, nothing to crash on.
    EXPECT_EQ(count, 0u) << "no backend, yet devices were listed";
    EXPECT_FALSE(analyzer_device_list_get(list, 0, &device));
    analyzer_device_list_destroy(list);
    return;
#else
    ASSERT_GT(count, 0u) << "expected at least one device";
    ASSERT_TRUE(analyzer_device_list_get(list, 0, &device));
    ASSERT_NE(device.uid, nullptr);
    ASSERT_NE(device.name, nullptr);
    EXPECT_FALSE(std::string(device.uid).empty());

    // Out of range must fail rather than read past the end.
    EXPECT_FALSE(analyzer_device_list_get(list, count + 10, &device));

    analyzer_device_list_destroy(list);
#endif
}

}  // namespace
}  // namespace analyzer::ffi
