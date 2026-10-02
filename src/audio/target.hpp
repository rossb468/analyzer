// Which native backend this build targets.
//
// The one place that turns the compiler's idea of the platform into the two
// macros the rest of the module tests, so that "is this iOS?" is answered the
// same way in the platform selector, the backends' own headers and the tests.
// This is the C++ counterpart of the Rust cfg(target_os = "...") attributes.
//
// Both are always defined, to 0 or 1, so a misspelt macro is a -Wundef warning
// rather than a silent "off".
//
// TARGET_OS_IPHONE is true for the iOS simulator as well as devices, which is
// what is wanted: the simulator runs the same backend. CMake agrees, selecting
// the iOS sources when CMAKE_SYSTEM_NAME is "iOS".

#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define ANALYZER_AUDIO_IOS 1
#define ANALYZER_AUDIO_COREAUDIO 0
#else
#define ANALYZER_AUDIO_IOS 0
#define ANALYZER_AUDIO_COREAUDIO 1
#endif
#else
#define ANALYZER_AUDIO_IOS 0
#define ANALYZER_AUDIO_COREAUDIO 0
#endif
