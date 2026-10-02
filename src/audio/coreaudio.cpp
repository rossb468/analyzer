// The CoreAudio HAL code. Built only for macOS (see CMakeLists.txt); every
// Apple type in the module lives in this file.

#include "audio/coreaudio.hpp"

#if ANALYZER_AUDIO_COREAUDIO

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include "audio/error.hpp"

namespace analyzer::audio {

namespace {

// Largest callback we will preallocate for.
//
// The IOProc must not allocate, so its scratch buffers are sized once at open.
// A device asking for more frames than this would overflow them, so the open
// call clamps the request and re-reads what the device actually granted.
constexpr std::size_t kMaxBufferFrames = 8192;

// The HAL's "not permitted" status, observed when microphone access is denied.
//
// CoreAudio does not surface this as a named constant, and it does not arrive
// promptly: the server retries StartAndWaitForState on a 30 second timeout,
// so a denied stream stalls for minutes before failing. Recognising the code
// lets the caller say what is actually wrong instead of appearing to hang.
constexpr OSStatus kHalNotPermitted = 0x10004003;

// Turn an OSStatus from start/stop into something a user can act on.
BackendError describe_status(OSStatus status, const std::string& operation) {
    if (status == kHalNotPermitted) {
        return BackendError(
            operation + " was refused by CoreAudio (status " + std::to_string(status) +
            "). This is macOS microphone permission being denied. Grant access under System "
            "Settings > Privacy & Security > Microphone for the application running this code. "
            "Note that TCC will not raise a prompt for a process launched in a non-interactive "
            "background session - it refuses outright - so this must be run from a foreground "
            "terminal or a bundled app at least once.");
    }
    return BackendError(operation + " failed with OSStatus " + std::to_string(status));
}

// ---------------------------------------------------------------------------
// Property helpers
// ---------------------------------------------------------------------------

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope) noexcept {
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

// Copy a T out of a property's raw bytes, or nullopt if the bytes end first.
//
// A memcpy rather than a pointer cast: the HAL's byte blobs are arrays of C
// structs, and reading them through a cast pointer would rest on alignment and
// aliasing assumptions about a std::byte buffer. This way a short or malformed
// reply is a nullopt, not an out-of-bounds read.
template <class T>
std::optional<T> read_at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

// Read a variable-length property as raw bytes.
std::optional<std::vector<std::byte>> property_bytes(AudioObjectID object,
                                                     const AudioObjectPropertyAddress& addr) {
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(object, &addr, 0, nullptr, &size) != 0 || size == 0) {
        return std::nullopt;
    }
    std::vector<std::byte> buffer(size);
    // `size` is the capacity going in and what CoreAudio actually wrote coming
    // out; the buffer is exactly the capacity it asked for, so it cannot be
    // overrun.
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, buffer.data()) != 0) {
        return std::nullopt;
    }
    buffer.resize(std::min<std::size_t>(size, buffer.size()));
    return buffer;
}

// Read a fixed-size property.
template <class T>
std::optional<T> scalar(AudioObjectID object, AudioObjectPropertySelector selector,
                        AudioObjectPropertyScope scope) {
    const AudioObjectPropertyAddress addr = address(selector, scope);
    T value{};
    // The size says it is a T, so CoreAudio cannot overrun `value`.
    UInt32 size = static_cast<UInt32>(sizeof(T));
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) != 0) {
        return std::nullopt;
    }
    return value;
}

// Read a CFString property and copy it into an owned std::string.
std::optional<std::string> cfstring_property(AudioObjectID object,
                                             AudioObjectPropertySelector selector) {
    const AudioObjectPropertyAddress addr = address(selector, kAudioObjectPropertyScopeGlobal);
    CFStringRef cfstr = nullptr;
    UInt32 size = static_cast<UInt32>(sizeof(CFStringRef));
    // Writes exactly one CFStringRef, which we own a reference to.
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &cfstr) != 0 ||
        cfstr == nullptr) {
        return std::nullopt;
    }
    std::array<char, 1024> buffer{};
    const Boolean ok = CFStringGetCString(cfstr, buffer.data(), static_cast<CFIndex>(buffer.size()),
                                          kCFStringEncodingUTF8);
    // The Get rule does not apply here - AudioObjectGetPropertyData returns a
    // +1 reference for CFString properties, so we own and release it.
    CFRelease(cfstr);
    if (!ok) {
        return std::nullopt;
    }
    return std::string(buffer.data());
}

// Total channels in a scope, summed over the device's streams.
std::uint32_t channel_count(AudioDeviceID device, AudioObjectPropertyScope scope) {
    const auto bytes =
        property_bytes(device, address(kAudioDevicePropertyStreamConfiguration, scope));
    if (!bytes) {
        return 0;
    }
    const std::span<const std::byte> data(*bytes);
    // An AudioBufferList is a count followed by that many AudioBuffers. Read
    // field by field at the C struct's own offsets.
    const auto count = read_at<UInt32>(data, offsetof(AudioBufferList, mNumberBuffers));
    if (!count) {
        return 0;
    }
    std::uint32_t total = 0;
    for (UInt32 index = 0; index < *count; ++index) {
        const auto buffer = read_at<AudioBuffer>(
            data, offsetof(AudioBufferList, mBuffers) + index * sizeof(AudioBuffer));
        if (!buffer) {
            break;
        }
        total += buffer->mNumberChannels;
    }
    return total;
}

std::vector<double> available_rates(AudioDeviceID device) {
    const auto bytes = property_bytes(
        device,
        address(kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal));
    if (!bytes) {
        return {};
    }
    const std::span<const std::byte> data(*bytes);
    std::vector<double> rates;
    for (std::size_t offset = 0; offset + sizeof(AudioValueRange) <= data.size();
         offset += sizeof(AudioValueRange)) {
        const auto range = read_at<AudioValueRange>(data, offset);
        rates.push_back(range->mMinimum);
        if (std::abs(range->mMaximum - range->mMinimum) > 0.5) {
            rates.push_back(range->mMaximum);
        }
    }
    // Plain `<`: a device does not report NaN, and the sort below would be the
    // least of the problems if it did.
    std::ranges::sort(rates);
    rates.erase(std::unique(rates.begin(), rates.end(),
                            [](double a, double b) { return std::abs(a - b) < 0.5; }),
                rates.end());
    return rates;
}

std::vector<AudioDeviceID> all_device_ids() {
    const auto bytes =
        property_bytes(kAudioObjectSystemObject,
                       address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal));
    if (!bytes) {
        return {};
    }
    const std::span<const std::byte> data(*bytes);
    std::vector<AudioDeviceID> ids;
    for (std::size_t offset = 0; offset + sizeof(AudioDeviceID) <= data.size();
         offset += sizeof(AudioDeviceID)) {
        ids.push_back(*read_at<AudioDeviceID>(data, offset));
    }
    return ids;
}

std::optional<AudioDeviceID> default_device(AudioObjectPropertySelector selector) {
    const auto id =
        scalar<AudioDeviceID>(kAudioObjectSystemObject, selector, kAudioObjectPropertyScopeGlobal);
    if (!id || *id == 0) {
        return std::nullopt;
    }
    return id;
}

std::optional<DeviceInfo> describe(AudioDeviceID device, std::optional<AudioDeviceID> default_in,
                                   std::optional<AudioDeviceID> default_out) {
    // The UID is the stable identity; a device without one is not addressable
    // across restarts and is not worth offering.
    auto uid = cfstring_property(device, kAudioDevicePropertyDeviceUID);
    if (!uid) {
        return std::nullopt;
    }
    auto name = cfstring_property(device, kAudioObjectPropertyName)
                    .value_or("Audio device " + std::to_string(device));

    return DeviceInfo{
        .id = DeviceId(*uid),
        .name = std::move(name),
        .input_channels = channel_count(device, kAudioObjectPropertyScopeInput),
        .output_channels = channel_count(device, kAudioObjectPropertyScopeOutput),
        .default_sample_rate = scalar<double>(device, kAudioDevicePropertyNominalSampleRate,
                                              kAudioObjectPropertyScopeGlobal)
                                   .value_or(0.0),
        .supported_sample_rates = available_rates(device),
        .is_default_input = default_in == device,
        .is_default_output = default_out == device,
    };
}

void set_sample_rate(AudioDeviceID device, double requested, const std::string& name) {
    const double current = scalar<double>(device, kAudioDevicePropertyNominalSampleRate,
                                          kAudioObjectPropertyScopeGlobal)
                               .value_or(0.0);
    if (std::abs(current - requested) < 0.5) {
        return;
    }

    const AudioObjectPropertyAddress addr =
        address(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    // Writes exactly one double, which is what this property expects.
    const OSStatus status = AudioObjectSetPropertyData(
        device, &addr, 0, nullptr, static_cast<UInt32>(sizeof(double)), &requested);
    if (status != 0) {
        throw UnsupportedSampleRateError(name, requested);
    }
}

// Ask for a buffer size and report what the device actually granted.
std::uint32_t set_buffer_frames(AudioDeviceID device, std::uint32_t requested) {
    const UInt32 wanted = std::clamp<UInt32>(requested, 16, static_cast<UInt32>(kMaxBufferFrames));
    const AudioObjectPropertyAddress addr =
        address(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    // Writes exactly one UInt32, as the property expects. The status is
    // ignored on purpose: the device may refuse, and what it reports back below
    // is the only thing trusted.
    AudioObjectSetPropertyData(device, &addr, 0, nullptr, static_cast<UInt32>(sizeof(UInt32)),
                               &wanted);

    // The device may refuse or round, so trust only what it reports back.
    const std::uint32_t granted =
        scalar<UInt32>(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal)
            .value_or(wanted);

    if (granted > kMaxBufferFrames) {
        throw BackendError("device insists on " + std::to_string(granted) +
                           "-frame buffers, more than the " + std::to_string(kMaxBufferFrames) +
                           " preallocated");
    }
    return granted;
}

StreamLatency read_latency(AudioDeviceID device) {
    return StreamLatency{
        .input_frames =
            scalar<UInt32>(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput)
                .value_or(0),
        .output_frames =
            scalar<UInt32>(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput)
                .value_or(0),
        .safety_offset_frames =
            scalar<UInt32>(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput)
                .value_or(0),
    };
}

// ---------------------------------------------------------------------------
// The IOProc
// ---------------------------------------------------------------------------

// State the IOProc reaches through its client pointer.
//
// Owned by CoreAudioStream::Impl, which is heap-allocated and never moved while
// the stream lives, so the pointer CoreAudio holds stays valid. Every buffer
// here is sized at open so the IOProc allocates nothing.
struct IoProcState {
    IoProcState(std::unique_ptr<AudioCallback> callback_in, std::vector<std::uint32_t> selected_in,
                std::vector<std::uint32_t> outputs_in)
        : callback(std::move(callback_in)),
          interleaved(kMaxBufferFrames * std::max<std::size_t>(selected_in.size(), 1), 0.0f),
          output_scratch(kMaxBufferFrames * std::max<std::size_t>(outputs_in.size(), 1), 0.0f),
          selected(std::move(selected_in)),
          outputs(std::move(outputs_in)) {}

    std::unique_ptr<AudioCallback> callback;
    // Selected input channels, interleaved, refilled each callback.
    std::vector<float> interleaved;
    // Interleaved output the callback writes, scattered afterwards.
    std::vector<float> output_scratch;
    std::vector<std::uint32_t> selected;
    std::vector<std::uint32_t> outputs;
};

// Where one device channel lives inside a buffer list.
struct ChannelSlot {
    void* data;
    // Position of the channel within each frame of its buffer, and the
    // buffer's samples per frame.
    std::size_t offset;
    std::size_t stride;
    // Samples the buffer holds, for bounds checking against the frame count.
    std::size_t capacity;
};

// Walk the buffer list to find which stream holds device channel `wanted`.
//
// The HAL presents one AudioBuffer per stream, each carrying some number of
// interleaved channels, and device channels number straight through them.
std::optional<ChannelSlot> find_channel(const AudioBuffer* buffers, std::size_t count,
                                        std::uint32_t wanted) noexcept {
    std::uint32_t base = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t channels = buffers[index].mNumberChannels;
        if (wanted < base + channels) {
            return ChannelSlot{buffers[index].mData, wanted - base,
                               std::max<std::size_t>(channels, 1),
                               buffers[index].mDataByteSize / sizeof(float)};
        }
        base += channels;
    }
    return std::nullopt;
}

// Frames the output side is asking for, when there is no input to size against.
//
// `list` must be null or a valid AudioBufferList.
std::size_t output_frames(const AudioBufferList* list) noexcept {
    if (list == nullptr || list->mNumberBuffers == 0) {
        return 0;
    }
    const AudioBuffer& buffer = list->mBuffers[0];
    const std::size_t channels = std::max<std::uint32_t>(buffer.mNumberChannels, 1);
    return (buffer.mDataByteSize / sizeof(float)) / channels;
}

// Write the interleaved scratch back into CoreAudio's output buffers.
//
// Only the selected channels are written; every other channel the device
// exposes is silenced rather than left alone, because a measurement stimulus
// escaping from a channel the user did not choose is a genuinely bad surprise -
// and a device buffer is not guaranteed to arrive clean.
//
// `list` must be null or a valid AudioBufferList with Float32 samples.
void scatter_output(AudioBufferList* list, std::span<const std::uint32_t> selected_channels,
                    std::span<const float> interleaved, std::size_t frames) noexcept {
    if (list == nullptr || selected_channels.empty()) {
        return;
    }
    const std::size_t buffer_count = list->mNumberBuffers;
    if (buffer_count == 0) {
        return;
    }
    AudioBuffer* buffers = list->mBuffers;

    for (std::size_t index = 0; index < buffer_count; ++index) {
        if (buffers[index].mData != nullptr) {
            // The buffer reports its own size in bytes.
            std::memset(buffers[index].mData, 0, buffers[index].mDataByteSize);
        }
    }

    const std::size_t stride = selected_channels.size();
    for (std::size_t slot = 0; slot < stride; ++slot) {
        const auto target = find_channel(buffers, buffer_count, selected_channels[slot]);
        if (!target || target->data == nullptr) {
            continue;
        }
        auto* data = static_cast<float*>(target->data);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t at = frame * target->stride + target->offset;
            const std::size_t from = frame * stride + slot;
            // `frames` came from the input side or the first output buffer, so
            // this buffer's own size is checked rather than assumed.
            if (at >= target->capacity || from >= interleaved.size()) {
                break;
            }
            data[at] = interleaved[from];
        }
    }
}

// Copy the selected channels out of CoreAudio's buffer list into one
// interleaved buffer, returning the frames gathered.
//
// The HAL presents one AudioBuffer per stream, so a device may hand over a
// single interleaved buffer or several. Both shapes are flattened here so the
// rest of the system only ever sees interleaved frames.
//
// `list` must be a valid AudioBufferList with Float32 samples, as the HAL
// always provides.
std::size_t gather_input(const AudioBufferList* list,
                         std::span<const std::uint32_t> selected_channels,
                         std::span<float> interleaved) noexcept {
    const std::size_t buffer_count = list->mNumberBuffers;
    if (buffer_count == 0) {
        return 0;
    }

    const std::size_t selected = selected_channels.size();
    if (selected == 0) {
        return 0;
    }
    const AudioBuffer* buffers = list->mBuffers;

    // Frames is the same across buffers; take it from the first.
    const std::size_t first_channels = std::max<std::uint32_t>(buffers[0].mNumberChannels, 1);
    const std::size_t frames = std::min((buffers[0].mDataByteSize / sizeof(float)) / first_channels,
                                        interleaved.size() / selected);
    if (frames == 0) {
        return 0;
    }

    // A channel that cannot be found, or whose buffer is short, must read as
    // silence rather than as whatever the previous block left here.
    std::fill_n(interleaved.begin(), frames * selected, 0.0f);

    for (std::size_t slot = 0; slot < selected; ++slot) {
        const auto source = find_channel(buffers, buffer_count, selected_channels[slot]);
        if (!source || source->data == nullptr) {
            continue;
        }
        const auto* data = static_cast<const float*>(source->data);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t at = frame * source->stride + source->offset;
            // The frame count came from the first buffer; another stream's
            // buffer is checked against its own size.
            if (at >= source->capacity) {
                break;
            }
            interleaved[frame * selected + slot] = data[at];
        }
    }

    return frames;
}

// The real-time callback CoreAudio invokes.
//
// Runs on a thread with a hard deadline. It gathers the selected channels into
// a preallocated buffer and calls through; it must not allocate, lock or throw.
// noexcept is what makes the last of those a guarantee: an exception escaping
// into CoreAudio's C frames would terminate the process, so this is declared to
// do that rather than to unwind.
OSStatus io_proc(AudioObjectID /*device*/, const AudioTimeStamp* /*now*/,
                 const AudioBufferList* input, const AudioTimeStamp* /*input_time*/,
                 AudioBufferList* output, const AudioTimeStamp* /*output_time*/,
                 void* client) noexcept {
    if (client == nullptr || input == nullptr) {
        return 0;
    }
    // `client` is the pointer handed to AudioDeviceCreateIOProcID, which points
    // at the IoProcState inside a live CoreAudioStream::Impl for as long as the
    // IOProc can run.
    IoProcState& state = *static_cast<IoProcState*>(client);

    const std::size_t channels = state.selected.size();
    const std::size_t output_channels = state.outputs.size();

    std::size_t frames = gather_input(input, state.selected, state.interleaved);
    if (frames == 0) {
        // An output-only stream still has work to do, so fall back to what the
        // output side is asking for rather than returning early.
        frames = output_frames(output);
        if (frames == 0) {
            return 0;
        }
        // The scratch buffers hold kMaxBufferFrames. open() checked the device's
        // buffer size against that, but another application can change it
        // afterwards; clamping keeps the spans below inside the allocation
        // rather than aborting on a size the hardware chose.
        frames = std::min(frames, kMaxBufferFrames);
        // No input arrived to fill the gathered block, so it reads as silence
        // rather than as the previous callback's samples.
        std::fill_n(state.interleaved.begin(), frames * channels, 0.0f);
    }

    const std::span<float> writable(state.output_scratch.data(), frames * output_channels);
    // Never hand the callback stale contents: whatever was here last block would
    // otherwise play again if the callback declined to write.
    std::ranges::fill(writable, 0.0f);

    AudioBuffers buffers(std::span<const float>(state.interleaved.data(), frames * channels),
                         writable, channels, output_channels, frames);
    state.callback->process(buffers);

    if (output_channels > 0) {
        scatter_output(output, state.outputs, state.output_scratch, frames);
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

struct CoreAudioStream::Impl {
    Impl(std::unique_ptr<AudioCallback> callback, std::vector<std::uint32_t> selected,
         std::vector<std::uint32_t> outputs)
        : state(std::move(callback), std::move(selected), std::move(outputs)) {}

    // Null until install_io_proc() succeeds.
    AudioDeviceIOProcID proc_id = nullptr;
    // CoreAudio holds a raw pointer to this member from install_io_proc() until
    // the destructor has destroyed the IOProc, so it must not be moved or
    // replaced in between - which owning it through a unique_ptr guarantees.
    IoProcState state;
};

CoreAudioStream::CoreAudioStream(std::uint32_t device, StreamConfig granted, StreamLatency latency,
                                 std::unique_ptr<AudioCallback> callback)
    : device_(device),
      config_(std::move(granted)),
      latency_(latency),
      // Every buffer the IOProc touches is allocated here, once.
      impl_(std::make_unique<Impl>(std::move(callback), config_.input_channels,
                                   config_.output_channels)) {}

void CoreAudioStream::install_io_proc() {
    AudioDeviceIOProcID proc_id = nullptr;
    const OSStatus status = AudioDeviceCreateIOProcID(device_, io_proc, &impl_->state, &proc_id);
    if (status != 0 || proc_id == nullptr) {
        throw BackendError("AudioDeviceCreateIOProcID failed with OSStatus " +
                           std::to_string(status));
    }
    impl_->proc_id = proc_id;
}

CoreAudioStream::~CoreAudioStream() {
    // A destructor must not throw, and a failure to stop a stream that is being
    // thrown away has nowhere useful to go.
    try {
        stop();
    } catch (...) {
    }
    // Must happen before impl_ is freed: the IOProc holds a pointer into it. By
    // now the device is stopped, so the audio thread is no longer inside it.
    if (impl_->proc_id != nullptr) {
        AudioDeviceDestroyIOProcID(device_, impl_->proc_id);
    }
}

void CoreAudioStream::start() {
    if (running_) {
        throw AlreadyRunningError();
    }
    const OSStatus status = AudioDeviceStart(device_, impl_->proc_id);
    if (status != 0) {
        throw describe_status(status, "AudioDeviceStart");
    }
    running_ = true;
}

void CoreAudioStream::stop() {
    if (!running_) {
        return;
    }
    // Stopping an already-stopped device is harmless.
    const OSStatus status = AudioDeviceStop(device_, impl_->proc_id);
    running_ = false;
    if (status != 0) {
        throw describe_status(status, "AudioDeviceStop");
    }
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

namespace detail {

std::optional<std::uint32_t> resolve_uid(std::string_view uid) {
    for (const AudioDeviceID id : all_device_ids()) {
        if (cfstring_property(id, kAudioDevicePropertyDeviceUID) == uid) {
            return id;
        }
    }
    return std::nullopt;
}

}  // namespace detail

std::vector<DeviceInfo> CoreAudioBackend::devices() const {
    const auto default_in = default_device(kAudioHardwarePropertyDefaultInputDevice);
    const auto default_out = default_device(kAudioHardwarePropertyDefaultOutputDevice);
    std::vector<DeviceInfo> devices;
    for (const AudioDeviceID id : all_device_ids()) {
        if (auto info = describe(id, default_in, default_out)) {
            devices.push_back(std::move(*info));
        }
    }
    return devices;
}

std::unique_ptr<CoreAudioStream> CoreAudioBackend::open_input(
    const StreamConfig& config, std::unique_ptr<AudioCallback> callback) {
    config.validate();

    // Input and output must be the same device.
    //
    // An IOProc belongs to one device, so two devices means two procs on two
    // clocks, and a transfer function measured across unsynchronised clocks
    // drifts in phase until it is meaningless. macOS already solves this
    // with aggregate devices, which present two interfaces as one clock
    // domain - so the answer is to point at an aggregate, not to pretend two
    // procs are one.
    if (config.input && config.output && *config.input != *config.output) {
        throw BackendError("input '" + config.input->str() + "' and output '" +
                           config.output->str() +
                           "' are different devices; create an aggregate device in Audio MIDI "
                           "Setup so they share a clock");
    }
    if (!config.input) {
        throw DeviceNotFoundError("no input device selected");
    }
    const std::string& uid = config.input->str();
    const auto resolved = detail::resolve_uid(uid);
    if (!resolved) {
        throw DeviceNotFoundError(uid);
    }
    const AudioDeviceID device = *resolved;

    const std::string name = cfstring_property(device, kAudioObjectPropertyName).value_or(uid);
    const std::uint32_t available = channel_count(device, kAudioObjectPropertyScopeInput);
    if (available == 0 && !config.input_channels.empty()) {
        throw ChannelOutOfRangeError(name, 0, 0);
    }
    for (const std::uint32_t channel : config.input_channels) {
        if (channel >= available) {
            throw ChannelOutOfRangeError(name, channel, available);
        }
    }

    const std::uint32_t outputs_available = channel_count(device, kAudioObjectPropertyScopeOutput);
    for (const std::uint32_t channel : config.output_channels) {
        if (channel >= outputs_available) {
            throw ChannelOutOfRangeError(name, channel, outputs_available);
        }
    }

    set_sample_rate(device, config.sample_rate, name);
    const std::uint32_t granted_frames = set_buffer_frames(device, config.buffer_frames);

    StreamConfig granted = config;
    granted.buffer_frames = granted_frames;
    granted.sample_rate = scalar<double>(device, kAudioDevicePropertyNominalSampleRate,
                                         kAudioObjectPropertyScopeGlobal)
                              .value_or(config.sample_rate);

    // The stream's constructor is private, so make_unique cannot reach it. If
    // install_io_proc() throws, the stream's destructor still runs and frees
    // the state; nothing is registered with CoreAudio yet to undo.
    std::unique_ptr<CoreAudioStream> stream(
        new CoreAudioStream(device, std::move(granted), read_latency(device), std::move(callback)));
    stream->install_io_proc();
    return stream;
}

std::unique_ptr<AudioStream> CoreAudioBackend::open(const StreamConfig& config,
                                                    std::unique_ptr<AudioCallback> callback) {
    return open_input(config, std::move(callback));
}

}  // namespace analyzer::audio

#endif  // ANALYZER_AUDIO_COREAUDIO
