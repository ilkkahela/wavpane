#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wav {

struct WavData {
    uint32_t sampleRate = 0;
    uint16_t numChannels = 0;
    uint16_t bitsPerSample = 0;
    // channels[c][i] = sample i of channel c, normalized to [-1..1]
    std::vector<std::vector<float>> channels;

    size_t frameCount() const { return channels.empty() ? 0 : channels[0].size(); }
    double durationSeconds() const {
        return sampleRate ? (double)frameCount() / (double)sampleRate : 0.0;
    }
};

class WavReader {
public:
    WavData read(const std::string& path) const {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) throw std::runtime_error("cannot open file: " + path);

        // Chunk sizes are capped to what's actually in the file: streamed
        // writers (e.g. ffmpeg to a pipe) leave data size = 0xFFFFFFFF, and a
        // corrupt header shouldn't be able to make us allocate gigabytes.
        f.seekg(0, std::ios::end);
        const std::streamoff fileSize = f.tellg();
        f.seekg(0, std::ios::beg);
        auto remaining = [&]() -> uint64_t {
            const std::streamoff pos = f.tellg();
            return (pos < 0 || pos > fileSize) ? 0 : (uint64_t)(fileSize - pos);
        };

        char magic[4];
        f.read(magic, 4);
        if (f.gcount() != 4 || std::string(magic, 4) != "RIFF")
            throw std::runtime_error("not a RIFF file: " + path);
        f.ignore(4); // riff size
        f.read(magic, 4);
        if (f.gcount() != 4 || std::string(magic, 4) != "WAVE")
            throw std::runtime_error("not a WAVE file: " + path);

        WavData out;
        uint16_t audioFormat = 0;
        bool haveFmt = false;
        std::vector<uint8_t> raw;

        while (f.good()) {
            char id[4];
            f.read(id, 4);
            if (f.gcount() != 4) break;
            uint32_t sz = readU32(f);
            std::string cid(id, 4);

            if (cid == "fmt ") {
                if (sz < 16) throw std::runtime_error("fmt chunk too small");
                if (sz > remaining()) throw std::runtime_error("fmt chunk truncated");
                std::vector<uint8_t> chunk(sz);
                f.read(reinterpret_cast<char*>(chunk.data()), sz);
                audioFormat        = le16(&chunk[0]);
                out.numChannels    = le16(&chunk[2]);
                out.sampleRate     = le32(&chunk[4]);
                out.bitsPerSample  = le16(&chunk[14]);
                // WAVE_FORMAT_EXTENSIBLE (common for 24-bit and multichannel
                // files): the real format is the SubFormat GUID's first 2 bytes.
                if (audioFormat == kFormatExtensible && sz >= 26) audioFormat = le16(&chunk[24]);
                haveFmt = true;
            } else if (cid == "data") {
                // A truncated file keeps only the bytes really present instead
                // of padding the tail with zeros.
                raw.resize((size_t)std::min<uint64_t>(sz, remaining()));
                f.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)raw.size());
                raw.resize((size_t)f.gcount());
            } else {
                f.ignore(sz);
            }
            if (sz % 2 != 0) f.ignore(1); // chunks are word-aligned
        }

        if (!haveFmt) throw std::runtime_error("missing fmt chunk");
        if (out.numChannels == 0) throw std::runtime_error("zero channels");
        if (raw.empty()) throw std::runtime_error("missing data chunk");

        if (out.sampleRate == 0) throw std::runtime_error("zero sample rate");
        // Reject what decodeSample can't decode rather than silently turning
        // it into zeros (or into garbage, for float read as int).
        const uint16_t bits = out.bitsPerSample;
        const bool pcmOk   = audioFormat == kFormatPcm   && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        const bool floatOk = audioFormat == kFormatFloat && (bits == 32 || bits == 64);
        if (!pcmOk && !floatOk)
            throw std::runtime_error("unsupported format " + std::to_string(audioFormat) +
                                     " / " + std::to_string(bits) + "-bit");
        const int bytesPerSample = bits / 8;
        const size_t frameBytes = (size_t)bytesPerSample * out.numChannels;
        const size_t frames = raw.size() / frameBytes;

        out.channels.assign(out.numChannels, std::vector<float>(frames));
        for (size_t i = 0; i < frames; ++i) {
            const uint8_t* base = &raw[i * frameBytes];
            for (uint16_t c = 0; c < out.numChannels; ++c) {
                const uint8_t* p = base + (size_t)c * bytesPerSample;
                out.channels[c][i] = decodeSample(p, out.bitsPerSample, audioFormat);
            }
        }
        return out;
    }

private:
    static constexpr uint16_t kFormatPcm = 1, kFormatFloat = 3, kFormatExtensible = 0xFFFE;

    static uint32_t readU32(std::ifstream& f) {
        uint8_t b[4]{};
        f.read(reinterpret_cast<char*>(b), 4);
        return le32(b);
    }
    static uint16_t le16(const uint8_t* p) {
        return (uint16_t)(p[0] | (p[1] << 8));
    }
    static uint32_t le32(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    // audioFormat: 1 = PCM integer, 3 = IEEE float. Non-finite floats become
    // 0: NaN would otherwise poison every min/max/sum it's merged into.
    static float decodeSample(const uint8_t* p, uint16_t bits, uint16_t audioFormat) {
        if (audioFormat == kFormatFloat) {
            float v;
            if (bits == 64) {
                uint64_t u = (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
                double d; std::memcpy(&d, &u, 8);
                v = (float)d;
            } else {
                uint32_t u = le32(p);
                std::memcpy(&v, &u, 4);
            }
            return std::isfinite(v) ? v : 0.0f;
        }
        switch (bits) {
            case 8: {
                // unsigned 8-bit, centered at 128
                return ((float)p[0] - 128.0f) / 128.0f;
            }
            case 16: {
                int16_t v = (int16_t)le16(p);
                return (float)v / 32768.0f;
            }
            case 24: {
                int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
                if (v & 0x00800000) v |= (int32_t)0xFF000000; // sign-extend
                return (float)v / 8388608.0f;
            }
            case 32: {
                int32_t v = (int32_t)le32(p);
                return (float)v / 2147483648.0f;
            }
            default:
                return 0.0f;
        }
    }
};

} // namespace wav
