# Unit tests: one GoogleTest executable per module, run through ctest.
#
# Each tests/<module>/ directory mirrors src/<module>/, with one
# <concept>_test.cpp per source file. The tests are ports of the Rust suite and
# carry its comments: they are the specification the C++ has to meet.

# GoogleTest is vendored in third_party/ like everything else, so the tests
# build offline. Only the gtest half is copied; nothing here uses gmock.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(GOOGLETEST_VERSION 1.15.2)
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/googletest
                 ${CMAKE_BINARY_DIR}/third_party/googletest EXCLUDE_FROM_ALL)
include(GoogleTest)

# Helpers shared by every module's tests: signal generators and span
# comparisons. Header-only.
add_library(analyzer_test_support INTERFACE)
add_library(analyzer::test_support ALIAS analyzer_test_support)
target_include_directories(analyzer_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(analyzer_test_support INTERFACE GTest::gtest)

foreach(module dsp cal plot model engine audio ffi cli)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${module}/CMakeLists.txt)
        add_subdirectory(${module})
    endif()
endforeach()
