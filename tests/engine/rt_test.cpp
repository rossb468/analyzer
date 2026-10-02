// Ported from crates/analyzer-engine/src/rt.rs.

#include "engine/rt.hpp"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::engine {
namespace {

// This binary links analyzer::alloc_trap. If it ever stops doing so, every
// other test in this file passes vacuously - so check that first.
TEST(AllocTrap, IsInstalledInTheTestBinary) {
    EXPECT_TRUE(alloc_trap_installed());
}

TEST(AllocTrap, RtSectionPassesValuesThrough) {
    const int sum = rt_section([] {
        int total = 0;
        for (int n = 1; n <= 10; ++n) {
            total += n;
        }
        return total;
    });
    EXPECT_EQ(sum, 55);
}

TEST(AllocTrap, PreallocatedBuffersAreFineToUse) {
    // The realistic shape: allocate up front, then only write.
    std::vector<float> buffer(128, 0.0f);
    rt_section([&buffer] {
        for (std::size_t n = 0; n < buffer.size(); ++n) {
            buffer[n] = static_cast<float>(n);
        }
    });
    EXPECT_EQ(buffer.back(), 127.0f);
}

TEST(AllocTrap, PermitAllocOpensAHoleInTheGuard) {
    const auto size =
        rt_section([] { return permit_alloc([] { return std::vector{1, 2, 3}.size(); }); });
    EXPECT_EQ(size, 3u);
}

TEST(AllocTrap, TheGuardIsLiftedOnExit) {
    rt_section([] {});
    // Outside any section again, so this must be allowed.
    auto allowed = std::make_unique<int>(1);
    EXPECT_EQ(*allowed, 1);
}

// Proves the trap actually fires, which is the whole point of the module. A
// violation aborts the process, so it is checked as a death test: GoogleTest
// runs the statement in a child process and expects it to die with the
// message.
TEST(AllocTrapDeathTest, AllocatingInsideAnRtSectionAbortsTheProcess) {
    EXPECT_DEATH(rt_section([] {
                     auto doomed = std::make_unique<std::vector<char>>(64);
                     return doomed->size();
                 }),
                 "inside rt_section");
}

}  // namespace
}  // namespace analyzer::engine
