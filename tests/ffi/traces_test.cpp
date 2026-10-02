// Ported from crates/analyzer-ffi/src/lib.rs.

#include <string>

#include <gtest/gtest.h>

#include "ffi/internal.hpp"
#include "test_util.hpp"

namespace analyzer::ffi {
namespace {

TEST(Traces, CallsTolerateNullHandles) {
    AnalyzerTraceInfo info{};
    EXPECT_EQ(analyzer_trace_store_capture(nullptr, nullptr, nullptr), -1);
    EXPECT_EQ(analyzer_trace_store_count(nullptr), 0u);
    EXPECT_FALSE(analyzer_trace_store_info(nullptr, 0, &info));
    EXPECT_FALSE(analyzer_trace_store_set_visible(nullptr, 0, true));
    EXPECT_FALSE(analyzer_trace_store_remove(nullptr, 0));
    analyzer_trace_store_clear(nullptr);
    analyzer_trace_store_destroy(nullptr);
    EXPECT_EQ(analyzer_trace_store_copy(nullptr, 0, nullptr, nullptr, 0), 0u);
}

// The store has its own lifetime precisely so it can be exercised without a
// device, and so a trace survives the session that captured it.
TEST(Traces, AnEmptyStoreReportsEmptyAndRefusesBadIndices) {
    AnalyzerTraceStore* store = analyzer_trace_store_create();
    ASSERT_NE(store, nullptr);

    AnalyzerTraceInfo info{};
    EXPECT_EQ(analyzer_trace_store_count(store), 0u);
    EXPECT_FALSE(analyzer_trace_store_info(store, 0, &info));
    EXPECT_FALSE(analyzer_trace_store_set_visible(store, 3, true));
    EXPECT_FALSE(analyzer_trace_store_remove(store, 3));
    analyzer_trace_store_clear(store);

    // Capturing needs a session; without one there is nothing to store.
    EXPECT_EQ(analyzer_trace_store_capture(store, nullptr, nullptr), -1);

    analyzer_trace_store_destroy(store);
}

// Trace names cross the boundary in an inline buffer, so a long one must
// truncate on a character boundary rather than corrupt the string.
TEST(Traces, ALongTraceNameTruncatesSafely) {
    AnalyzerTraceInfo info{};
    std::string long_name;
    for (int i = 0; i < 200; ++i) {
        long_name += "\xC3\xA9";  // é
    }
    write_c_string(info.name, long_name);
    const std::string read = read_c_string(info.name);
    EXPECT_TRUE(test::only_repeats_of(read, "\xC3\xA9")) << "got " << read;
    EXPECT_LT(read.size(), static_cast<std::size_t>(ANALYZER_TRACE_NAME_LEN));
}

}  // namespace
}  // namespace analyzer::ffi
