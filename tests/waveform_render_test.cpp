#include <gtest/gtest.h>

#include "../waveform_render.hpp"
#include "../waveform_pyramid.hpp"

#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

namespace {

// A fixed seed makes both the correctness checks and the benchmark below
// operate on byte-identical input every run, on every machine — required for
// the benchmark's timings to be comparable across runs and for the
// determinism check to mean anything.
constexpr uint32_t kSeed = 1234567u;

// Synthetic "seed input": a mix of a couple of sine tones plus seeded noise,
// clamped to [-1, 1] like real decoded PCM. Not just noise, so both the
// envelope's min/max path and its RMS-family bands see realistic structure
// rather than uniformly-random per-sample extremes.
std::vector<float> makeSeededSamples(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-0.05f, 0.05f);
    std::vector<float> ys(n);
    for (size_t i = 0; i < n; ++i) {
        double t = (double)i / 44100.0;
        float tone = 0.6f * std::sin(2.0 * M_PI * 220.0 * t) +
                     0.3f * std::sin(2.0 * M_PI * 5000.0 * t);
        ys[i] = std::clamp(tone + noise(rng), -1.0f, 1.0f);
    }
    return ys;
}

// Headless SDL surface + software renderer: draws entirely into memory, no
// window or display connection required, which is what makes this runnable
// in CI. SDL is reference-counted by subsystem, so Init/Quit per test is
// cheap and keeps tests independent.
struct HeadlessRenderer {
    SDL_Surface* surface = nullptr;
    SDL_Renderer* renderer = nullptr;

    HeadlessRenderer(int w, int h) {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_Init(SDL_INIT_VIDEO);
        surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    }
    ~HeadlessRenderer() {
        if (renderer) SDL_DestroyRenderer(renderer);
        if (surface) SDL_DestroySurface(surface);
    }
};

EnvelopeShading makeShading(float globalPeak) { return EnvelopeShading{globalPeak, 5}; }

// Standalone reference (deliberately NOT sharing code with
// waveform_pyramid.hpp) for cross-checking queryPyramid: the same linear
// left-to-right accumulation drawWaveform itself still uses (see
// waveform_render.hpp) -- the pyramid isn't wired into rendering (see
// waveform_pyramid.hpp's header comment for why), just tested standalone here.
BinStat bruteForceRange(const std::vector<float>& ys, long i0, long i1) {
    BinStat b;
    b.mn = ys[i0];
    b.mx = ys[i0];
    for (long i = i0; i <= i1; ++i) {
        float v = ys[i];
        if (v < b.mn) b.mn = v;
        if (v > b.mx) b.mx = v;
        float av = std::fabs(v);
        float sq = av * av;
        b.sumSq += sq;
        b.sumCube += sq * av;
        b.sumQuad += sq * sq;
    }
    b.count = (uint32_t)(i1 - i0 + 1);
    return b;
}

// queryPyramid sums the same terms as bruteForceRange but in a different
// (tree-shaped) grouping, so float rounding differs by a few ULPs per merge
// — expected, not a bug (see waveform_pyramid.hpp's numerical-precision
// comment). Tolerance scales with count since sumSq/sumCube/sumQuad grow
// with it (each term is in [0,1]).
void expectBinStatNear(const BinStat& got, const BinStat& ref) {
    EXPECT_EQ(got.count, ref.count);
    EXPECT_FLOAT_EQ(got.mn, ref.mn);
    EXPECT_FLOAT_EQ(got.mx, ref.mx);
    float tol = std::max(1e-3f, (float)ref.count * 5e-6f);
    EXPECT_NEAR(got.sumSq, ref.sumSq, tol);
    EXPECT_NEAR(got.sumCube, ref.sumCube, tol);
    EXPECT_NEAR(got.sumQuad, ref.sumQuad, tol);
}

} // namespace

// ─── Color generation pipeline (pure, no SDL/rendering involved) ───────────

TEST(CurveForLevel, GammaRisesMonotonicallyAndLevel9IsHardWhite) {
    float prevGamma = -1.0f;
    for (int level = 1; level <= 8; ++level) {
        FadeCurve c = curveForLevel(level);
        EXPECT_FALSE(c.hardWhite);
        EXPECT_GT(c.gamma, prevGamma);
        prevGamma = c.gamma;
    }
    // Level 1 crushes loudness toward the dark rungs (gamma < 1); level 8
    // lifts it toward the bright ones (gamma > 1) -- "darkest" to "brightest".
    EXPECT_LT(curveForLevel(1).gamma, 1.0f);
    EXPECT_GT(curveForLevel(8).gamma, 1.0f);

    EXPECT_TRUE(curveForLevel(9).hardWhite);
}

TEST(LevelT, RunsFromZeroToOneMonotonicallyAndClampsOutsideOneToEight) {
    EXPECT_FLOAT_EQ(levelT(1), 0.0f);
    EXPECT_FLOAT_EQ(levelT(8), 1.0f);
    for (int level = 2; level <= 8; ++level) EXPECT_GT(levelT(level), levelT(level - 1));
    EXPECT_FLOAT_EQ(levelT(0), 0.0f);
    EXPECT_FLOAT_EQ(levelT(9), 1.0f);
}

TEST(CoreBoundary, InterpolatesBetweenAnchorsAndClampsAtTheEnds) {
    const float anchors[5] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    EXPECT_FLOAT_EQ(coreBoundary(anchors, 0.0f), 0.1f);   // mean square
    EXPECT_FLOAT_EQ(coreBoundary(anchors, 4.0f), 0.5f);   // peak (not past the array end)
    EXPECT_NEAR(coreBoundary(anchors, 2.5f), 0.35f, 1e-6f);
    EXPECT_FLOAT_EQ(coreBoundary(anchors, -1.0f), 0.1f);
    EXPECT_FLOAT_EQ(coreBoundary(anchors, 7.0f), 0.5f);
}

TEST(StepIntensity, SilenceLandsOnTheDarkestRungAtAnyLevel) {
    // lin=0 -> shaped=0 regardless of gamma, so silence always bottoms out
    // at the same fixed grayscale rung no matter which level is active.
    constexpr float kDarkFloor = 0.14f; // must match stepIntensity's internal constant
    SteppedIntensity low  = stepIntensity(0.0f, 1.0f, curveForLevel(1));
    SteppedIntensity high = stepIntensity(0.0f, 1.0f, curveForLevel(8));
    EXPECT_NEAR(low.fade, kDarkFloor, 1e-5f);
    EXPECT_NEAR(high.fade, kDarkFloor, 1e-5f);
}

TEST(StepIntensity, LoudestColumnReachesFullFade) {
    FadeCurve curve = curveForLevel(5);
    SteppedIntensity s = stepIntensity(/*colPeak=*/1.0f, /*globalPeak=*/1.0f, curve);
    EXPECT_FALSE(s.hardWhite);
    EXPECT_NEAR(s.fade, 1.0f, 1e-5f);
}

TEST(StepIntensity, HigherLevelGammaLiftsQuietColumnsAtEqualLoudness) {
    SteppedIntensity low  = stepIntensity(0.1f, 1.0f, curveForLevel(1));
    SteppedIntensity high = stepIntensity(0.1f, 1.0f, curveForLevel(8));
    EXPECT_LT(low.fade, high.fade);
}

// The core property this pipeline is built around: intensity (level) only
// ever changes *which* rung a given colPeak bins into (via gamma) -- it
// never introduces a fade value outside the one fixed 13-rung grayscale.
TEST(StepIntensity, FadeAlwaysLandsOnTheSameFixedGrayscaleRegardlessOfLevel) {
    constexpr float kDarkFloor = 0.14f;
    constexpr int kFadeSteps = 12;
    std::vector<float> grayscale;
    for (int i = 0; i <= kFadeSteps; ++i)
        grayscale.push_back(kDarkFloor + (1.0f - kDarkFloor) * ((float)i / kFadeSteps));

    auto isOnGrayscale = [&](float fade) {
        for (float g : grayscale) if (std::fabs(g - fade) < 1e-4f) return true;
        return false;
    };

    std::mt19937 rng(kSeed);
    std::uniform_real_distribution<float> peakDist(0.0f, 1.0f);
    for (int level = 1; level <= 8; ++level) {
        FadeCurve curve = curveForLevel(level);
        for (int trial = 0; trial < 50; ++trial) {
            float colPeak = peakDist(rng);
            SteppedIntensity s = stepIntensity(colPeak, 1.0f, curve);
            EXPECT_TRUE(isOnGrayscale(s.fade))
                << "level=" << level << " colPeak=" << colPeak << " fade=" << s.fade;
        }
    }
}

TEST(StepIntensity, HardWhiteIgnoresLoudness) {
    FadeCurve curve = curveForLevel(9);
    SteppedIntensity silent = stepIntensity(0.0f, 1.0f, curve);
    SteppedIntensity loud   = stepIntensity(1.0f, 1.0f, curve);
    EXPECT_TRUE(silent.hardWhite);
    EXPECT_TRUE(loud.hardWhite);
    EXPECT_FLOAT_EQ(silent.fade, 1.0f);
    EXPECT_FLOAT_EQ(loud.fade, 1.0f);
}

TEST(ColorForStep, HardWhiteIgnoresBaseColor) {
    SDL_Color darkBase{150, 150, 168, 165};
    SDL_Color c = colorForStep(darkBase, SteppedIntensity{0.0f, true}, /*bgIsWhite=*/false);
    EXPECT_EQ(c.r, 255); EXPECT_EQ(c.g, 255); EXPECT_EQ(c.b, 255); EXPECT_EQ(c.a, 255);
}

TEST(ColorForStep, FullFadeReturnsBaseColorUnchanged) {
    SDL_Color base{200, 100, 50, 210};
    SDL_Color c = colorForStep(base, SteppedIntensity{1.0f, false}, /*bgIsWhite=*/false);
    EXPECT_EQ(c.r, base.r); EXPECT_EQ(c.g, base.g); EXPECT_EQ(c.b, base.b); EXPECT_EQ(c.a, base.a);
}

TEST(ColorForStep, PartialFadeScalesRgbOnlyOnDarkBackground) {
    SDL_Color base{200, 100, 50, 210};
    SDL_Color c = colorForStep(base, SteppedIntensity{0.5f, false}, /*bgIsWhite=*/false);
    EXPECT_EQ(c.r, 100); EXPECT_EQ(c.g, 50); EXPECT_EQ(c.b, 25);
    EXPECT_EQ(c.a, base.a); // alpha untouched on a dark backdrop
}

TEST(ColorForStep, WhiteBackgroundRampsAlphaTowardOpaqueAsFadeDrops) {
    SDL_Color base{150, 150, 168, 165};
    SDL_Color quiet = colorForStep(base, SteppedIntensity{0.0f, false}, /*bgIsWhite=*/true);
    SDL_Color loud  = colorForStep(base, SteppedIntensity{1.0f, false}, /*bgIsWhite=*/true);
    EXPECT_EQ(quiet.a, 255);   // fade->0: fully opaque so near-black reads as true black
    EXPECT_EQ(loud.a, base.a); // fade->1: alpha eases back to the base value
}

// ─── Pyramid correctness ────────────────────────────────────────────────────

// Property-based: for a spread of buffer sizes (including small, odd, and
// power-of-two-adjacent N, where the ceil-based level pairing is most likely
// to go wrong at the boundary) cross-check queryPyramid against the brute
// force reference over many seeded random ranges, plus the edge cases
// (single-sample range, full-range query).
TEST(WaveformPyramid, MatchesBruteForceOverRandomRanges) {
    std::mt19937 rng(kSeed);
    for (long n : {1L, 2L, 3L, 4L, 5L, 7L, 8L, 9L, 17L, 31L, 32L, 33L, 1000L, 100037L}) {
        auto ys = makeSeededSamples((size_t)n, kSeed + (uint32_t)n);
        WaveformPyramid pyr = buildPyramid(ys);
        ASSERT_EQ(pyr.n, n);

        // Full-range query.
        expectBinStatNear(queryPyramid(pyr, ys, 0, n - 1), bruteForceRange(ys, 0, n - 1));
        // Every single-sample range.
        for (long i = 0; i < n; ++i)
            expectBinStatNear(queryPyramid(pyr, ys, i, i), bruteForceRange(ys, i, i));

        std::uniform_int_distribution<long> pick(0, n - 1);
        for (int trial = 0; trial < 50; ++trial) {
            long a = pick(rng), b = pick(rng);
            if (a > b) std::swap(a, b);
            expectBinStatNear(queryPyramid(pyr, ys, a, b), bruteForceRange(ys, a, b));
        }
    }
}

TEST(WaveformPyramid, LevelsHalveAndEveryLevelTilesTheWholeBuffer) {
    // The no-gaps/no-overlaps tiling invariant queryPyramid relies on: at
    // every level the bin counts sum to N, sizes ceil-halve, and the top is
    // a single bin over everything.
    for (long n : {1L, 2L, 3L, 1000L, 1025L}) {
        std::vector<float> ys((size_t)n, 0.25f);
        WaveformPyramid pyr = buildPyramid(ys);
        ASSERT_FALSE(pyr.levels.empty());
        EXPECT_EQ(pyr.levels[0].size(), (size_t)(n + 1) / 2) << "n=" << n;
        for (size_t k = 0; k < pyr.levels.size(); ++k) {
            uint64_t total = 0;
            for (const BinStat& b : pyr.levels[k]) total += b.count;
            EXPECT_EQ(total, (uint64_t)n) << "n=" << n << " level=" << k;
            if (k > 0) EXPECT_EQ(pyr.levels[k].size(), (pyr.levels[k - 1].size() + 1) / 2);
        }
        EXPECT_EQ(pyr.levels.back().size(), 1u);
    }
}

TEST(MergeBins, EmptyBinIsTheIdentityAndItsZeroMinMaxNeverLeaks) {
    // An all-positive bin: if the empty bin's default mn = 0 leaked into a
    // merge, the min would wrongly drop to 0.
    const BinStat x{0.2f, 0.7f, 1.0f, 2.0f, 3.0f, 5};
    for (const BinStat& m : {mergeBins(BinStat{}, x), mergeBins(x, BinStat{})}) {
        EXPECT_FLOAT_EQ(m.mn, 0.2f);
        EXPECT_FLOAT_EQ(m.mx, 0.7f);
        EXPECT_FLOAT_EQ(m.sumSq, 1.0f);
        EXPECT_EQ(m.count, 5u);
    }
    EXPECT_EQ(mergeBins(BinStat{}, BinStat{}).count, 0u);
}

TEST(WaveformPyramid, EmptyBufferHasNoLevels) {
    WaveformPyramid pyr = buildPyramid({});
    EXPECT_EQ(pyr.n, 0);
    EXPECT_TRUE(pyr.levels.empty());
}

// ─── Correctness (drawWaveform) ─────────────────────────────────────────────

TEST(DrawWaveform, ProducesNonBlankOutputForNonSilentInput) {
    const int W = 400, H = 200;
    auto ys = makeSeededSamples(20000, kSeed);
    WaveformPyramid pyr = buildPyramid(ys);
    float peak = 0.0f;
    for (float v : ys) peak = std::max(peak, std::fabs(v));

    HeadlessRenderer hr(W, H);
    ASSERT_NE(hr.renderer, nullptr);
    SDL_SetRenderDrawColor(hr.renderer, 0, 0, 0, 255);
    SDL_RenderClear(hr.renderer);

    // Zoomed out: the whole 20000-sample buffer mapped across the window, so
    // this exercises the min/max-binning envelope path (samplesPerPx > kLineModeMaxSamplesPerPx).
    drawWaveform(hr.renderer, ys, pyr, /*sampleRate=*/44100.0, /*panX=*/0.0f,
                 /*scaleX=*/(float)W / (float)ys.size() * 44100.0f,
                 /*centerY=*/H / 2.0f, /*ampScale=*/H / 2.0f, W,
                 SDL_Color{255, 255, 255, 255}, /*ss=*/1, makeShading(peak),
                 /*bgIsWhite=*/false);
    SDL_RenderPresent(hr.renderer);

    // Some pixel must differ from the black background wherever the waveform
    // was drawn — a broad correctness signal without pinning exact pixels.
    auto* px = (const uint8_t*)hr.surface->pixels;
    int bpp = SDL_BYTESPERPIXEL(hr.surface->format);
    bool sawNonBlack = false;
    for (int y = 0; y < H && !sawNonBlack; ++y) {
        const uint8_t* row = px + (size_t)y * hr.surface->pitch;
        for (int x = 0; x < W; ++x) {
            const uint8_t* p = row + (size_t)x * bpp;
            if (p[0] || p[1] || p[2]) { sawNonBlack = true; break; }
        }
    }
    EXPECT_TRUE(sawNonBlack);
}

// Regression: horizontal view math was float. 100 px per sample, 300 s into
// a file puts panX at ~1.3e9 px, where float steps in 128 px increments.
TEST(ViewMath, KeepsSubPixelPrecisionDeepIntoLongFiles) {
    const double sampleRate = 44100.0, scaleX = sampleRate * 100.0; // 100 px/sample
    const double t = 300.0;
    const double panX = 640.0 - t * scaleX;                           // t at x = 640
    EXPECT_NEAR(toSX(t, panX, scaleX), 640.0, 1e-3);
    EXPECT_NEAR(toSX(t + 1.0 / sampleRate, panX, scaleX), 740.0, 1e-3);
    EXPECT_NEAR(toWX(740.0, panX, scaleX) * sampleRate, t * sampleRate + 1.0, 1e-3);
}

TEST(ViewMath, ClampedIndexNeverOverflowsTheCast) {
    EXPECT_EQ(clampedIndex(1e30, 1000), 1000);
    EXPECT_EQ(clampedIndex(-1e30, 1000), -1);
    EXPECT_EQ(clampedIndex(42.0, 1000), 42);
}

// A view panned absurdly far (both render paths) must draw nothing rather
// than feed out-of-range doubles into long casts.
TEST(DrawWaveform, FarOffscreenViewDrawsNothing) {
    auto ys = makeSeededSamples(5000, kSeed);
    WaveformPyramid pyr = buildPyramid(ys);
    for (double scaleX : {100.0, 44100.0 * 10.0}) {            // envelope, then line mode
        for (double panX : {1e18, -1e18}) {
            HeadlessRenderer hr(64, 32);
            ASSERT_NE(hr.renderer, nullptr);
            SDL_SetRenderDrawColor(hr.renderer, 0, 0, 0, 255);
            SDL_RenderClear(hr.renderer);
            drawWaveform(hr.renderer, ys, pyr, 44100.0, panX, scaleX, 16.0f, 16.0f, 64,
                         SDL_Color{255, 255, 255, 255}, 1, makeShading(1.0f), false);
            SDL_RenderPresent(hr.renderer);
            auto* px = (const uint8_t*)hr.surface->pixels;
            const int bpp = SDL_BYTESPERPIXEL(hr.surface->format);
            int lit = 0;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 64; ++x) {
                    const uint8_t* p = px + (size_t)y * hr.surface->pitch + (size_t)x * bpp;
                    lit += (p[0] || p[1] || p[2]);
                }
            EXPECT_EQ(lit, 0) << "scaleX=" << scaleX << " panX=" << panX;
        }
    }
}

TEST(DrawWaveform, EmptyInputDrawsNothingAndDoesNotCrash) {
    HeadlessRenderer hr(100, 100);
    ASSERT_NE(hr.renderer, nullptr);
    SDL_SetRenderDrawColor(hr.renderer, 10, 20, 30, 255);
    SDL_RenderClear(hr.renderer);

    std::vector<float> empty;
    WaveformPyramid pyr = buildPyramid(empty);
    drawWaveform(hr.renderer, empty, pyr, 44100.0, 0.0f, 1.0f, 50.0f, 50.0f, 100,
                 SDL_Color{255, 255, 255, 255}, 1, makeShading(1.0f), false);
    SDL_RenderPresent(hr.renderer);

    auto* px = (const uint8_t*)hr.surface->pixels;
    int bpp = SDL_BYTESPERPIXEL(hr.surface->format);
    const uint8_t* p0 = px;
    EXPECT_EQ(p0[0], 10);
    EXPECT_EQ(p0[1], 20);
    EXPECT_EQ(p0[2], 30);
    (void)bpp;
}

// The envelope-vs-line choice must depend only on the zoom (samples per
// screen pixel), not on the supersampling factor -- otherwise the app's
// interactive ss=2 pass and settled ss=16 pass draw different things at the
// same zoom and the view visibly snaps when it settles. Line mode is the only
// path that paints with `color`, so a pure-red line color tells them apart
// (the envelope's fixed palette is always gray: r == g == b).
TEST(DrawWaveform, LineVsEnvelopeChoiceIsIndependentOfSupersampling) {
    const int W = 200, H = 100;
    auto ys = makeSeededSamples(50000, kSeed);
    WaveformPyramid pyr = buildPyramid(ys);
    float peak = 0.0f;
    for (float v : ys) peak = std::max(peak, std::fabs(v));

    auto drawsRedLine = [&](double samplesPerPx, int ss) {
        HeadlessRenderer hr(W * ss, H * ss);
        SDL_SetRenderDrawColor(hr.renderer, 0, 0, 0, 255);
        SDL_RenderClear(hr.renderer);
        drawWaveform(hr.renderer, ys, pyr, 44100.0, 0.0f,
                     (float)(44100.0 / samplesPerPx), H / 2.0f, H / 2.0f, W,
                     SDL_Color{255, 0, 0, 255}, ss, makeShading(peak), false);
        SDL_RenderPresent(hr.renderer);
        auto* px = (const uint8_t*)hr.surface->pixels;
        int bpp = SDL_BYTESPERPIXEL(hr.surface->format);
        for (int y = 0; y < hr.surface->h; ++y)
            for (int x = 0; x < hr.surface->w; ++x) {
                const uint8_t* p = px + (size_t)y * hr.surface->pitch + (size_t)x * bpp;
                if (p[0] > 128 && p[1] == 0 && p[2] == 0) return true;
            }
        return false;
    };

    for (int ss : {1, 2, 4, 16}) {
        EXPECT_TRUE (drawsRedLine(kLineModeMaxSamplesPerPx * 0.5, ss)) << "ss=" << ss;
        EXPECT_FALSE(drawsRedLine(kLineModeMaxSamplesPerPx * 2.0, ss)) << "ss=" << ss;
    }
}

// Same seed, same call, twice: must produce byte-identical pixels. This is
// the property the benchmark below leans on to be replicable — if rendering
// weren't deterministic given identical input, per-iteration timings would
// also be comparing apples to oranges across runs.
TEST(DrawWaveform, DeterministicForFixedSeed) {
    const int W = 300, H = 150;
    auto ys = makeSeededSamples(50000, kSeed);
    WaveformPyramid pyr = buildPyramid(ys);
    float peak = 0.0f;
    for (float v : ys) peak = std::max(peak, std::fabs(v));

    auto renderOnce = [&]() {
        HeadlessRenderer hr(W, H);
        SDL_SetRenderDrawColor(hr.renderer, 0, 0, 0, 255);
        SDL_RenderClear(hr.renderer);
        drawWaveform(hr.renderer, ys, pyr, 44100.0, 0.0f,
                     (float)W / (float)ys.size() * 44100.0f, H / 2.0f,
                     H / 2.0f, W, SDL_Color{255, 255, 255, 255}, 2,
                     makeShading(peak), false);
        SDL_RenderPresent(hr.renderer);
        size_t bytes = (size_t)hr.surface->pitch * hr.surface->h;
        return std::vector<uint8_t>((uint8_t*)hr.surface->pixels,
                                     (uint8_t*)hr.surface->pixels + bytes);
    };

    auto a = renderOnce();
    auto b = renderOnce();
    EXPECT_EQ(a, b);
}

// ─── Benchmark: pyramid construction ────────────────────────────────────────
//
// buildPyramid() is a one-time cost paid at WAV load, not per frame — this
// confirms it stays small even for a multi-million-sample buffer (input.wav
// is ~3.9M mono samples). Left enabled (not DISABLED_) since a few builds of
// a few million samples is well under a second, unlike the full-redraw
// benchmark below.
TEST(WaveformPyramid, BenchmarkBuild) {
    constexpr int kPasses = 20;
    constexpr size_t kNumSamples = 4000000; // ~input.wav's mono sample count

    auto ys = makeSeededSamples(kNumSamples, kSeed);
    std::vector<double> buildMicros;
    buildMicros.reserve(kPasses);

    for (int i = 0; i < kPasses; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        WaveformPyramid pyr = buildPyramid(ys);
        auto t1 = std::chrono::steady_clock::now();
        ASSERT_EQ(pyr.n, (long)kNumSamples);
        buildMicros.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::sort(buildMicros.begin(), buildMicros.end());
    double total = std::accumulate(buildMicros.begin(), buildMicros.end(), 0.0);
    double mean = total / buildMicros.size();
    double median = buildMicros[buildMicros.size() / 2];

    printf("[BenchmarkBuild] passes=%d samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus\n",
           kPasses, kNumSamples, mean, median, buildMicros.front(), buildMicros.back());
    RecordProperty("mean_us", mean);
    RecordProperty("median_us", median);
}

// ─── Benchmark: full redraw ─────────────────────────────────────────────────
//
// Not a pass/fail perf gate (thresholds like that are flaky across machines
// and CI load) — it runs a fixed, seed-derived workload for a fixed number of
// passes and reports timing stats, so the *input* is exactly reproducible
// run-to-run even though wall-clock time naturally isn't. Compare successive
// runs on the same machine to judge whether a change regressed (or fixed)
// rendering cost.
//
// IMPORTANT CAVEAT: HeadlessRenderer uses SDL_CreateSoftwareRenderer, not
// the GPU-accelerated backend wav_main.cpp actually gets from
// SDL_CreateRenderer(window, nullptr). The two behave very differently here:
// the software renderer defers a call's real per-pixel rasterization cost
// until SDL_RenderPresent (measured directly -- SDL_RenderGeometry/RenderLine
// calls themselves are cheap to *enqueue*, but Present pays for all of it),
// so "fewer draw calls" doesn't reliably predict "faster" the way it does on
// a real GPU renderer -- this benchmark's numbers (mean ~32.4ms/median
// ~36.9ms per pass with the original per-column SDL_RenderLine code; the
// batched SDL_RenderGeometry version measured *slower* here, ~48-50ms) are
// software-renderer artifacts and should NOT be used to judge either the
// RenderGeometry batching or the pyramid.
//
// The real, decisive numbers came from instrumenting the actual windowed app
// (temporarily, then reverted) against input.wav at its real default
// settings (ss=16, full ~3.9M-sample file), A/B/C testing three configs of
// one renderPass call:
//   A: original SDL_RenderLine, no pyramid        -> ~29.7ms
//   B: batched SDL_RenderGeometry, no pyramid      -> ~26.0ms
//   C: batched SDL_RenderGeometry + WaveformPyramid -> ~14.8ms
// Both optimizations are real, additive wins on the renderer the app
// actually uses -- roughly halving frame cost together -- which is why both
// are wired in (see wav_main.cpp's buildPyramid(mono) call and drawWaveform's
// use of SDL_RenderGeometry above) despite what this specific benchmark says.
//
// DISABLED_ (GoogleTest convention): thousands of full-window redraws take
// tens of seconds, which is too slow to run on every `ctest`/`make test`.
// Run it explicitly with:
//   ./waveform_render_test --gtest_also_run_disabled_tests \
//       --gtest_filter='*BenchmarkManyPasses*'
TEST(DrawWaveform, DISABLED_BenchmarkManyPasses) {
    constexpr int kPasses = 3000;
    constexpr size_t kNumSamples = 200000; // ~4.5s @ 44.1kHz, zoomed out
    constexpr int W = 1280, H = 800;
    constexpr int kSupersample = 2;

    auto ys = makeSeededSamples(kNumSamples, kSeed);
    WaveformPyramid pyr = buildPyramid(ys); // built once, like at WAV load
    float peak = 0.0f;
    for (float v : ys) peak = std::max(peak, std::fabs(v));
    EnvelopeShading shading = makeShading(peak);

    HeadlessRenderer hr(W, H);
    ASSERT_NE(hr.renderer, nullptr);

    // Seeded per-pass jitter on pan/scale stands in for a user dragging /
    // zooming across many frames, so the benchmark doesn't just measure one
    // memoized column layout — but it's driven by the same fixed seed, so
    // the exact sequence of (panX, scaleX) values is identical every run.
    std::mt19937 jitterRng(kSeed);
    std::uniform_real_distribution<float> panJitter(-500.0f, 500.0f);
    std::uniform_real_distribution<float> scaleJitter(0.5f, 4.0f);

    std::vector<double> passMicros;
    passMicros.reserve(kPasses);

    for (int i = 0; i < kPasses; ++i) {
        float panX = panJitter(jitterRng);
        float scaleX = ((float)W / (float)kNumSamples * 44100.0f) * scaleJitter(jitterRng);

        auto t0 = std::chrono::steady_clock::now();
        SDL_SetRenderDrawColor(hr.renderer, 0, 0, 0, 255);
        SDL_RenderClear(hr.renderer);
        drawWaveform(hr.renderer, ys, pyr, 44100.0, panX, scaleX, H / 2.0f,
                     H / 2.0f, W, SDL_Color{255, 255, 255, 255},
                     kSupersample, shading, false);
        SDL_RenderPresent(hr.renderer);
        auto t1 = std::chrono::steady_clock::now();

        passMicros.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::sort(passMicros.begin(), passMicros.end());
    double total = std::accumulate(passMicros.begin(), passMicros.end(), 0.0);
    double mean = total / passMicros.size();
    double median = passMicros[passMicros.size() / 2];
    double p95 = passMicros[(size_t)(passMicros.size() * 0.95)];
    double minV = passMicros.front(), maxV = passMicros.back();

    printf("[BenchmarkManyPasses] passes=%d samples=%zu win=%dx%d ss=%d\n",
           kPasses, kNumSamples, W, H, kSupersample);
    printf("[BenchmarkManyPasses] mean=%.1fus median=%.1fus p95=%.1fus min=%.1fus max=%.1fus total=%.2fms\n",
           mean, median, p95, minV, maxV, total / 1000.0);

    RecordProperty("mean_us", mean);
    RecordProperty("median_us", median);
    RecordProperty("p95_us", p95);

    // Sanity bound only, generous enough to never flake on a loaded CI box —
    // it exists to catch an accidental O(n^2)-style regression, not to
    // enforce a performance target.
    EXPECT_LT(mean, 200000.0) << "drawWaveform mean pass time is unexpectedly high";
}
