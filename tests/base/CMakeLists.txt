# base is header-only, so there is no module library for analyzer_module_tests
# to link. This mirrors that function by hand: one GoogleTest executable from
# every *_test.cpp here, linked against the interface target.
file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(sources)
    add_executable(analyzer_base_tests ${sources})
    target_link_libraries(analyzer_base_tests PRIVATE
        analyzer::base analyzer::test_support GTest::gtest_main)
    analyzer_warnings(analyzer_base_tests)
    gtest_discover_tests(analyzer_base_tests
        PROPERTIES ENVIRONMENT "GTEST_DEATH_TEST_STYLE=threadsafe"
        DISCOVERY_TIMEOUT 60)
endif()
