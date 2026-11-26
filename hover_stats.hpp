#pragma once

// Statistics behind the hover readout in wav_main.cpp: exact peak, RMS and
// power means for the samples under the cursor, plus an amplitude histogram
// of those same samples. Pure logic (no SDL), unit-tested in
// tests/hover_stats_test.cpp.
//
// Peak and RMS come from the WaveformPyramid (exact, O(log range)). The
// histogram needs the raw values, so it's an O(range) scan, capped by
// striding through the window when it holds more than `maxBinned` samples.

#include "waveform_pyramid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

struct HoverStats {
    bool     valid = false;
    long     i0 = 0, i1 = 0;      // inclusive raw sample range measured
    uint32_t count = 0;           // i1 - i0 + 1
    float    mn = 0.0f, mx = 0.0f;
    float    peak = 0.0f;         // max |v|
    float    rms = 0.0f;
    // The rest of drawWaveform's per-column anchors, over the same window, so
    // the histogram can be colored by the same layers the main view uses
    // (see coreBoundary in waveform_render.hpp).
    float    ms = 0.0f;           // mean square (unrooted, like the hot spine)
    float    cubeM = 0.0f;        // cbrt(mean |v|^3)
    float    quadM = 0.0f;        // (mean v^4)^(1/4)
};

// Histogram of sample values over [lo, hi]: bin k covers
// [lo + k*width, lo + (k+1)*width). Samples outside the domain aren't counted.
struct ValueHistogram {
    float lo = 0.0f, hi = 0.0f;
    std::vector<uint32_t> bins;
    uint32_t binnedSamples = 0;   // samples visited (in or out of the domain)
    long     stride = 1;          // > 1 when the window was decimated

    float width() const { return bins.empty() ? 0.0f : (hi - lo) / (float)bins.size(); }
    float center(int k) const { return lo + ((float)k + 0.5f) * width(); }
};

// Sample range [i0, i1] covered by the time span [tL, tR] (seconds), widened
// symmetrically to at least `minSamples` -- zoomed far in, the cursor's few
// pixels cover only a sample or two, and the stats and histogram of a
// single sample say nothing. Returns false if the span lies entirely
// outside [0, N).
inline bool hoverSampleRange(double tL, double tR, double sampleRate, long N,
                             long minSamples, long& i0, long& i1) {
    if (N <= 0 || !(sampleRate > 0.0)) return false;
    if (tR < tL) std::swap(tL, tR);
    double fL = std::floor(tL * sampleRate);
    double fR = std::ceil (tR * sampleRate);
    if (!std::isfinite(fL) || !std::isfinite(fR)) return false;
    if (fR < 0.0 || fL > (double)(N - 1)) return false;

    // Clamp before the cast so far-off-screen spans can't overflow `long`.
    long a = (long)std::max(fL, -(double)N);
    long b = (long)std::min(fR, 2.0 * (double)N);
    if (b - a + 1 < minSamples) {
        long center = a + (b - a) / 2;
        a = center - minSamples / 2;
        b = a + minSamples - 1;
    }
    i0 = std::clamp(a, 0L, N - 1);
    i1 = std::clamp(b, 0L, N - 1);
    return i0 <= i1;
}

inline HoverStats computeHoverStats(const std::vector<float>& ys, const WaveformPyramid& pyr,
                                    long i0, long i1) {
    HoverStats s;
    const long N = (long)ys.size();
    if (N == 0 || pyr.n != N || i0 < 0 || i1 >= N || i1 < i0) return s;

    BinStat b = queryPyramid(pyr, ys, i0, i1);
    s.valid = true;
    s.i0 = i0; s.i1 = i1;
    s.count = b.count;
    s.mn = b.mn; s.mx = b.mx;
    s.peak = std::max(std::fabs(b.mn), std::fabs(b.mx));
    // The pyramid's float sums can nudge a rooted mean a hair above peak on a
    // constant-magnitude signal (where they should be equal). Mathematically
    // they never exceed it, so clamp them to peak.
    const double n = (double)b.count;
    s.ms    = (float)((double)b.sumSq / n);
    s.rms   = std::min((float)std::sqrt((double)b.sumSq / n), s.peak);
    s.cubeM = std::min((float)std::cbrt((double)b.sumCube / n), s.peak);
    s.quadM = std::min((float)std::pow ((double)b.sumQuad / n, 0.25), s.peak);
    return s;
}

inline ValueHistogram computeHistogram(const std::vector<float>& ys, long i0, long i1,
                                       float lo, float hi, int nBins, uint32_t maxBinned) {
    ValueHistogram h;
    const long N = (long)ys.size();
    if (i0 < 0 || i1 >= N || i1 < i0 || nBins < 1 || !(hi > lo)) return h;

    h.lo = lo; h.hi = hi;
    const long count = i1 - i0 + 1;
    h.stride = (maxBinned > 0) ? std::max(1L, (count + (long)maxBinned - 1) / (long)maxBinned) : 1L;
    h.bins.assign((size_t)nBins, 0);
    const double scale = (double)nBins / ((double)hi - (double)lo);
    for (long i = i0; i <= i1; i += h.stride) {
        ++h.binnedSamples;
        const float v = ys[i];
        if (!(v >= lo && v <= hi)) continue; // also rejects NaN (floor -> int cast would be UB)
        int k = (int)std::floor(((double)v - (double)lo) * scale);
        ++h.bins[(size_t)std::min(k, nBins - 1)]; // v == hi lands in the top bin
    }
    return h;
}

// Screen-row height of one histogram bin, in pixels.
constexpr float kHistBinPx = 2.0f;

// Bin domain for the histogram panel: the window's [-peak, +peak], clipped
// to the visible rows and snapped to the 2px row grid so bars land on whole
// pixels. nBins == 0 when that range is entirely off screen.
struct HistLayout { float lo = 0.0f, hi = 0.0f; int nBins = 0; };

inline HistLayout histLayout(float peak, float panY, float scaleY, int winH) {
    const float yTop = panY - peak * scaleY, yBot = panY + peak * scaleY;
    if (!(scaleY > 0.0f) || yBot < 0.0f || yTop > (float)winH) return {};
    const float rTop = std::floor(std::max(yTop, 0.0f));
    const float rBot = std::min(yBot, (float)winH);
    const int nBins = std::max(1, (int)std::ceil((rBot - rTop) / kHistBinPx));
    return { (panY - (rTop + (float)nBins * kHistBinPx)) / scaleY, (panY - rTop) / scaleY, nBins };
}

// Same bins to within a quarter pixel (the eased pan keeps creeping by
// sub-pixel amounts after it's "settled").
inline bool sameLayout(const HistLayout& a, const HistLayout& b, float scaleY) {
    return a.nBins == b.nBins
        && std::fabs(a.hi - b.hi) * scaleY < 0.25f
        && std::fabs(a.lo - b.lo) * scaleY < 0.25f;
}
