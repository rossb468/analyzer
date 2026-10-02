// dr_wav is a single header; this is the one translation unit that holds its
// implementation. Stdio is left out because the file is read into memory here,
// where a std::filesystem::path can be opened on every platform.
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "model/wav.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include "base/number_text.hpp"
#include "model/error.hpp"

namespace analyzer::model {

namespace {

constexpr std::size_t kPcmHeaderBytes = 44;
constexpr std::size_t kExtensibleHeaderBytes = 68;

// Samples converted and written at a time, so a long file is never held twice.
constexpr std::size_t kChunkSamples = 16'384;

std::size_t bytes_per_sample(SampleDepth depth) noexcept {
    switch (depth) {
        case SampleDepth::Int16: return 2;
        case SampleDepth::Int24: return 3;
        case SampleDepth::Float32: return 4;
    }
    return 4;
}

std::uint16_t bits(SampleDepth depth) noexcept {
    return static_cast<std::uint16_t>(bytes_per_sample(depth) * 8);
}

std::string os_error() {
    return errno == 0 ? "unknown error" : std::error_code(errno, std::generic_category()).message();
}

std::string describe(const std::filesystem::path& path) {
    return path.string();
}

void put_u16(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    put_u16(out, value & 0xFFFF);
    put_u16(out, value >> 16);
}

void put_text(std::vector<std::byte>& out, std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// The first 16 bits of a WAVE_FORMAT_EXTENSIBLE sub-format GUID are the format
// tag; the rest is the fixed KSDATAFORMAT_SUBTYPE base shared by PCM and float.
void put_sub_format(std::vector<std::byte>& out, std::uint32_t tag) {
    put_u32(out, tag);
    constexpr std::array<unsigned char, 12> kBase = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                                     0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    for (const unsigned char byte : kBase) {
        out.push_back(static_cast<std::byte>(byte));
    }
}

// The mono header the Rust core's writer (hound) produced, which dr_wav's
// writer cannot: dr_wav refuses WAVE_FORMAT_EXTENSIBLE and always writes a
// 16-byte "fmt " chunk, but other tools expect more than 16 bits to arrive
// as the extensible form, and the golden files record exactly that.
std::vector<std::byte> header(SampleDepth depth, std::uint32_t sample_rate,
                              std::uint32_t data_bytes) {
    const bool extensible = depth != SampleDepth::Int16;
    const std::uint32_t width = static_cast<std::uint32_t>(bytes_per_sample(depth));
    std::vector<std::byte> out;
    out.reserve(kExtensibleHeaderBytes);

    put_text(out, "RIFF");
    const std::size_t header_bytes = extensible ? kExtensibleHeaderBytes : kPcmHeaderBytes;
    put_u32(out, data_bytes + static_cast<std::uint32_t>(header_bytes - 8));
    put_text(out, "WAVE");
    put_text(out, "fmt ");
    if (!extensible) {
        put_u32(out, 16);
        put_u16(out, 1);  // WAVE_FORMAT_PCM
    } else {
        put_u32(out, 40);
        put_u16(out, 0xFFFE);  // WAVE_FORMAT_EXTENSIBLE
    }
    put_u16(out, 1);  // mono
    put_u32(out, sample_rate);
    put_u32(out, sample_rate * width);  // bytes per second
    put_u16(out, width);                // block align
    put_u16(out, bits(depth));          // the container size
    if (extensible) {
        put_u16(out, 22);           // bytes remaining in the extension
        put_u16(out, bits(depth));  // valid bits: the whole container
        put_u32(out, 1);            // channel mask: front left
        put_sub_format(out, depth == SampleDepth::Float32 ? 3 : 1);
    }
    put_text(out, "data");
    put_u32(out, data_bytes);
    return out;
}

// Scale a float sample to an integer of `magnitude_bits` magnitude bits.
//
// Clamped rather than wrapped; see write_wav.
std::int32_t quantise(float sample, int magnitude_bits) {
    const auto peak = static_cast<float>((std::int64_t{1} << magnitude_bits) - 1);
    return analyzer::saturating_cast<std::int32_t>(
        std::round(std::clamp(sample, -1.0f, 1.0f) * peak));
}

void encode(std::span<const float> samples, SampleDepth depth, std::vector<char>& out) {
    out.clear();
    out.reserve(samples.size() * bytes_per_sample(depth));
    const auto push = [&out](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
        }
    };
    for (const float sample : samples) {
        switch (depth) {
            case SampleDepth::Float32: push(std::bit_cast<std::uint32_t>(sample), 4); break;
            case SampleDepth::Int16:
                push(static_cast<std::uint32_t>(quantise(sample, 15)), 2);
                break;
            case SampleDepth::Int24:
                push(static_cast<std::uint32_t>(quantise(sample, 23)), 3);
                break;
        }
    }
}

std::vector<char> read_file(const std::filesystem::path& path) {
    errno = 0;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw IoError("opening " + describe(path) + ": " + os_error());
    }
    const std::streamoff size = file.tellg();
    file.seekg(0);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.read(bytes.data(), size);
    if (!file) {
        throw IoError("reading " + describe(path) + ": " + os_error());
    }
    return bytes;
}

// Closes the dr_wav reader however the function leaves.
struct Reader {
    drwav wav{};
    bool open = false;

    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader() {
        if (open) {
            drwav_uninit(&wav);
        }
    }
};

}  // namespace

std::string_view as_key(SampleDepth depth) noexcept {
    switch (depth) {
        case SampleDepth::Int16: return "i16";
        case SampleDepth::Int24: return "i24";
        case SampleDepth::Float32: return "f32";
    }
    return "f32";
}

std::optional<SampleDepth> depth_from_key(std::string_view key) noexcept {
    if (key == "i16" || key == "16") {
        return SampleDepth::Int16;
    }
    if (key == "i24" || key == "24") {
        return SampleDepth::Int24;
    }
    if (key == "f32" || key == "float" || key == "32") {
        return SampleDepth::Float32;
    }
    return std::nullopt;
}

std::vector<float> render(dsp::Signal signal, float sample_rate, float seconds) {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0f) {
        throw BadParameterError("sample rate must be positive, got " + text::shortest(sample_rate));
    }
    if (!std::isfinite(seconds) || seconds <= 0.0f) {
        throw BadParameterError("duration must be positive, got " + text::shortest(seconds));
    }

    const double frames =
        std::round(static_cast<double>(seconds) * static_cast<double>(sample_rate));
    if (frames > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw BadParameterError("that is more audio than a WAV file can hold");
    }

    dsp::Generator generator(sample_rate, signal, kGeneratorSeed);
    std::vector<float> out(static_cast<std::size_t>(frames), 0.0f);
    generator.fill(out);
    return out;
}

void write_wav(const std::filesystem::path& path, std::span<const float> samples, float sample_rate,
               SampleDepth depth) {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0f) {
        throw BadParameterError("sample rate must be positive, got " + text::shortest(sample_rate));
    }
    // The chunk sizes are 32-bit, and so is the RIFF size that includes them.
    const std::uint64_t data_bytes = std::uint64_t{samples.size()} * bytes_per_sample(depth);
    if (data_bytes > std::numeric_limits<std::uint32_t>::max() - kExtensibleHeaderBytes) {
        throw BadParameterError("that is more audio than a WAV file can hold");
    }

    errno = 0;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw IoError("creating " + describe(path) + ": " + os_error());
    }

    const std::vector<std::byte> head =
        header(depth, analyzer::saturating_cast<std::uint32_t>(sample_rate),
               static_cast<std::uint32_t>(data_bytes));
    file.write(reinterpret_cast<const char*>(head.data()),
               static_cast<std::streamsize>(head.size()));

    std::vector<char> chunk;
    for (std::size_t start = 0; start < samples.size(); start += kChunkSamples) {
        encode(samples.subspan(start, std::min(kChunkSamples, samples.size() - start)), depth,
               chunk);
        file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }

    file.close();
    if (!file) {
        throw IoError("finishing " + describe(path) + ": " + os_error());
    }
}

std::size_t write_signal(const std::filesystem::path& path, dsp::Signal signal, float sample_rate,
                         float seconds, SampleDepth depth) {
    const std::vector<float> samples = render(signal, sample_rate, seconds);
    write_wav(path, samples, sample_rate, depth);
    return samples.size();
}

WavFile read_wav(const std::filesystem::path& path) {
    const std::vector<char> bytes = read_file(path);

    Reader reader;
    if (!drwav_init_memory(&reader.wav, bytes.data(), bytes.size(), nullptr)) {
        throw IoError("opening " + describe(path) + ": not a readable WAV file");
    }
    reader.open = true;
    const drwav& wav = reader.wav;

    const bool is_float = wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT;
    const bool is_int = wav.translatedFormatTag == DR_WAVE_FORMAT_PCM;
    const std::uint16_t width = wav.bitsPerSample;
    const bool supported =
        (is_float && width == 32) || (is_int && (width == 16 || width == 24 || width == 32));
    if (!supported) {
        throw UnsupportedFormatError(
            "unsupported WAV format: " + std::string(is_float ? "Float" : "Int") + " at " +
            std::to_string(width) + " bits per sample");
    }
    if (wav.channels == 0) {
        throw IoError(describe(path) + " declares zero channels");
    }

    WavFile file;
    file.channels = wav.channels;
    file.sample_rate = static_cast<double>(wav.sampleRate);
    file.bits_per_sample = width;
    file.is_float = is_float;

    // dr_wav quietly shortens a data chunk that claims more than the file holds,
    // so it has to be asked what the chunk claimed. Reading half a recording
    // without a word is how a measurement ends up wrong; the Rust reader's
    // "unexpected end of file" is the behaviour kept. The size field is the four
    // bytes just before the first sample in a RIFF file.
    if (wav.container == drwav_container_riff && wav.dataChunkDataPos >= 4) {
        const auto start = static_cast<std::size_t>(wav.dataChunkDataPos);
        std::uint64_t declared = 0;
        for (std::size_t i = 4; i-- > 0;) {
            declared = (declared << 8) | static_cast<unsigned char>(bytes[start - 4 + i]);
        }
        if (declared > bytes.size() - start) {
            throw IoError("reading " + describe(path) + ": unexpected end of file");
        }
    }

    // The frame count is the file's claim, not a fact: a corrupt one must not
    // become a multi-gigabyte allocation before any data is read.
    const std::uint64_t frames = wav.totalPCMFrameCount;
    const std::uint64_t frame_bytes = std::uint64_t{width / 8u} * wav.channels;
    if (frames > bytes.size() / frame_bytes) {
        throw IoError("reading " + describe(path) + ": unexpected end of file");
    }
    const std::uint64_t count = frames * wav.channels;
    file.samples.resize(static_cast<std::size_t>(count));

    // Read at the file's own width and normalise here, so the divisor is
    // exactly the one the Rust reader used rather than whatever dr_wav's
    // conversion helpers choose.
    std::uint64_t got = 0;
    if (is_float) {
        got = drwav_read_pcm_frames(&reader.wav, frames, file.samples.data());
    } else if (width == 16) {
        std::vector<std::int16_t> raw(file.samples.size());
        got = drwav_read_pcm_frames_s16(&reader.wav, frames, raw.data());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            file.samples[i] = static_cast<float>(raw[i]) / 32768.0f;
        }
    } else {
        // dr_wav hands back 24-bit samples shifted up into 32 bits, which is the
        // same number scaled by 2^8, so one divisor serves both widths.
        std::vector<std::int32_t> raw(file.samples.size());
        got = drwav_read_pcm_frames_s32(&reader.wav, frames, raw.data());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            file.samples[i] = static_cast<float>(raw[i]) / 2147483648.0f;
        }
    }
    if (got != frames) {
        throw IoError("reading " + describe(path) + ": unexpected end of file");
    }
    return file;
}

}  // namespace analyzer::model
