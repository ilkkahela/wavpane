#pragma once

// Everything wav_main.cpp needs from an audio file, ready to draw: the mono
// downmix, its pyramid, and the per-file constants. Kept out of main so a
// file can be (re)loaded at any time -- command line or drag-and-drop -- and
// so the loading path is unit-tested (tests/wav_reader_test.cpp).

#include "wav_reader.hpp"
#include "waveform_pyramid.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct LoadedAudio {
    std::vector<float> mono;
    WaveformPyramid    pyramid;
    double sampleRate = 0.0;
    double duration   = 0.0;
    float  globalPeak = 1.0f;   // reference for the loudness fade; 1.0 for silence
    std::string name;           // file name without directories, for the title bar
};

// Throws std::runtime_error (with a user-presentable message) on failure.
inline LoadedAudio loadAudio(const std::string& path) {
    wav::WavReader reader;
    wav::WavData wd = reader.read(path);
    if (wd.frameCount() == 0) throw std::runtime_error("no samples in " + path);

    LoadedAudio a;
    a.sampleRate = wd.sampleRate;
    a.duration   = wd.durationSeconds();
    a.name       = std::filesystem::path(path).filename().string();

    // Downmix to mono: one trace regardless of channel count. The per-channel
    // buffers are released as soon as this returns -- nothing reads them
    // again, and for a long stereo file they'd otherwise pin twice the mono
    // buffer's memory for the whole session.
    if (wd.numChannels == 1) {
        a.mono = std::move(wd.channels[0]);
    } else {
        a.mono.resize(wd.frameCount());
        for (size_t i = 0; i < a.mono.size(); ++i) {
            float sum = 0.0f;
            for (uint16_t c = 0; c < wd.numChannels; ++c) sum += wd.channels[c][i];
            a.mono[i] = sum / (float)wd.numChannels;
        }
    }

    float peak = 0.0f;
    for (float v : a.mono) peak = std::max(peak, std::fabs(v));
    a.globalPeak = (peak < 1e-6f) ? 1.0f : peak;

    a.pyramid = buildPyramid(a.mono);
    return a;
}
