# libanalyzer.a: every module the C ABI needs, in one static library.
#
# CMake builds one archive per module, which is right for the core's own
# dependency graph and wrong for an app: the macOS and iOS clients link with
# swiftc, which knows nothing about CMake targets and would otherwise need
# every archive listed in the right order. So the archives are merged into one,
# and the apps link that plus the C++ runtime.
#
#   cmake --build build --target analyzer_bundle
#   -> build/lib/libanalyzer.a
#
# Apple-only for now, since only the Apple apps exist: Apple's libtool merges
# archives in one call. The Windows and Linux clients will want the same thing
# from lib.exe and ar when they arrive.

set(ANALYZER_BUNDLE_LIBRARIES
    analyzer_ffi analyzer_audio analyzer_engine analyzer_model
    analyzer_plot analyzer_cal analyzer_dsp kissfft)

set(bundle_output ${CMAKE_BINARY_DIR}/lib/libanalyzer.a)
set(bundle_inputs "")
foreach(library ${ANALYZER_BUNDLE_LIBRARIES})
    list(APPEND bundle_inputs $<TARGET_FILE:${library}>)
endforeach()

add_custom_command(
    OUTPUT ${bundle_output}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/lib
    COMMAND libtool -static -no_warning_for_no_symbols -o ${bundle_output} ${bundle_inputs}
    DEPENDS ${ANALYZER_BUNDLE_LIBRARIES}
    COMMENT "Merging the core into libanalyzer.a"
    VERBATIM)

add_custom_target(analyzer_bundle ALL DEPENDS ${bundle_output})
