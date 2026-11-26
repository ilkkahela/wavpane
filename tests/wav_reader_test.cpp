#include <gtest/gtest.h>

#include "../wav_reader.hpp"
#include "../audio_loader.hpp"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

void push16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}
void push32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}

// The 16-byte core of a fmt chunk. audioFormat: 1 = PCM, 3 = IEEE float.
std::vector<uint8_t> fmtCore(uint16_t audioFormat, uint16_t numChannels, uint32_t sampleRate,
                             uint16_t bitsPerSample) {
    std::vector<uint8_t> fmt;
    push16(fmt, audioFormat);
    push16(fmt, numChannels);
    push32(fmt, sampleRate);
    uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    push32(fmt, byteRate);
    push16(fmt, (uint16_t)(numChannels * (bitsPerSample / 8)));
    push16(fmt, bitsPerSample);
    return fmt;
}

// RIFF/WAVE byte stream from explicit fmt bytes: optional extra chunks
// first, then fmt, then data.
std::vector<uint8_t> buildWavFromFmt(const std::vector<uint8_t>& fmt, const std::vector<uint8_t>& dataBytes,
                                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extraChunks = {}) {
    std::vector<uint8_t> body;
    for (const auto& [id, chunkBytes] : extraChunks) {
        body.insert(body.end(), id.begin(), id.end());
        push32(body, (uint32_t)chunkBytes.size());
        body.insert(body.end(), chunkBytes.begin(), chunkBytes.end());
        if (chunkBytes.size() % 2) body.push_back(0);
    }
    body.insert(body.end(), {'f', 'm', 't', ' '});
    push32(body, (uint32_t)fmt.size());
    body.insert(body.end(), fmt.begin(), fmt.end());
    if (fmt.size() % 2) body.push_back(0);

    body.insert(body.end(), {'d', 'a', 't', 'a'});
    push32(body, (uint32_t)dataBytes.size());
    body.insert(body.end(), dataBytes.begin(), dataBytes.end());
    if (dataBytes.size() % 2) body.push_back(0);

    std::vector<uint8_t> out;
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    push32(out, (uint32_t)(4 + body.size()));
    out.insert(out.end(), {'W', 'A', 'V', 'E'});
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Minimal, valid RIFF/WAVE byte stream with a plain 16-byte fmt chunk.
std::vector<uint8_t> buildWav(uint16_t audioFormat, uint16_t numChannels, uint32_t sampleRate,
                               uint16_t bitsPerSample, const std::vector<uint8_t>& dataBytes,
                               const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extraChunks = {}) {
    return buildWavFromFmt(fmtCore(audioFormat, numChannels, sampleRate, bitsPerSample), dataBytes, extraChunks);
}

// Overwrites the data chunk's declared size (e.g. to fake a streamed or
// truncated file) without changing the bytes that follow it.
void patchDataSize(std::vector<uint8_t>& wav, uint32_t size) {
    for (size_t i = 12; i + 8 <= wav.size(); ++i) {
        if (std::memcmp(&wav[i], "data", 4) == 0) {
            for (int b = 0; b < 4; ++b) wav[i + 4 + b] = (uint8_t)((size >> (8 * b)) & 0xFF);
            return;
        }
    }
}

std::vector<uint8_t> floatBytes(std::initializer_list<float> vs) {
    std::vector<uint8_t> out;
    for (float f : vs) { uint8_t b[4]; std::memcpy(b, &f, 4); out.insert(out.end(), b, b + 4); }
    return out;
}


std::string writeTempFile(const std::vector<uint8_t>& bytes, const std::string& suffix) {
    auto path = std::filesystem::temp_directory_path() / ("wavpane_wavtest_" + suffix + ".wav");
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    f.close();
    return path.string();
}

wav::WavData readBytes(const std::vector<uint8_t>& bytes, const std::string& suffix) {
    auto path = writeTempFile(bytes, suffix);
    wav::WavReader reader;
    try {
        auto wd = reader.read(path);
        std::filesystem::remove(path);
        return wd;
    } catch (...) {
        std::filesystem::remove(path);
        throw;
    }
}

} // namespace

TEST(WavReader, Mono16Basic) {
    std::vector<uint8_t> data;
    for (int16_t v : {(int16_t)0, (int16_t)32767, (int16_t)-32768, (int16_t)-16384, (int16_t)16384})
        push16(data, (uint16_t)v);
    auto path = writeTempFile(buildWav(1, 1, 44100, 16, data), "mono16");

    wav::WavReader reader;
    wav::WavData wd = reader.read(path);
    std::filesystem::remove(path);

    EXPECT_EQ(wd.sampleRate, 44100u);
    EXPECT_EQ(wd.numChannels, 1u);
    EXPECT_EQ(wd.bitsPerSample, 16u);
    ASSERT_EQ(wd.frameCount(), 5u);
    EXPECT_NEAR(wd.channels[0][0], 0.0f, 1e-6f);
    EXPECT_NEAR(wd.channels[0][1], 32767.0f / 32768.0f, 1e-6f);
    EXPECT_NEAR(wd.channels[0][2], -1.0f, 1e-6f);
    EXPECT_NEAR(wd.channels[0][3], -0.5f, 1e-6f);
    EXPECT_NEAR(wd.channels[0][4], 0.5f, 1e-6f);
}

TEST(WavReader, StereoDeinterleaving) {
    std::vector<uint8_t> data;
    for (int i = 0; i < 4; ++i) {
        push16(data, (uint16_t)(int16_t)(i * 1000));
        push16(data, (uint16_t)(int16_t)(-i * 1000));
    }
    auto path = writeTempFile(buildWav(1, 2, 48000, 16, data), "stereo16");

    wav::WavReader reader;
    wav::WavData wd = reader.read(path);
    std::filesystem::remove(path);

    ASSERT_EQ(wd.numChannels, 2u);
    ASSERT_EQ(wd.frameCount(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(wd.channels[0][i],  (float)(i * 1000) / 32768.0f, 1e-5f) << "left, frame " << i;
        EXPECT_NEAR(wd.channels[1][i], -(float)(i * 1000) / 32768.0f, 1e-5f) << "right, frame " << i;
    }
}

TEST(WavReader, DropsATrailingPartialFrame) {
    // 3 full stereo 16-bit frames + one dangling left sample: the reader must
    // keep the channels the same length and not read past the data.
    std::vector<uint8_t> data;
    for (int i = 0; i < 3; ++i) { push16(data, (uint16_t)(int16_t)(1000 * (i + 1))); push16(data, 0); }
    push16(data, (uint16_t)(int16_t)12345);
    auto path = writeTempFile(buildWav(1, 2, 44100, 16, data), "partialframe");

    wav::WavReader reader;
    auto wd = reader.read(path);
    std::filesystem::remove(path);

    ASSERT_EQ(wd.channels.size(), 2u);
    EXPECT_EQ(wd.channels[0].size(), 3u);
    EXPECT_EQ(wd.channels[1].size(), 3u);
    EXPECT_NEAR(wd.channels[0][2], 3000.0f / 32768.0f, 1e-5f);
}

TEST(WavReader, BitDepthDecoding) {
    { // 8-bit unsigned, centered at 128
        std::vector<uint8_t> data = {0, 128, 255};
        auto path = writeTempFile(buildWav(1, 1, 8000, 8, data), "u8");
        wav::WavReader reader;
        auto wd = reader.read(path);
        std::filesystem::remove(path);
        ASSERT_EQ(wd.frameCount(), 3u);
        EXPECT_NEAR(wd.channels[0][0], -1.0f, 1e-6f);
        EXPECT_NEAR(wd.channels[0][1],  0.0f, 1e-6f);
        EXPECT_NEAR(wd.channels[0][2],  127.0f / 128.0f, 1e-6f);
    }
    { // 24-bit signed, little-endian, sign-extended
        std::vector<uint8_t> data;
        for (int32_t v : {0, 8388607, -8388608}) {
            data.push_back((uint8_t)(v & 0xFF));
            data.push_back((uint8_t)((v >> 8) & 0xFF));
            data.push_back((uint8_t)((v >> 16) & 0xFF));
        }
        auto path = writeTempFile(buildWav(1, 1, 44100, 24, data), "s24");
        wav::WavReader reader;
        auto wd = reader.read(path);
        std::filesystem::remove(path);
        ASSERT_EQ(wd.frameCount(), 3u);
        EXPECT_NEAR(wd.channels[0][0], 0.0f, 1e-6f);
        EXPECT_NEAR(wd.channels[0][1], 8388607.0f / 8388608.0f, 1e-6f);
        EXPECT_NEAR(wd.channels[0][2], -1.0f, 1e-6f);
    }
    { // 32-bit IEEE float, passthrough
        std::vector<uint8_t> data;
        for (float f : {0.25f, -0.75f}) {
            uint8_t b[4];
            std::memcpy(b, &f, 4);
            data.insert(data.end(), b, b + 4);
        }
        auto path = writeTempFile(buildWav(3, 1, 44100, 32, data), "f32");
        wav::WavReader reader;
        auto wd = reader.read(path);
        std::filesystem::remove(path);
        ASSERT_EQ(wd.frameCount(), 2u);
        EXPECT_NEAR(wd.channels[0][0], 0.25f, 1e-6f);
        EXPECT_NEAR(wd.channels[0][1], -0.75f, 1e-6f);
    }
}

// Real-world WAV files (e.g. ffmpeg output) carry a LIST/INFO metadata chunk
// alongside fmt/data. This also exercises the word-alignment padding path
// (chunk sizes must round up to an even byte count).
TEST(WavReader, SkipsUnknownChunksIncludingOddSizedPadding) {
    std::vector<uint8_t> data;
    push16(data, (uint16_t)(int16_t)1000);
    push16(data, (uint16_t)(int16_t)-1000);

    std::vector<uint8_t> listBytes = {'I', 'N', 'F', 'O', 'x'}; // odd size, needs padding
    auto path = writeTempFile(buildWav(1, 1, 22050, 16, data, {{"LIST", listBytes}}), "listchunk");

    wav::WavReader reader;
    auto wd = reader.read(path);
    std::filesystem::remove(path);

    EXPECT_EQ(wd.sampleRate, 22050u);
    ASSERT_EQ(wd.frameCount(), 2u);
    EXPECT_NEAR(wd.channels[0][0], 1000.0f / 32768.0f, 1e-5f);
    EXPECT_NEAR(wd.channels[0][1], -1000.0f / 32768.0f, 1e-5f);
}

TEST(WavReader, ThrowsOnInvalidInput) {
    wav::WavReader reader;

    EXPECT_THROW(reader.read("/nonexistent/path/does_not_exist.wav"), std::runtime_error);

    {
        std::vector<uint8_t> junk = {'X', 'Y', 'Z', '!', 0, 0, 0, 0};
        auto path = writeTempFile(junk, "notriff");
        EXPECT_THROW(reader.read(path), std::runtime_error);
        std::filesystem::remove(path);
    }
    {
        // Valid RIFF/WAVE/fmt but a zero-length data chunk.
        auto path = writeTempFile(buildWav(1, 1, 44100, 16, {}), "nodata");
        EXPECT_THROW(reader.read(path), std::runtime_error);
        std::filesystem::remove(path);
    }
}

// Regression: the reader trusted the declared data size. Streamed writers
// (ffmpeg to a pipe) leave it at 0xFFFFFFFF -- a 4 GiB allocation -- and a
// truncated file got its missing tail padded with zeros.
TEST(WavReader, ClampsBogusDataSizesToTheBytesInTheFile) {
    std::vector<uint8_t> data;
    for (int16_t v : {(int16_t)1000, (int16_t)2000, (int16_t)3000}) push16(data, (uint16_t)v);
    for (uint32_t declared : {0xFFFFFFFFu, (uint32_t)data.size() + 100u}) {
        auto bytes = buildWav(1, 1, 44100, 16, data);
        patchDataSize(bytes, declared);
        auto wd = readBytes(bytes, "bogussize");
        ASSERT_EQ(wd.frameCount(), 3u) << "declared=" << declared;
        EXPECT_NEAR(wd.channels[0][2], 3000.0f / 32768.0f, 1e-5f);
    }
}

// Regression: WAVE_FORMAT_EXTENSIBLE (0xFFFE) float data was decoded as
// 32-bit integers -- garbage near zero.
TEST(WavReader, DecodesExtensibleAnd64BitFloat) {
    {
        std::vector<uint8_t> fmt = fmtCore(0xFFFE, 1, 48000, 32);
        push16(fmt, 22);                 // cbSize
        push16(fmt, 32);                 // valid bits per sample
        push32(fmt, 0x4);                // channel mask
        push16(fmt, 3);                  // SubFormat GUID: KSDATAFORMAT_SUBTYPE_IEEE_FLOAT...
        for (int i = 0; i < 14; ++i) fmt.push_back(0);
        ASSERT_EQ(fmt.size(), 40u);
        auto wd = readBytes(buildWavFromFmt(fmt, floatBytes({0.25f, -0.75f})), "extfloat");
        ASSERT_EQ(wd.frameCount(), 2u);
        EXPECT_FLOAT_EQ(wd.channels[0][0], 0.25f);
        EXPECT_FLOAT_EQ(wd.channels[0][1], -0.75f);
    }
    {
        std::vector<uint8_t> data;
        for (double d : {0.5, -0.125}) { uint8_t b[8]; std::memcpy(b, &d, 8); data.insert(data.end(), b, b + 8); }
        auto wd = readBytes(buildWav(3, 1, 48000, 64, data), "f64");
        ASSERT_EQ(wd.frameCount(), 2u);
        EXPECT_FLOAT_EQ(wd.channels[0][0], 0.5f);
        EXPECT_FLOAT_EQ(wd.channels[0][1], -0.125f);
    }
}

// Regression: NaN/Inf in a float file reached the pyramid and histogram,
// poisoning min/max and (for NaN) indexing out of bounds.
TEST(WavReader, ReplacesNonFiniteFloatsWithZero) {
    auto wd = readBytes(buildWav(3, 1, 44100, 32,
                                 floatBytes({std::nanf(""), INFINITY, -INFINITY, 0.5f})), "nonfinite");
    ASSERT_EQ(wd.frameCount(), 4u);
    EXPECT_EQ(wd.channels[0][0], 0.0f);
    EXPECT_EQ(wd.channels[0][1], 0.0f);
    EXPECT_EQ(wd.channels[0][2], 0.0f);
    EXPECT_EQ(wd.channels[0][3], 0.5f);
}

// Regression: unsupported encodings decoded silently as all-zero audio, and
// a zero sample rate made every time computation divide by zero.
TEST(WavReader, RejectsFormatsItCannotDecode) {
    std::vector<uint8_t> data(16, 0x11);
    EXPECT_THROW(readBytes(buildWav(1, 1, 44100, 12, data), "pcm12"), std::runtime_error);
    EXPECT_THROW(readBytes(buildWav(2, 1, 44100, 4, data), "adpcm"), std::runtime_error);
    EXPECT_THROW(readBytes(buildWav(3, 1, 44100, 16, data), "float16"), std::runtime_error);
    EXPECT_THROW(readBytes(buildWav(1, 1, 0, 16, data), "rate0"), std::runtime_error);
}

// ─── loadAudio (what the app actually opens: command line or drag-and-drop) ──

TEST(LoadAudio, DownmixesStereoToTheChannelMean) {
    std::vector<uint8_t> data;
    for (auto [l, r] : {std::pair{16384, 0}, std::pair{-16384, 16384}, std::pair{8192, 8192}}) {
        push16(data, (uint16_t)(int16_t)l);
        push16(data, (uint16_t)(int16_t)r);
    }
    auto path = writeTempFile(buildWav(1, 2, 48000, 16, data), "loadstereo");
    LoadedAudio a = loadAudio(path);
    std::filesystem::remove(path);

    EXPECT_DOUBLE_EQ(a.sampleRate, 48000.0);
    ASSERT_EQ(a.mono.size(), 3u);
    EXPECT_NEAR(a.mono[0], 0.25f, 1e-6f);
    EXPECT_NEAR(a.mono[1], 0.0f, 1e-6f);
    EXPECT_NEAR(a.mono[2], 0.25f, 1e-6f);
    EXPECT_NEAR(a.globalPeak, 0.25f, 1e-6f);
    EXPECT_EQ(a.pyramid.n, 3);                    // pyramid built over the mono mix
}

TEST(LoadAudio, KeepsMonoAsIsAndNamesTheFileWithoutItsDirectory) {
    auto path = writeTempFile(buildWav(3, 1, 44100, 32, floatBytes({0.5f, -0.75f})), "loadmono");
    LoadedAudio a = loadAudio(path);
    std::filesystem::remove(path);

    ASSERT_EQ(a.mono.size(), 2u);
    EXPECT_EQ(a.mono[0], 0.5f);
    EXPECT_EQ(a.mono[1], -0.75f);
    EXPECT_FLOAT_EQ(a.globalPeak, 0.75f);
    EXPECT_NEAR(a.duration, 2.0 / 44100.0, 1e-12);
    EXPECT_EQ(a.name, "wavpane_wavtest_loadmono.wav"); // title bar shows no directories
}

TEST(LoadAudio, SilenceUsesAUnitPeakSoTheFadeHasAReference) {
    auto path = writeTempFile(buildWav(1, 1, 8000, 16, std::vector<uint8_t>(8, 0)), "loadsilent");
    LoadedAudio a = loadAudio(path);
    std::filesystem::remove(path);
    EXPECT_FLOAT_EQ(a.globalPeak, 1.0f);
}

TEST(LoadAudio, ThrowsAPresentableErrorForUnreadableFiles) {
    // These messages end up in the in-window error banner.
    try {
        loadAudio("/nonexistent/dir/missing.wav");
        FAIL() << "expected an exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("cannot open"), std::string::npos) << e.what();
    }
    // Less than one whole frame of data: no samples at all.
    auto path = writeTempFile(buildWav(1, 2, 44100, 16, {0x01, 0x02}), "loadempty");
    EXPECT_THROW(loadAudio(path), std::runtime_error);
    std::filesystem::remove(path);
}
