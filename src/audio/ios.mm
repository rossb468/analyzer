// The iOS backend: Objective-C++, because AVAudioSession is an Objective-C API.
// Built only for iOS (see CMakeLists.txt, which compiles it with ARC); every
// Apple type in the module lives in this file or coreaudio.cpp.
//
// ARC and ownership: Objective-C objects here are only ever locals - the
// shared session and the ports it hands out. Nothing Objective-C is stored in
// a C++ object, and nothing on the render thread touches the Objective-C
// runtime, so the real-time path is plain C++ over C structs.

#include "audio/ios.hpp"

#if ANALYZER_AUDIO_IOS

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include "audio/error.hpp"

namespace analyzer::audio {

namespace {

// RemoteIO's input element. Microphone audio comes out of its output scope.
constexpr UInt32 kInputElement = 1;

// RemoteIO's output element. The speaker is fed through its input scope.
constexpr UInt32 kOutputElement = 0;

// Largest callback preallocated for.
//
// iOS raises the slice size to 4096 frames when the screen locks, whatever
// buffer duration was asked for. This leaves headroom above that, and the unit
// is told it may not exceed it.
constexpr std::size_t kMaxBufferFrames = 8192;

// Most hardware channels in one direction.
//
// The input buffer list is a fixed-size struct so that the render callback has
// one to hand without allocating. Thirty-two covers any USB interface an
// iPhone can power.
constexpr std::size_t kMaxChannels = 32;

// AudioBufferList with room for kMaxChannels buffers.
//
// Laid out exactly as the C struct with a longer trailing array, which is how
// CoreAudio expects a multi-buffer list to be allocated.
struct InputBufferList {
    UInt32 number_buffers;
    AudioBuffer buffers[kMaxChannels];
};

constexpr bool input_list_layout_matches() noexcept {
    return offsetof(InputBufferList, number_buffers) == offsetof(AudioBufferList, mNumberBuffers) &&
           offsetof(InputBufferList, buffers) == offsetof(AudioBufferList, mBuffers) &&
           alignof(InputBufferList) == alignof(AudioBufferList);
}

// Checked at compile time as well as by the test: the render callback hands an
// InputBufferList to CoreAudio as an AudioBufferList, which is sound only while
// this holds.
static_assert(input_list_layout_matches(),
              "InputBufferList must be laid out as an AudioBufferList");

BackendError os_error(OSStatus status, const std::string& operation) {
    return BackendError(operation + " failed with OSStatus " + std::to_string(status));
}

std::string to_std_string(NSString* string) {
    const char* utf8 = string != nil ? string.UTF8String : nullptr;
    return utf8 != nullptr ? std::string(utf8) : std::string();
}

BackendError ns_error(NSError* error, const std::string& operation) {
    const std::string description =
        error != nil ? to_std_string(error.localizedDescription) : std::string("unknown error");
    return BackendError(operation + " failed: " + description);
}

// A non-negative count from the session, which reports NSInteger.
std::uint32_t to_u32(NSInteger value) noexcept {
    if (value <= 0) {
        return 0;
    }
    const auto largest = static_cast<NSInteger>(std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(std::min(value, largest));
}

// Round a non-negative duration or latency expressed in frames. Negative and
// NaN values - a session that has not reported yet - read as zero.
std::uint32_t round_to_u32(double value) noexcept {
    if (!(value > 0.0)) {
        return 0;
    }
    if (value >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(std::round(value));
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// The shared session is a process-wide singleton and always exists.
AVAudioSession* shared_session() {
    return [AVAudioSession sharedInstance];
}

// Put the session in play-and-record measurement mode and activate it.
//
// Done before enumeration as well as before opening: availableInputs is only
// populated for a session whose category records, and the sample rate it
// reports is only meaningful once the session is active.
//
// The category and mode constants are always present on iOS; a missing one
// would mean a missing framework, which the link would already have failed on.
void configure(AVAudioSession* session) {
    NSError* error = nil;
    // DefaultToSpeaker: play-and-record otherwise routes output to the earpiece
    // receiver, which is useless as a measurement source.
    if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                         mode:AVAudioSessionModeMeasurement
                      options:AVAudioSessionCategoryOptionDefaultToSpeaker
                        error:&error]) {
        throw ns_error(error, "setting the audio session category");
    }
    if (![session setActive:YES error:&error]) {
        throw ns_error(error, "activating the audio session");
    }
}

std::uint32_t port_channels(AVAudioSessionPortDescription* port) {
    NSArray<AVAudioSessionChannelDescription*>* channels = port.channels;
    const NSUInteger count = channels != nil ? channels.count : 1;
    return static_cast<std::uint32_t>(std::max<NSUInteger>(count, 1));
}

std::optional<std::string> current_input_uid(AVAudioSession* session) {
    AVAudioSessionPortDescription* port = session.currentRoute.inputs.firstObject;
    if (port == nil) {
        return std::nullopt;
    }
    return to_std_string(port.UID);
}

// The available input port with this UID, or nil.
AVAudioSessionPortDescription* find_port(AVAudioSession* session, const std::string& uid) {
    for (AVAudioSessionPortDescription* port in session.availableInputs) {
        if (to_std_string(port.UID) == uid) {
            return port;
        }
    }
    return nil;
}

// ---------------------------------------------------------------------------
// RemoteIO
// ---------------------------------------------------------------------------

AudioUnit new_remote_io() {
    AudioComponentDescription description{};
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_RemoteIO;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    description.componentFlags = 0;
    description.componentFlagsMask = 0;
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (component == nullptr) {
        throw BackendError("RemoteIO audio unit not found");
    }
    AudioUnit unit = nullptr;
    const OSStatus status = AudioComponentInstanceNew(component, &unit);
    if (status != 0 || unit == nullptr) {
        throw os_error(status, "AudioComponentInstanceNew");
    }
    return unit;
}

// Set a fixed-size property. `value`'s size is what is passed, so CoreAudio
// reads exactly one T.
template <class T>
void set_property(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope,
                  AudioUnitElement element, const T& value, const std::string& operation) {
    const OSStatus status = AudioUnitSetProperty(unit, property, scope, element, &value,
                                                 static_cast<UInt32>(sizeof(T)));
    if (status != 0) {
        throw os_error(status, operation);
    }
}

// ---------------------------------------------------------------------------
// Render state and callback
// ---------------------------------------------------------------------------

// State the render callback reaches through its client pointer.
//
// Owned by IosStream::Impl, which is heap-allocated and never moved while the
// unit lives, so the pointer RemoteIO holds stays valid. Every buffer here is
// sized at open so the callback allocates nothing.
struct RenderState {
    RenderState(std::unique_ptr<AudioCallback> callback_in, bool capture_in,
                std::size_t hardware_channels, std::vector<std::uint32_t> selected_in,
                std::vector<std::uint32_t> outputs_in)
        : callback(std::move(callback_in)),
          capture(capture_in),
          input_planes(kMaxBufferFrames * std::max<std::size_t>(hardware_channels, 1), 0.0f),
          input_hardware_channels(hardware_channels),
          interleaved(kMaxBufferFrames * std::max<std::size_t>(selected_in.size(), 1), 0.0f),
          output_scratch(kMaxBufferFrames * std::max<std::size_t>(outputs_in.size(), 1), 0.0f),
          selected(std::move(selected_in)),
          outputs(std::move(outputs_in)) {
        for (AudioBuffer& buffer : input_list.buffers) {
            buffer.mNumberChannels = 1;
            buffer.mDataByteSize = 0;
            buffer.mData = nullptr;
        }
    }

    // Set once the unit exists; the callback needs it to pull the input.
    AudioUnit unit = nullptr;
    std::unique_ptr<AudioCallback> callback;
    bool capture;
    // Points into input_planes; refilled each callback, because AudioUnitRender
    // may rewrite the sizes and pointers it is given.
    InputBufferList input_list{};
    // One plane of kMaxBufferFrames per hardware input channel.
    std::vector<float> input_planes;
    std::size_t input_hardware_channels;
    // Selected input channels, interleaved.
    std::vector<float> interleaved;
    // Interleaved output the callback writes, scattered afterwards.
    std::vector<float> output_scratch;
    std::vector<std::uint32_t> selected;
    std::vector<std::uint32_t> outputs;
};

// Zero every buffer in a list.
//
// `list` must be null or a valid AudioBufferList.
void silence(AudioBufferList* list) noexcept {
    if (list == nullptr) {
        return;
    }
    AudioBuffer* buffers = list->mBuffers;
    for (std::size_t index = 0; index < list->mNumberBuffers; ++index) {
        if (buffers[index].mData != nullptr) {
            // The buffer reports its own size in bytes.
            std::memset(buffers[index].mData, 0, buffers[index].mDataByteSize);
        }
    }
}

// Write interleaved scratch into the unit's non-interleaved output planes.
//
// `list` must be null or a valid non-interleaved Float32 AudioBufferList.
void scatter(AudioBufferList* list, std::span<const std::uint32_t> selected,
             std::span<const float> interleaved, std::size_t frames) noexcept {
    if (list == nullptr || selected.empty()) {
        return;
    }
    const std::size_t count = list->mNumberBuffers;
    AudioBuffer* buffers = list->mBuffers;
    const std::size_t stride = selected.size();
    for (std::size_t slot = 0; slot < stride; ++slot) {
        const std::size_t wanted = selected[slot];
        if (wanted >= count) {
            continue;
        }
        const AudioBuffer& buffer = buffers[wanted];
        if (buffer.mData == nullptr) {
            continue;
        }
        // The plane's own size bounds the write, whatever `frames` says.
        const std::size_t writable = std::min(frames, buffer.mDataByteSize / sizeof(float));
        auto* data = static_cast<float*>(buffer.mData);
        for (std::size_t frame = 0; frame < writable; ++frame) {
            const std::size_t from = frame * stride + slot;
            if (from < interleaved.size()) {
                data[frame] = interleaved[from];
            }
        }
    }
}

// The real-time render callback RemoteIO invokes for its output element.
//
// Pulls the input, gathers the selected channels, calls through, scatters the
// output. It must not allocate, lock or throw: noexcept turns an exception
// into termination rather than an unwind through CoreAudio's C frames.
OSStatus render_callback(void* client, AudioUnitRenderActionFlags* flags,
                         const AudioTimeStamp* timestamp, UInt32 /*bus*/, UInt32 frame_count,
                         AudioBufferList* output) noexcept {
    // `client` is the pointer installed with the render callback, which points
    // at the RenderState inside a live IosStream::Impl for as long as the unit
    // can run.
    RenderState& state = *static_cast<RenderState*>(client);

    const std::size_t frames = frame_count;
    if (frames == 0 || frames > kMaxBufferFrames) {
        // Larger than preallocated: play silence rather than grow a buffer here.
        silence(output);
        return 0;
    }

    const std::size_t channels = state.selected.size();
    const std::span<float> gathered(state.interleaved.data(), frames * channels);
    std::ranges::fill(gathered, 0.0f);

    if (state.capture) {
        const std::size_t hardware = std::min(state.input_hardware_channels, kMaxChannels);
        state.input_list.number_buffers = static_cast<UInt32>(hardware);
        float* base = state.input_planes.data();
        for (std::size_t index = 0; index < hardware; ++index) {
            AudioBuffer& buffer = state.input_list.buffers[index];
            buffer.mNumberChannels = 1;
            buffer.mDataByteSize = static_cast<UInt32>(frames * sizeof(float));
            // Plane `index` lies inside input_planes, which holds
            // kMaxBufferFrames per hardware channel.
            buffer.mData = base + index * kMaxBufferFrames;
        }
        // InputBufferList is laid out as an AudioBufferList (static_assert
        // above) with `hardware` buffers, each large enough for `frames`.
        const OSStatus status =
            AudioUnitRender(state.unit, flags, timestamp, kInputElement, frame_count,
                            reinterpret_cast<AudioBufferList*>(&state.input_list));
        // A failed render - permission refused, or the route changing under
        // us - leaves the gathered input silent rather than stale.
        if (status == 0) {
            for (std::size_t slot = 0; slot < channels; ++slot) {
                const std::size_t plane_start =
                    std::size_t{state.selected[slot]} * kMaxBufferFrames;
                if (plane_start + frames > state.input_planes.size()) {
                    continue;
                }
                const float* plane = state.input_planes.data() + plane_start;
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    gathered[frame * channels + slot] = plane[frame];
                }
            }
        }
    }

    const std::size_t output_channels = state.outputs.size();
    const std::span<float> writable(state.output_scratch.data(), frames * output_channels);
    // Never hand the callback stale contents.
    std::ranges::fill(writable, 0.0f);

    AudioBuffers buffers(gathered, writable, channels, output_channels, frames);
    state.callback->process(buffers);

    silence(output);
    scatter(output, state.outputs, state.output_scratch, frames);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

struct IosStream::Impl {
    Impl(std::unique_ptr<AudioCallback> callback, bool capture, std::size_t hardware_channels,
         std::vector<std::uint32_t> selected, std::vector<std::uint32_t> outputs)
        : state(std::move(callback), capture, hardware_channels, std::move(selected),
                std::move(outputs)) {}

    // Null until build_unit() has created it.
    AudioUnit unit = nullptr;
    bool initialized = false;
    // RemoteIO holds a raw pointer to this member from build_unit() until the
    // destructor has disposed of the unit, so it must not be moved or replaced
    // in between - which owning it through a unique_ptr guarantees.
    RenderState state;
};

IosStream::IosStream(StreamConfig config, StreamLatency latency,
                     std::unique_ptr<AudioCallback> callback, std::uint32_t inputs_available)
    : config_(std::move(config)),
      latency_(latency),
      // Every buffer the render callback touches is allocated here, once.
      impl_(std::make_unique<Impl>(std::move(callback), !config_.input_channels.empty(),
                                   inputs_available, config_.input_channels,
                                   config_.output_channels)) {}

void IosStream::build_unit(double rate, std::uint32_t inputs_available,
                           std::uint32_t outputs_available, double io_buffer_duration_seconds) {
    const AudioUnit unit = new_remote_io();
    // From here on the unit must be disposed on every early exit, which the
    // destructor does once the stream owns it.
    impl_->unit = unit;
    impl_->state.unit = unit;
    const bool capture = impl_->state.capture;

    constexpr AudioFormatFlags kFloatPlanar =
        static_cast<AudioFormatFlags>(kAudioFormatFlagIsFloat) |
        static_cast<AudioFormatFlags>(kAudioFormatFlagIsPacked) |
        static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
    const auto format = [rate](std::uint32_t channels) {
        AudioStreamBasicDescription description{};
        description.mSampleRate = rate;
        description.mFormatID = kAudioFormatLinearPCM;
        description.mFormatFlags = kFloatPlanar;
        description.mBytesPerPacket = static_cast<UInt32>(sizeof(float));
        description.mFramesPerPacket = 1;
        description.mBytesPerFrame = static_cast<UInt32>(sizeof(float));
        description.mChannelsPerFrame = std::max<std::uint32_t>(channels, 1);
        description.mBitsPerChannel = 32;
        description.mReserved = 0;
        return description;
    };

    set_property(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, kInputElement,
                 static_cast<UInt32>(capture ? 1 : 0), "enabling input");
    if (capture) {
        set_property(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, kInputElement,
                     format(inputs_available), "setting the input format");
    }
    set_property(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kOutputElement,
                 format(outputs_available), "setting the output format");
    set_property(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                 static_cast<UInt32>(kMaxBufferFrames), "setting the maximum slice");

    AURenderCallbackStruct render{};
    render.inputProc = render_callback;
    render.inputProcRefCon = &impl_->state;
    set_property(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, kOutputElement,
                 render, "installing the render callback");

    const OSStatus status = AudioUnitInitialize(unit);
    if (status != 0) {
        throw os_error(status, "AudioUnitInitialize");
    }
    impl_->initialized = true;

    config_.sample_rate = rate;
    config_.buffer_frames =
        std::clamp<std::uint32_t>(round_to_u32(io_buffer_duration_seconds * rate), 1,
                                  static_cast<std::uint32_t>(kMaxBufferFrames));
}

IosStream::~IosStream() {
    // A destructor must not throw, and a failure to stop a stream that is being
    // thrown away has nowhere useful to go.
    try {
        stop();
    } catch (...) {
    }
    // Must happen before impl_ is freed: the unit holds a pointer into it. The
    // unit was created by this stream and is disposed exactly once.
    if (impl_->unit != nullptr) {
        if (impl_->initialized) {
            AudioUnitUninitialize(impl_->unit);
        }
        AudioComponentInstanceDispose(impl_->unit);
    }
}

void IosStream::start() {
    if (running_) {
        throw AlreadyRunningError();
    }
    @autoreleasepool {
        // Reactivate in case an interruption deactivated the session since open.
        configure(shared_session());
    }
    const OSStatus status = AudioOutputUnitStart(impl_->unit);
    if (status != 0) {
        throw os_error(status, "AudioOutputUnitStart");
    }
    running_ = true;
}

void IosStream::stop() {
    if (!running_) {
        return;
    }
    // Stopping a stopped unit is harmless.
    const OSStatus status = AudioOutputUnitStop(impl_->unit);
    running_ = false;
    if (status != 0) {
        throw os_error(status, "AudioOutputUnitStop");
    }
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

std::vector<DeviceInfo> IosBackend::devices() const {
    std::vector<DeviceInfo> devices;
    @autoreleasepool {
        AVAudioSession* session = shared_session();
        configure(session);
        const std::optional<std::string> current = current_input_uid(session);

        const double rate = session.sampleRate;
        const std::uint32_t outputs = to_u32(session.outputNumberOfChannels);
        // No inputs listed (nil) simply enumerates nothing.
        for (AVAudioSessionPortDescription* port in session.availableInputs) {
            std::string uid = to_std_string(port.UID);
            const bool routed = current == uid;
            devices.push_back(DeviceInfo{
                .id = DeviceId(uid),
                .name = to_std_string(port.portName),
                .input_channels = port_channels(port),
                .output_channels = outputs,
                // Only the routed port's rate is known without switching
                // to it. Opening another verifies the rate it really gets.
                .default_sample_rate = rate,
                .supported_sample_rates = {},
                .is_default_input = routed,
                .is_default_output = routed,
            });
        }
    }
    return devices;
}

std::unique_ptr<IosStream> IosBackend::open_stream(const StreamConfig& config,
                                                   std::unique_ptr<AudioCallback> callback) {
    config.validate();

    if (config.input && config.output && *config.input != *config.output) {
        throw BackendError("input '" + config.input->str() + "' and output '" +
                           config.output->str() +
                           "' are different devices; on iOS one session carries both "
                           "directions, so they must name the same port");
    }

    std::unique_ptr<IosStream> stream;
    @autoreleasepool {
        AVAudioSession* session = shared_session();
        configure(session);

        const std::optional<DeviceId>& uid = config.input ? config.input : config.output;
        std::string name = "iOS audio session";
        if (uid) {
            AVAudioSessionPortDescription* port = find_port(session, uid->str());
            if (port == nil) {
                throw DeviceNotFoundError(uid->str());
            }
            name = to_std_string(port.portName);
            NSError* input_error = nil;
            if (![session setPreferredInput:port error:&input_error]) {
                throw ns_error(input_error, "selecting the input");
            }
            // Ask for every channel the port has. The session defaults to
            // one even on a multichannel interface. Refusal is not fatal:
            // the counts actually granted are checked below.
            [session setPreferredInputNumberOfChannels:session.maximumInputNumberOfChannels
                                                 error:nil];
        }

        NSError* rate_error = nil;
        if (![session setPreferredSampleRate:config.sample_rate error:&rate_error]) {
            throw ns_error(rate_error, "requesting the sample rate");
        }
        const double wanted = static_cast<double>(std::clamp<std::uint32_t>(
            config.buffer_frames, 16, static_cast<std::uint32_t>(kMaxBufferFrames)));
        // A preference, and only a preference: iOS rounds it, and the
        // callback copes with whatever size actually arrives.
        [session setPreferredIOBufferDuration:wanted / config.sample_rate error:nil];

        const double rate = session.sampleRate;
        const std::uint32_t inputs_available = to_u32(session.inputNumberOfChannels);
        const std::uint32_t outputs_available = to_u32(session.outputNumberOfChannels);
        const double io_duration = session.IOBufferDuration;
        const StreamLatency latency{
            .input_frames = round_to_u32(session.inputLatency * rate),
            .output_frames = round_to_u32(session.outputLatency * rate),
            .safety_offset_frames = 0,
        };

        // Switching the input port can switch the hardware rate. Carrying on at
        // the wrong one would mislabel every frequency the engine reports.
        if (std::abs(rate - config.sample_rate) > 0.5) {
            throw UnsupportedSampleRateError(name, config.sample_rate);
        }

        const auto check_channels = [&name](const std::vector<std::uint32_t>& channels,
                                            std::uint32_t available) {
            for (const std::uint32_t channel : channels) {
                if (channel >= available || channel >= kMaxChannels) {
                    throw ChannelOutOfRangeError(name, channel, available);
                }
            }
        };
        check_channels(config.input_channels, inputs_available);
        check_channels(config.output_channels, outputs_available);

        // The constructor is private, so make_unique cannot reach it. If
        // build_unit() throws, the stream's destructor still runs and disposes
        // of whatever was created.
        stream.reset(new IosStream(config, latency, std::move(callback), inputs_available));
        stream->build_unit(rate, inputs_available, outputs_available, io_duration);
    }
    return stream;
}

std::unique_ptr<AudioStream> IosBackend::open(const StreamConfig& config,
                                              std::unique_ptr<AudioCallback> callback) {
    return open_stream(config, std::move(callback));
}

namespace detail {

bool input_list_matches_audio_buffer_list() noexcept {
    return input_list_layout_matches();
}

std::uint32_t remote_io_subtype() noexcept {
    return kAudioUnitSubType_RemoteIO;
}

}  // namespace detail

}  // namespace analyzer::audio

#endif  // ANALYZER_AUDIO_IOS
