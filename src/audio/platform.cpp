#include "audio/platform.hpp"

#include <utility>

#include "audio/error.hpp"
#include "audio/target.hpp"

#if ANALYZER_AUDIO_COREAUDIO
#include "audio/coreaudio.hpp"
#elif ANALYZER_AUDIO_IOS
#include "audio/ios.hpp"
#endif

namespace analyzer::audio {

std::unique_ptr<AudioBackend> default_backend() {
#if ANALYZER_AUDIO_COREAUDIO
    return std::make_unique<CoreAudioBackend>();
#elif ANALYZER_AUDIO_IOS
    return std::make_unique<IosBackend>();
#else
    return std::make_unique<UnavailableBackend>();
#endif
}

std::unique_ptr<AudioStream> UnavailableBackend::open(const StreamConfig& /*config*/,
                                                      std::unique_ptr<AudioCallback> /*callback*/) {
    throw NoBackendError();
}

}  // namespace analyzer::audio
