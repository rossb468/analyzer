# The audio module. Unlike most modules its source list is explicit, not
# globbed: the Apple backends must be compiled only for the Apple platform they
# belong to, and a glob would pull coreaudio.cpp and ios.mm into every build.
# Which backend is built is decided here, by CMake, not by #if in a header.
#
# Which platform is which:
#   iOS     CMAKE_SYSTEM_NAME is "iOS" (CMake also sets APPLE for it, which is
#           why the macOS branch below comes second)
#   macOS   APPLE and not iOS
#   other   neither; default_backend() falls back to UnavailableBackend

analyzer_module(audio
    SOURCES
        backend.cpp
        device.cpp
        error.cpp
        offline.cpp
        platform.cpp
        stream.cpp
    DEPS analyzer::base)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # AVAudioSession is Objective-C, so the iOS backend is Objective-C++.
    enable_language(OBJCXX)
    target_sources(analyzer_audio PRIVATE ios.mm)
    set_target_properties(analyzer_audio PROPERTIES
        OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED ON
        OBJCXX_EXTENSIONS OFF)
    # ARC for the Objective-C++ file only. It keeps the session objects it uses
    # alive without a retain/release written by hand; nothing Objective-C is
    # stored past a function, so it adds nothing to the real-time path.
    target_compile_options(analyzer_audio PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    # PUBLIC so that an executable linking the static library picks the
    # frameworks up too.
    target_link_libraries(analyzer_audio PUBLIC
        "-framework AVFAudio"
        "-framework AudioToolbox"
        "-framework Foundation")
elseif(APPLE)
    target_sources(analyzer_audio PRIVATE coreaudio.cpp)
    target_link_libraries(analyzer_audio PUBLIC
        "-framework CoreAudio"
        "-framework CoreFoundation")
endif()
