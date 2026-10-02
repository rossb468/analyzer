# Modules in dependency order: each may use only those listed before it.
#
#   base     contract checks shared by everything; header-only
#   dsp      the maths. No I/O, no platform code
#   cal      calibration chain, dBFS to absolute dB SPL
#   plot     display reduction and axis transforms. Emits no pixels
#   model    measurements, file formats, exports, settings      (dsp)
#   engine   real-time analysis graph and lock-free buffering   (dsp)
#   audio    AudioBackend and the platform backends
#   ffi      the C ABI the apps call                            (everything)
#   cli      the headless harness                               (most)
#
# A module directory that does not exist yet is skipped, so a module can be
# added without touching this list's logic.
foreach(module base dsp cal plot model engine audio ffi cli)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${module}/CMakeLists.txt)
        add_subdirectory(${module})
    endif()
endforeach()

# The single library the apps link. Only meaningful once the C ABI exists.
if(APPLE AND TARGET analyzer_ffi)
    include(${PROJECT_SOURCE_DIR}/cmake/AnalyzerBundle.cmake)
endif()
