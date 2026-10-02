// Ported from crates/analyzer-ffi/src/lib.rs.

#include <gtest/gtest.h>

#include "ffi/internal.hpp"

namespace analyzer::ffi {
namespace {

TEST(Target, CallsTolerateNullHandles) {
    AnalyzerTarget target = analyzer_target_default();
    AnalyzerStatus status = status_ok();
    EXPECT_FALSE(analyzer_session_target(nullptr, &target));
    EXPECT_FALSE(analyzer_session_set_target(nullptr, &target));
    EXPECT_FALSE(analyzer_session_align_target(nullptr));
    EXPECT_EQ(analyzer_session_copy_target(nullptr, nullptr, 0), 0u);
    EXPECT_FALSE(analyzer_session_load_target(nullptr, nullptr, &status));
    EXPECT_NE(status.code, 0);
}

// The room parameters travel with every target, so switching to flat and back
// does not reset what was dialled in.
TEST(Target, TheDefaultTargetCarriesUsableRoomParameters) {
    const AnalyzerTarget target = analyzer_target_default();
    EXPECT_EQ(target.shape, AnalyzerTargetShape_Flat);
    EXPECT_GT(target.shelf_db, 0.0f);
    EXPECT_GT(target.transition_hz, 0.0f);
    EXPECT_FALSE(target.has_custom);
}

}  // namespace
}  // namespace analyzer::ffi
