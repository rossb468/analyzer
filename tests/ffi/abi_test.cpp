// Ported from crates/analyzer-ffi/src/lib.rs.
//
// The ABI facts the apps compile against: enumerator values, struct layouts
// and the mappings from the C enums onto the core's own types.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "ffi/convert.hpp"
#include "ffi/internal.hpp"

namespace analyzer::ffi {
namespace {

// Discriminants are part of the ABI. Changing one silently breaks an
// already-compiled UI, so they are pinned rather than left implicit.
TEST(Abi, EnumDiscriminantsAreStable) {
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerWindow_Rectangular), 0u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerWindow_Hann), 1u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerWindow_BlackmanHarris), 2u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerWindow_FlatTop), 3u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerWindow_Tukey), 4u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerOverlap_None), 0u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerOverlap_ThreeQuarters), 2u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerAveraging_PeakHold), 3u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerReduction_Mean), 1u);
}

TEST(Abi, EnumMappingsReachTheCoreTypes) {
    EXPECT_EQ(to_window_kind(AnalyzerWindow_FlatTop), dsp::WindowKind::flat_top());
    EXPECT_EQ(to_overlap(AnalyzerOverlap_Half), dsp::Overlap::Half);
    EXPECT_EQ(to_reduction(AnalyzerReduction_Mean), plot::Reduction::Mean);
}

// The structs cross a C boundary, so their layout must not drift silently.
TEST(Abi, ReprCTypesHaveTheExpectedShape) {
    EXPECT_EQ(sizeof(AnalyzerTick), 12u) << "float, float, bool + padding";
    EXPECT_EQ(alignof(AnalyzerStatus), 4u);
    EXPECT_GE(sizeof(AnalyzerFrameInfo), 24u);
}

// The same sizes and alignments c_compile_test.c asserts from C. The C++ view of
// the header and the C one must agree on every struct, because the apps build
// against the C one and this library against the other.
TEST(Abi, StructLayoutsAgreeWithTheCView) {
    if constexpr (sizeof(std::uintptr_t) == 8) {
        static_assert(sizeof(AnalyzerDevice) == 40 && alignof(AnalyzerDevice) == 8,
                      "AnalyzerDevice layout");
        static_assert(sizeof(AnalyzerSessionConfig) == 64 && alignof(AnalyzerSessionConfig) == 8,
                      "AnalyzerSessionConfig layout");
        static_assert(sizeof(AnalyzerStatus) == 260 && alignof(AnalyzerStatus) == 4,
                      "AnalyzerStatus layout");
        static_assert(sizeof(AnalyzerTransferInfo) == 20 && alignof(AnalyzerTransferInfo) == 4,
                      "AnalyzerTransferInfo layout");
        static_assert(sizeof(AnalyzerBand) == 20 && alignof(AnalyzerBand) == 4,
                      "AnalyzerBand layout");
        static_assert(sizeof(AnalyzerEqInfo) == 24 && alignof(AnalyzerEqInfo) == 8,
                      "AnalyzerEqInfo layout");
        static_assert(sizeof(AnalyzerFrameInfo) == 32 && alignof(AnalyzerFrameInfo) == 8,
                      "AnalyzerFrameInfo layout");
        static_assert(sizeof(AnalyzerDistortion) == 152 && alignof(AnalyzerDistortion) == 4,
                      "AnalyzerDistortion layout");
        static_assert(sizeof(AnalyzerTick) == 12 && alignof(AnalyzerTick) == 4,
                      "AnalyzerTick layout");
        static_assert(sizeof(AnalyzerTarget) == 24 && alignof(AnalyzerTarget) == 4,
                      "AnalyzerTarget layout");
        static_assert(sizeof(AnalyzerMeasureConfig) == 28 && alignof(AnalyzerMeasureConfig) == 4,
                      "AnalyzerMeasureConfig layout");
        static_assert(
            sizeof(AnalyzerMeasureProgress) == 32 && alignof(AnalyzerMeasureProgress) == 8,
            "AnalyzerMeasureProgress layout");
        static_assert(sizeof(AnalyzerMeasureResult) == 56 && alignof(AnalyzerMeasureResult) == 8,
                      "AnalyzerMeasureResult layout");
        static_assert(sizeof(AnalyzerTraceInfo) == 152 && alignof(AnalyzerTraceInfo) == 8,
                      "AnalyzerTraceInfo layout");
        static_assert(
            sizeof(AnalyzerOptimiserConfig) == 32 && alignof(AnalyzerOptimiserConfig) == 4,
            "AnalyzerOptimiserConfig layout");
        static_assert(sizeof(AnalyzerOptimisation) == 12 && alignof(AnalyzerOptimisation) == 4,
                      "AnalyzerOptimisation layout");
        static_assert(sizeof(AnalyzerSettings) == 1068 && alignof(AnalyzerSettings) == 4,
                      "AnalyzerSettings layout");
    }
}

TEST(Abi, TargetShapeCodesAreStable) {
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerTargetShape_Flat), 0u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerTargetShape_Tilt), 1u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerTargetShape_Room), 2u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerTargetShape_Custom), 3u);
}

TEST(Abi, FilterFormatCodesAreStable) {
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerFilterFormat_Rew), 0u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerFilterFormat_EqualizerApo), 1u);
    EXPECT_EQ(static_cast<std::uint32_t>(AnalyzerFilterFormat_MiniDsp), 2u);
    EXPECT_EQ(to_filter_format(AnalyzerFilterFormat_MiniDsp), model::FilterFormat::MiniDsp);
}

// Every handle the C side holds is a pointer to something it cannot see into,
// and every enum is exactly the width the header declares.
TEST(Abi, EnumsAreFourBytesWide) {
    static_assert(std::is_same_v<std::underlying_type_t<AnalyzerWindow>, std::uint32_t>);
    static_assert(std::is_same_v<std::underlying_type_t<AnalyzerEqMode>, std::uint32_t>);
    static_assert(std::is_same_v<std::underlying_type_t<AnalyzerSampleDepth>, std::uint32_t>);
    EXPECT_EQ(sizeof(AnalyzerSignal), 4u);
    EXPECT_EQ(sizeof(AnalyzerCurve), 4u);
}

// Both directions of the band mapping, so a field added to one side and not the
// other shows up here rather than as a fader that controls the wrong thing.
TEST(Abi, BandsRoundTripThroughTheCStruct) {
    dsp::FilterBand band;
    band.kind = dsp::FilterKind::HighShelf;
    band.hz = 8000.0f;
    band.gain_db = -3.5f;
    band.q = 0.7f;
    band.enabled = false;

    const AnalyzerBand c = to_c(band);
    EXPECT_EQ(c.kind, AnalyzerFilterKind_HighShelf);
    EXPECT_FALSE(c.enabled);
    EXPECT_EQ(to_filter_band(c), band);
}

// A value that is no enumerator must still give a defined answer.
TEST(Abi, AnEnumValueOutsideTheRangeFallsBackToTheDefault) {
    EXPECT_EQ(to_overlap(static_cast<AnalyzerOverlap>(99)), dsp::Overlap::ThreeQuarters);
    EXPECT_EQ(to_reduction(static_cast<AnalyzerReduction>(99)), plot::Reduction::Max);
    EXPECT_EQ(to_filter_kind(static_cast<AnalyzerFilterKind>(99)), dsp::FilterKind::Peaking);
}

}  // namespace
}  // namespace analyzer::ffi
