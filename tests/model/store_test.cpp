// Ported from crates/analyzer-model/src/store.rs.

#include "model/store.hpp"

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::model {
namespace {

Measurement measurement(const std::string& name) {
    return Measurement(MeasurementId{0}, name, 48'000.0, ImpulseResponseData{{1.0, 2.0, 3.0}, 0.0});
}

std::vector<std::string> names(const MeasurementStore& store) {
    std::vector<std::string> out;
    for (const Measurement& m : store.measurements()) {
        out.push_back(m.name);
    }
    return out;
}

TEST(MeasurementStore, AddedMeasurementsGetUniqueIdentifiers) {
    MeasurementStore store;
    const MeasurementId a = store.add(measurement("a"));
    const MeasurementId b = store.add(measurement("b"));
    EXPECT_NE(a, b);
    EXPECT_EQ(store.get(a)->name, "a");
    EXPECT_EQ(store.get(b)->name, "b");
}

// Reusing an id after a removal would let a stale reference resolve to the
// wrong measurement.
TEST(MeasurementStore, IdentifiersAreNotReusedAfterRemoval) {
    MeasurementStore store;
    const MeasurementId a = store.add(measurement("a"));
    store.remove(a);
    const MeasurementId b = store.add(measurement("b"));
    EXPECT_NE(a, b) << "an identifier must not be handed out twice";
}

TEST(MeasurementStore, InsertionOrderIsPreserved) {
    MeasurementStore store;
    for (const char* name : {"first", "second", "third"}) {
        store.add(measurement(name));
    }
    EXPECT_EQ(names(store), (std::vector<std::string>{"first", "second", "third"}));
}

TEST(MeasurementStore, ReorderMovesAMeasurement) {
    MeasurementStore store;
    store.add(measurement("a"));
    const MeasurementId b = store.add(measurement("b"));
    store.add(measurement("c"));

    EXPECT_TRUE(store.reorder(b, 0));
    EXPECT_EQ(names(store), (std::vector<std::string>{"b", "a", "c"}));
}

TEST(MeasurementStore, ReorderClampsPastTheEndAndRejectsUnknownIds) {
    MeasurementStore store;
    const MeasurementId a = store.add(measurement("a"));
    store.add(measurement("b"));

    EXPECT_TRUE(store.reorder(a, 99));
    EXPECT_EQ(names(store), (std::vector<std::string>{"b", "a"}));

    EXPECT_FALSE(store.reorder(MeasurementId{999}, 0));
}

TEST(MeasurementStore, ALoadedMeasurementKeepsItsIdentifier) {
    MeasurementStore store;
    Measurement loaded = measurement("from disk");
    loaded.id = MeasurementId{42};

    const MeasurementId id = store.insert_loaded(std::move(loaded));
    EXPECT_EQ(id, MeasurementId{42});
    // And the next fresh id must not collide with it.
    EXPECT_GT(store.add(measurement("new")).value, 42u);
}

TEST(MeasurementStore, ACollidingLoadedIdentifierIsReassigned) {
    MeasurementStore store;
    const MeasurementId existing = store.add(measurement("existing"));

    Measurement loaded = measurement("loaded");
    loaded.id = existing;
    const MeasurementId id = store.insert_loaded(std::move(loaded));

    EXPECT_NE(id, existing) << "two measurements must not share an id";
    EXPECT_EQ(store.size(), 2u);
}

TEST(MeasurementStore, UniqueNameSuffixesOnlyWhenNeeded) {
    MeasurementStore store;
    EXPECT_EQ(store.unique_name("Left"), "Left");

    store.add(measurement("Left"));
    EXPECT_EQ(store.unique_name("Left"), "Left (2)");

    store.add(measurement("Left (2)"));
    EXPECT_EQ(store.unique_name("Left"), "Left (3)");
    EXPECT_EQ(store.unique_name("Right"), "Right");
}

TEST(MeasurementStore, MutationThroughTheStoreSticks) {
    MeasurementStore store;
    const MeasurementId id = store.add(measurement("before"));
    store.get(id)->name = "after";
    EXPECT_EQ(std::as_const(store).get(id)->name, "after");
}

TEST(MeasurementStore, RemovingReturnsTheMeasurementAndShrinksTheStore) {
    MeasurementStore store;
    const MeasurementId id = store.add(measurement("gone"));
    EXPECT_EQ(store.size(), 1u);

    const auto removed = store.remove(id);
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->name, "gone");
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.get(id), nullptr);
    EXPECT_FALSE(store.remove(id).has_value()) << "removing twice yields nothing";
}

TEST(MeasurementStore, ClearingResetsIdentifiers) {
    MeasurementStore store;
    store.add(measurement("a"));
    store.clear();
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.add(measurement("b")), MeasurementId{1});
}

}  // namespace
}  // namespace analyzer::model
