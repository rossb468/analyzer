// What the Rust wav tests could lean on hound for: the error paths of the
// reader, and the exact bytes of the header the writer produces.

#include "model/wav.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "model/error.hpp"
#include "test_util.hpp"

namespace analyzer::model {
namespace {

using test::read_bytes;
using test::scratch;

std::uint32_t u32_at(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 4; i-- > 0;) {
        value = (value << 8) | std::to_integer<std::uint32_t>(bytes[offset + i]);
    }
    return value;
}

std::uint16_t u16_at(const std::vector<std::byte>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset]) |
                                      (std::to_integer<unsigned>(bytes[offset + 1]) << 8));
}

TEST(WavHeader, SixteenBitIsTheClassicPcmHeader) {
    const auto path = scratch("pcm.wav");
    write_wav(path, std::vector<float>(10, 0.0f), 48'000.0f, SampleDepth::Int16);
    const std::vector<std::byte> bytes = read_bytes(path);

    ASSERT_EQ(bytes.size(), 44u + 20u);
    EXPECT_EQ(u32_at(bytes, 4), 36u + 20u) << "RIFF size";
    EXPECT_EQ(u32_at(bytes, 16), 16u) << "fmt size";
    EXPECT_EQ(u16_at(bytes, 20), 1u) << "WAVE_FORMAT_PCM";
    EXPECT_EQ(u32_at(bytes, 24), 48'000u);
    EXPECT_EQ(u16_at(bytes, 34), 16u);
    EXPECT_EQ(u32_at(bytes, 40), 20u) << "data size";
    std::filesystem::remove(path);
}

TEST(WavHeader, TwentyFourBitAndFloatAreExtensible) {
    for (const auto& [depth, tag, width] :
         {std::tuple{SampleDepth::Int24, 1u, 3u}, std::tuple{SampleDepth::Float32, 3u, 4u}}) {
        const auto path = scratch(std::string(as_key(depth)) + ".wav");
        write_wav(path, std::vector<float>(10, 0.0f), 44'100.0f, depth);
        const std::vector<std::byte> bytes = read_bytes(path);

        ASSERT_EQ(bytes.size(), 68u + 10u * width) << as_key(depth);
        EXPECT_EQ(u32_at(bytes, 4), 60u + 10u * width) << "RIFF size";
        EXPECT_EQ(u32_at(bytes, 16), 40u) << "fmt size";
        EXPECT_EQ(u16_at(bytes, 20), 0xFFFEu) << "WAVE_FORMAT_EXTENSIBLE";
        EXPECT_EQ(u32_at(bytes, 24), 44'100u);
        EXPECT_EQ(u32_at(bytes, 28), 44'100u * width) << "bytes per second";
        EXPECT_EQ(u16_at(bytes, 32), width) << "block align";
        EXPECT_EQ(u16_at(bytes, 34), width * 8) << "container bits";
        EXPECT_EQ(u16_at(bytes, 36), 22u) << "extension size";
        EXPECT_EQ(u16_at(bytes, 38), width * 8) << "valid bits";
        EXPECT_EQ(u32_at(bytes, 40), 1u) << "channel mask";
        EXPECT_EQ(u32_at(bytes, 44), tag) << "sub-format";
        EXPECT_EQ(u32_at(bytes, 64), 10u * width) << "data size";
        std::filesystem::remove(path);
    }
}

TEST(WavRead, AMissingFileIsAnIoError) {
    const auto path = scratch("does-not-exist.wav");
    try {
        read_wav(path);
        FAIL() << "expected an IoError";
    } catch (const IoError& error) {
        EXPECT_NE(std::string(error.what()).find("opening"), std::string::npos) << error.what();
    }
}

TEST(WavRead, ANonWavFileIsAnIoError) {
    const auto path = scratch("not-a-wav.wav");
    {
        std::ofstream file(path, std::ios::binary);
        file << "this is not a RIFF file at all";
    }
    EXPECT_THROW(read_wav(path), IoError);
    std::filesystem::remove(path);
}

TEST(WavRead, ADataChunkShorterThanItsHeaderClaimsIsAnIoError) {
    const auto path = scratch("short.wav");
    write_wav(path, std::vector<float>(100, 0.25f), 48'000.0f, SampleDepth::Float32);
    std::vector<std::byte> bytes = read_bytes(path);
    bytes.resize(bytes.size() - 40);
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    EXPECT_THROW(read_wav(path), IoError);
    std::filesystem::remove(path);
}

TEST(WavWrite, AnUnwritablePathIsAnIoError) {
    const auto path = scratch("no-such-directory") / "out.wav";
    EXPECT_THROW(write_wav(path, std::vector<float>(4, 0.0f), 48'000.0f, SampleDepth::Float32),
                 IoError);
}

}  // namespace
}  // namespace analyzer::model
