#include <gtest/gtest.h>

#include "../hover_stats.hpp"

#include <cmath>
#include <numeric>
#include <vector>

namespace {

constexpr int kBins = 49;

HoverStats statsOver(const std::vector<float>& ys) {
    WaveformPyramid pyr = buildPyramid(ys);
    return computeHoverStats(ys, pyr, 0, (long)ys.size() - 1);
}

// Histogram over the window's own [-peak, +peak].
ValueHistogram histOver(const std::vector<float>& ys, uint32_t maxBinned = 1u << 20) {
    HoverStats s = statsOver(ys);
    float r = s.peak > 0.0f ? s.peak : 1.0f;
    return computeHistogram(ys, 0, (long)ys.size() - 1, -r, r, kBins, maxBinned);
}

uint32_t binTotal(const ValueHistogram& h) {
    return std::accumulate(h.bins.begin(), h.bins.end(), 0u);
}

} // namespace

// ─── hoverSampleRange ────────────────────────────────────────────────────────

TEST(HoverSampleRange, SwapsReversedSpansAndSurvivesExtremeTimes) {
    long a0, a1, b0, b1;
    ASSERT_TRUE(hoverSampleRange(0.001, 0.002, 1000.0, 100, 1, a0, a1));
    ASSERT_TRUE(hoverSampleRange(0.002, 0.001, 1000.0, 100, 1, b0, b1));
    EXPECT_EQ(a0, b0);
    EXPECT_EQ(a1, b1);
    // Zoomed absurdly far out: the span dwarfs `long` but must clamp to the
    // buffer instead of overflowing the cast.
    long i0, i1;
    ASSERT_TRUE(hoverSampleRange(-1e30, 1e30, 44100.0, 100, 1, i0, i1));
    EXPECT_EQ(i0, 0);
    EXPECT_EQ(i1, 99);
    // NaN (e.g. a degenerate scale) is rejected rather than cast.
    EXPECT_FALSE(hoverSampleRange(std::nan(""), 0.5, 44100.0, 100, 1, i0, i1));
}

TEST(HoverSampleRange, CoversTheRequestedSpan) {
    long i0 = -1, i1 = -1;
    ASSERT_TRUE(hoverSampleRange(1.0, 2.0, 100.0, 1000, 1, i0, i1));
    EXPECT_EQ(i0, 100);
    EXPECT_EQ(i1, 200);
}

TEST(HoverSampleRange, WidensTinySpansToTheMinimum) {
    long i0 = -1, i1 = -1;
    ASSERT_TRUE(hoverSampleRange(5.001, 5.002, 100.0, 1000, 64, i0, i1));
    EXPECT_EQ(i1 - i0 + 1, 64);
    EXPECT_LE(i0, 500);
    EXPECT_GE(i1, 501);
}

TEST(HoverSampleRange, ClampsToTheBufferEdges) {
    long i0 = -1, i1 = -1;
    ASSERT_TRUE(hoverSampleRange(-1.0, 0.5, 100.0, 1000, 1, i0, i1));
    EXPECT_EQ(i0, 0);
    EXPECT_EQ(i1, 50);
    ASSERT_TRUE(hoverSampleRange(9.5, 20.0, 100.0, 1000, 1, i0, i1));
    EXPECT_EQ(i0, 950);
    EXPECT_EQ(i1, 999);
}

TEST(HoverSampleRange, RejectsSpansEntirelyOutsideTheData) {
    long i0, i1;
    EXPECT_FALSE(hoverSampleRange(-2.0, -1.0, 100.0, 1000, 64, i0, i1));
    EXPECT_FALSE(hoverSampleRange(10.5, 11.0, 100.0, 1000, 64, i0, i1));
    EXPECT_FALSE(hoverSampleRange(0.0, 1.0, 100.0, 0, 64, i0, i1));
}

// ─── computeHoverStats ───────────────────────────────────────────────────────

TEST(HoverStats, SineHasCrestOfRootTwo) {
    // 100 whole periods of a 0.5-amplitude sine, 64 samples per period.
    std::vector<float> ys(6400);
    for (size_t i = 0; i < ys.size(); ++i)
        ys[i] = 0.5f * (float)std::sin(2.0 * M_PI * (double)i / 64.0);
    HoverStats s = statsOver(ys);
    ASSERT_TRUE(s.valid);
    EXPECT_NEAR(s.peak, 0.5f, 1e-4f);
    EXPECT_NEAR(s.rms, 0.5f / std::sqrt(2.0f), 1e-4f);
    EXPECT_NEAR(s.peak / s.rms, std::sqrt(2.0f), 1e-3f);
}

TEST(HoverStats, SquareWaveHasUnitCrest) {
    std::vector<float> ys(1000);
    for (size_t i = 0; i < ys.size(); ++i) ys[i] = (i / 10) % 2 ? 0.25f : -0.25f;
    HoverStats s = statsOver(ys);
    EXPECT_FLOAT_EQ(s.peak, 0.25f);
    EXPECT_NEAR(s.peak / s.rms, 1.0f, 1e-5f);
    EXPECT_LE(s.rms, s.peak);
    // All mass in the two outermost bins.
    ValueHistogram h = histOver(ys);
    EXPECT_EQ(h.bins.front() + h.bins.back(), 1000u);
}

TEST(HoverStats, SingleImpulseHasCrestOfRootN) {
    std::vector<float> ys(400, 0.0f);
    ys[123] = -0.8f;
    HoverStats s = statsOver(ys);
    EXPECT_NEAR(s.peak / s.rms, 20.0f, 1e-3f); // sqrt(400)
    ValueHistogram h = histOver(ys);
    EXPECT_EQ(h.bins[kBins / 2], 399u);       // zeros in the center bin
    EXPECT_EQ(h.bins.front(), 1u);            // the impulse at -peak
}

TEST(HoverStats, SilenceHasZeroStatsAndCenteredHistogram) {
    std::vector<float> ys(256, 0.0f);
    HoverStats s = statsOver(ys);
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.peak, 0.0f);
    EXPECT_EQ(s.rms, 0.0f);
    EXPECT_EQ(histOver(ys).bins[kBins / 2], 256u);
}

TEST(Histogram, StridingCapsTheScan) {
    std::vector<float> ys(10000);
    for (size_t i = 0; i < ys.size(); ++i) ys[i] = (float)((int)(i % 7) - 3) / 3.0f;
    ValueHistogram h = histOver(ys, 1000);
    EXPECT_EQ(h.stride, 10);
    EXPECT_EQ(h.binnedSamples, 1000u);
    EXPECT_EQ(binTotal(h), h.binnedSamples);
    // Peak/RMS still cover every sample, not just the strided ones.
    EXPECT_EQ(statsOver(ys).count, 10000u);
}

TEST(Histogram, SkipsSamplesOutsideTheDomain) {
    // Domain [0, 1] in 4 bins: negatives are dropped, not piled into bin 0.
    std::vector<float> ys = {-0.9f, -0.1f, 0.1f, 0.3f, 0.6f, 0.9f, 1.0f, 1.5f};
    ValueHistogram h = computeHistogram(ys, 0, (long)ys.size() - 1, 0.0f, 1.0f, 4, 1000);
    ASSERT_EQ(h.bins.size(), 4u);
    EXPECT_EQ(h.bins[0], 1u);
    EXPECT_EQ(h.bins[1], 1u);
    EXPECT_EQ(h.bins[2], 1u);
    EXPECT_EQ(h.bins[3], 2u);                 // 0.9 and exactly hi
    EXPECT_EQ(h.binnedSamples, 8u);
    EXPECT_FLOAT_EQ(h.center(0), 0.125f);
}

TEST(Histogram, InteriorBinEdgesBelongToTheUpperBin) {
    // Bins are half-open [lo + k*w, lo + (k+1)*w): an exact edge value must
    // go up a bin, never be counted twice or dropped.
    std::vector<float> ys = {0.0f, 0.25f, 0.5f, 0.75f};
    ValueHistogram h = computeHistogram(ys, 0, 3, 0.0f, 1.0f, 4, 1000);
    ASSERT_EQ(h.bins.size(), 4u);
    for (int k = 0; k < 4; ++k) EXPECT_EQ(h.bins[k], 1u) << "bin " << k;
}

// Regression: NaN slipped past the `v < lo || v > hi` domain check and its
// floor() was cast to an int bin index -- UB, in practice an out-of-bounds write.
TEST(Histogram, IgnoresNaNSamples) {
    std::vector<float> ys = {std::nanf(""), 0.1f, std::nanf(""), -0.1f};
    ValueHistogram h = computeHistogram(ys, 0, 3, -1.0f, 1.0f, 4, 1000);
    ASSERT_EQ(h.bins.size(), 4u);
    EXPECT_EQ(binTotal(h), 2u);
    EXPECT_EQ(h.binnedSamples, 4u);
}

TEST(Histogram, RejectsEmptyDomains) {
    std::vector<float> ys(10, 0.0f);
    EXPECT_TRUE(computeHistogram(ys, 0, 9, 0.5f, 0.5f, 4, 100).bins.empty());
    EXPECT_TRUE(computeHistogram(ys, 0, 9, -1.0f, 1.0f, 0, 100).bins.empty());
    EXPECT_TRUE(computeHistogram(ys, 5, 10, -1.0f, 1.0f, 4, 100).bins.empty());
}

TEST(HoverStats, SubrangeMatchesBruteForce) {
    std::vector<float> ys(5000);
    for (size_t i = 0; i < ys.size(); ++i) ys[i] = (float)std::sin((double)i * 0.37) * (float)(i % 13) / 13.0f;
    WaveformPyramid pyr = buildPyramid(ys);
    HoverStats s = computeHoverStats(ys, pyr, 1234, 3210);
    double sumSq = 0.0; float peak = 0.0f;
    for (long i = 1234; i <= 3210; ++i) { sumSq += (double)ys[i] * ys[i]; peak = std::max(peak, std::fabs(ys[i])); }
    EXPECT_FLOAT_EQ(s.peak, peak);
    EXPECT_NEAR(s.rms, std::sqrt(sumSq / 1977.0), 1e-5);
    ValueHistogram h = computeHistogram(ys, 1234, 3210, -s.peak, s.peak, kBins, 1u << 20);
    EXPECT_EQ(binTotal(h), 1977u);
}

TEST(HoverStats, RejectsBadRanges) {
    std::vector<float> ys(100, 0.1f);
    WaveformPyramid pyr = buildPyramid(ys);
    EXPECT_FALSE(computeHoverStats(ys, pyr, -1, 10).valid);
    EXPECT_FALSE(computeHoverStats(ys, pyr, 50, 100).valid);
    EXPECT_FALSE(computeHoverStats(ys, pyr, 60, 50).valid);
}

TEST(HoverStats, CarriesTheEnvelopeAnchors) {
    // Constant magnitude: every rooted moment equals |v|, and the unrooted
    // mean square is v^2.
    std::vector<float> ys(1000);
    for (size_t i = 0; i < ys.size(); ++i) ys[i] = (i % 2) ? 0.5f : -0.5f;
    HoverStats s = statsOver(ys);
    ASSERT_TRUE(s.valid);
    EXPECT_NEAR(s.ms, 0.25f, 1e-5f);
    EXPECT_NEAR(s.rms, 0.5f, 1e-5f);
    EXPECT_NEAR(s.cubeM, 0.5f, 1e-5f);
    EXPECT_NEAR(s.quadM, 0.5f, 1e-5f);
    EXPECT_LE(s.cubeM, s.peak);
    EXPECT_LE(s.quadM, s.peak);
}

// ─── histLayout / sameLayout (histogram panel <-> screen rows) ──────────────

TEST(HistLayout, SnapsToTheRowGridAndCoversThePeak) {
    // panY off the pixel grid on purpose: the top bin edge must still land on
    // a whole pixel, every bin is exactly kHistBinPx tall, and the binned
    // domain must contain all of [-peak, +peak].
    const float peak = 0.5f, panY = 400.3f, scaleY = 300.0f;
    HistLayout L = histLayout(peak, panY, scaleY, 800);
    ASSERT_GT(L.nBins, 0);
    const float yTop = panY - L.hi * scaleY;
    const float yBot = panY - L.lo * scaleY;
    EXPECT_NEAR(yTop, std::round(yTop), 1e-3f);
    EXPECT_NEAR(yBot - yTop, (float)L.nBins * kHistBinPx, 1e-3f);
    EXPECT_GE(L.hi, peak);
    EXPECT_LE(L.lo, -peak);
}

TEST(HistLayout, ClipsToTheWindowAndRejectsOffscreenRanges) {
    // +/-1 at 1000 px per unit spans 2000 px; only the 800 visible rows get bins.
    HistLayout L = histLayout(1.0f, 400.0f, 1000.0f, 800);
    EXPECT_EQ(L.nBins, 400);
    EXPECT_NEAR(L.hi,  0.4f, 1e-6f);
    EXPECT_NEAR(L.lo, -0.4f, 1e-6f);
    // Entirely above / below the window, or a degenerate scale: no bins.
    EXPECT_EQ(histLayout(0.1f, -500.0f, 1000.0f, 800).nBins, 0);
    EXPECT_EQ(histLayout(0.1f, 1500.0f, 1000.0f, 800).nBins, 0);
    EXPECT_EQ(histLayout(0.5f, 400.0f, 0.0f, 800).nBins, 0);
}

TEST(HistLayout, SameLayoutToleratesOnlySubQuarterPixelDrift) {
    const float scaleY = 300.0f;
    HistLayout a = histLayout(0.5f, 400.0f, scaleY, 800);
    auto shifted = [&](float px) {
        HistLayout b = a;
        b.hi += px / scaleY; b.lo += px / scaleY;
        return b;
    };
    EXPECT_TRUE (sameLayout(a, shifted(0.2f), scaleY));   // easing creep: keep the cached bins
    EXPECT_FALSE(sameLayout(a, shifted(0.3f), scaleY));   // visible shift: rebin
    HistLayout c = a; c.nBins += 1;
    EXPECT_FALSE(sameLayout(a, c, scaleY));
}
