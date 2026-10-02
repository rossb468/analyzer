// Ported from crates/analyzer-ffi/src/lib.rs.

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "ffi/internal.hpp"
#include "test_util.hpp"

namespace analyzer::ffi {
namespace {

TEST(Optimiser, OptimisingWithoutASessionReportsRatherThanCrashes) {
    const AnalyzerOptimiserConfig config = analyzer_optimiser_config_default();
    AnalyzerOptimisation result{};
    AnalyzerStatus status = status_ok();
    EXPECT_FALSE(analyzer_session_optimise(nullptr, &config, &result, &status));
    EXPECT_NE(status.code, 0);
    EXPECT_FALSE(analyzer_session_optimise(nullptr, nullptr, nullptr, nullptr));
}

// The defaults encode the rule that matters: boosting a null burns headroom
// without filling it, so boost is capped far below cut.
TEST(Optimiser, TheDefaultFitCapsBoostWellBelowCut) {
    const AnalyzerOptimiserConfig config = analyzer_optimiser_config_default();
    EXPECT_LT(config.max_boost_db, config.max_cut_db);
    EXPECT_GT(config.max_filters, 0u);
    EXPECT_GT(config.to_hz, config.from_hz);
    EXPECT_GT(config.max_q, config.min_q);
}

// Exporting with no equaliser running must say so rather than leave an empty
// file that looks like a successful export of nothing.
TEST(Optimiser, ExportingFiltersWithoutASessionReportsRatherThanWrites) {
    const std::filesystem::path path = test::scratch("no-session.txt");
    std::filesystem::remove(path);
    const std::string c = test::c_path(path);

    AnalyzerStatus status = status_ok();
    EXPECT_FALSE(
        analyzer_session_export_filters(nullptr, AnalyzerFilterFormat_Rew, c.c_str(), &status));
    EXPECT_NE(status.code, 0);
    EXPECT_FALSE(std::filesystem::exists(path)) << "nothing should have been written";
}

}  // namespace
}  // namespace analyzer::ffi
