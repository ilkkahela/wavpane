#pragma once

// Waveform rendering, kept out of wav_main.cpp so it can be tested directly
// (tests/waveform_render_test.cpp).

#include <SDL3/SDL.h>
#include "waveform_pyramid.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// ─── View math ────────────────────────────────────────────────────────────────

// Horizontal view math is in double: zoomed in to individual samples a few
// minutes into a file, panX reaches ~1e9 px, where a float can only step in
// ~100 px increments (the view would snap and samples land in the wrong place).
inline double toSX(double wx, double panX, double sx) { return panX + wx * sx; }
inline double toWX(double px, double panX, double sx) { return (px - panX) / sx; }

// floor/ceil'd sample position clamped to [-1, N] *before* the cast to long:
// a view panned or zoomed absurdly far would otherwise overflow it (UB).
inline long clampedIndex(double v, long N) {
    return (long)std::clamp(v, -1.0, (double)N);
}

// At or below this many samples per screen pixel, drawWaveform draws the
// sample line instead of the binned envelope.
inline constexpr double kLineModeMaxSamplesPerPx = 16.0;

// ─── Waveform rendering ─────────────────────────────────────────────────────
// Two strategies depending on zoom:
//
//  - Zoomed out (> kLineModeMaxSamplesPerPx): one envelope bar per texture
//    column, in three layers sized by that column's own statistics:
//
//      1. dark rim:   true min..max                 -> dim gray
//      2. white core: +/- the level-selected anchor -> near-white
//      3. hot spine:  +/- mean square (unrooted)    -> additive white
//
//    The anchors satisfy ms <= rms <= cubeM <= quadM <= peak (power-mean
//    inequality), so the core never pokes out past the rim. Keys 1-9
//    (`level`) only move where the core ends along those anchors; colors
//    fade with the column's loudness relative to the global peak.
//
//  - Zoomed in (<= kLineModeMaxSamplesPerPx): a 2px-wide line through the
//    actual samples.
//
// `ss` is a supersampling factor: the caller renders into an offscreen
// texture of `winW * ss` columns and downsamples it, which is what
// antialiases the result. Geometry is generated at texture resolution (not
// via SDL_SetRenderScale) to avoid 1-texel gaps between adjacent bars.
struct EnvelopeCol { float mn, mx; float ms, rms, cubeM, quadM; bool valid; };

// How the envelope is shaded.
// `level` (1-9): 1 puts the white core at `ms` (mostly dark), 8 at `peak`
// (near white), 9 is flat opaque white regardless of loudness.
// `globalPeak` is the reference for the per-column loudness fade.
struct EnvelopeShading { float globalPeak; int level = 5; };

// ─── Color pipeline ─────────────────────────────────────────────────────────
// Three pure stages (unit-tested without rendering):
//
//   1. curveForLevel: level -> gamma (how loudness maps onto rungs)
//   2. stepIntensity: column peak / global peak -> one of 13 fixed grayscale
//                     rungs, picked using that gamma
//   3. colorForStep:  rung + a layer's base color -> paint color
//
// Only the binning (stage 2) depends on level; the rungs and the coloring
// don't. The core boundary's *position* (coreBoundary) uses the same levelT
// curve, so brightness and boundary move together.

// 0..1 progression for levels 1..8, front-loaded (sqrt) so midtone levels
// brighten sooner instead of staying dim until the last few keys.
inline float levelT(int level) {
    constexpr float kLevelCurveGamma = 0.5f;
    return std::pow(std::clamp((float)(level - 1) / 7.0f, 0.0f, 1.0f), kLevelCurveGamma);
}

// Stage 1. hardWhite (level 9) is a color override that gamma can't express,
// so it rides along and colorForStep applies it.
struct FadeCurve {
    float gamma;      // > 1 lifts quiet columns toward brighter rungs
    bool  hardWhite;  // level 9: flat opaque white
};

inline FadeCurve curveForLevel(int level) {
    // lin^(1/gamma): gamma > 1 lifts loudness toward brighter rungs, < 1
    // crushes it toward darker ones. Level 9's gamma is unused.
    constexpr float kGammaMin = 0.6f; // level 1
    constexpr float kGammaMax = 6.0f; // level 8
    return FadeCurve{kGammaMin + (kGammaMax - kGammaMin) * levelT(level), level >= 9};
}

// Stage 2: posterize the column's loudness into one of 13 flat rungs (like
// an LED meter), floored so silence is dark gray rather than black.
struct SteppedIntensity { float fade; bool hardWhite; };

inline SteppedIntensity stepIntensity(float colPeak, float globalPeak, const FadeCurve& curve) {
    if (curve.hardWhite) return SteppedIntensity{1.0f, true};
    constexpr float kDarkFloor = 0.14f;
    constexpr int kMidtoneSteps = 11;
    constexpr int kFadeSteps = kMidtoneSteps + 1; // rungs 0..12: darkest, 11 midtones, brightest
    float lin     = std::clamp(colPeak / globalPeak, 0.0f, 1.0f);
    float shaped  = std::pow(lin, 1.0f / curve.gamma);
    float stepped = std::round(shaped * kFadeSteps) / (float)kFadeSteps;
    return SteppedIntensity{kDarkFloor + (1.0f - kDarkFloor) * stepped, false};
}

// Stage 3: fade the base RGB toward black by the rung. Alpha stays at the
// base value, except on a white backdrop, where it ramps toward opaque as
// the rung darkens -- otherwise near-black translucent colors would blend
// into pale gray against white.
inline SDL_Color colorForStep(SDL_Color base, SteppedIntensity step, bool bgIsWhite) {
    if (step.hardWhite) return SDL_Color{255, 255, 255, 255};
    float a = bgIsWhite ? (255.0f + (base.a - 255.0f) * step.fade) : (float)base.a;
    return SDL_Color{
        (uint8_t)std::clamp(base.r * step.fade, 0.0f, 255.0f),
        (uint8_t)std::clamp(base.g * step.fade, 0.0f, 255.0f),
        (uint8_t)std::clamp(base.b * step.fade, 0.0f, 255.0f),
        (uint8_t)std::clamp(a, 0.0f, 255.0f)
    };
}

// Where the white core ends for a column (or any window): `pos` in [0, 4]
// interpolates along its {ms, rms, cubeM, quadM, peak} anchors, with
// pos = levelT(level) * 4. Shared with the hover histogram.
inline float coreBoundary(const float (&anchors)[5], float pos) {
    const int lo = std::clamp((int)std::floor(pos), 0, 3);
    const float frac = std::clamp(pos - (float)lo, 0.0f, 1.0f);
    return anchors[lo] + (anchors[lo + 1] - anchors[lo]) * frac;
}

// The envelope's layer colors, before colorForStep's loudness fade.
constexpr SDL_Color kRimBase{150, 150, 168, 165};
constexpr SDL_Color kCoreBase{255, 255, 255, 255}; // opaque: loudest columns are true white
constexpr SDL_Color kHotBase{255, 255, 255, 190};

inline void drawWaveform(SDL_Renderer* r, const std::vector<float>& ys,
                          const WaveformPyramid& pyramid,
                          double sampleRate, double panX, double scaleX,
                          float centerY, float ampScale, int winW, SDL_Color color,
                          int ss, const EnvelopeShading& shading, bool bgIsWhite) {
    const long N = (long)ys.size();
    if (N == 0) return;

    // `color` is used only by the zoomed-in line path; the envelope uses the
    // fixed layer palette above.
    const int texW = winW * ss;
    const double samplesPerPx = sampleRate / scaleX;

    // Zoomed out: solid min..max bars rather than thin lines -- with many
    // samples per pixel a thin line only traces a sparse zigzag and looks
    // dark exactly where the signal is busiest. The threshold is per
    // *screen* pixel, independent of ss, so the interactive and settled
    // passes switch modes at the same zoom.
    if (samplesPerPx > kLineModeMaxSamplesPerPx) {
        static thread_local std::vector<EnvelopeCol> cols;
        cols.resize(texW);

        // Each column's sample window is widened to ~3x its width, overlapping
        // its neighbors -- a box-filter anti-alias that stops periodic content
        // from beating against the column grid (moire on pure tones).
        const float colW = 1.f / ss;
        for (int xt = 0; xt < texW; ++xt) {
            float xLogical = (float)xt / (float)ss;
            double wxL = toWX(xLogical - colW,          panX, scaleX);
            double wxR = toWX(xLogical + 1.f/ss + colW, panX, scaleX);
            long i0raw = clampedIndex(std::floor(wxL * sampleRate), N);
            long i1raw = clampedIndex(std::ceil (wxR * sampleRate), N);

            // Columns outside the recording stay empty (clamping would paint
            // a flat shelf at the first/last sample's value).
            if (i1raw < 0 || i0raw >= N) { cols[xt].valid = false; continue; }

            long i0 = std::clamp(i0raw, 0L, N - 1);
            long i1 = std::clamp(i1raw, 0L, N - 1);
            if (i1 < i0) i1 = i0;

            BinStat b = queryPyramid(pyramid, ys, i0, i1);
            double count = (double)b.count;
            double meanSq = (double)b.sumSq / count;
            cols[xt] = {
                b.mn, b.mx,
                (float)meanSq,
                (float)std::sqrt(meanSq),
                (float)std::cbrt((double)b.sumCube / count),
                (float)std::pow ((double)b.sumQuad / count, 0.25),
                true,
            };
        }

        // Each layer is one batched SDL_RenderGeometry call rather than one
        // draw call per column.
        static thread_local std::vector<SDL_Vertex> verts;
        auto addBar = [&](int xt, float top, float bot, SDL_Color color) {
            SDL_FColor fc{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
            float xL = (float)xt, xR = xL + 1.0f;
            SDL_Vertex a{{xL, top}, fc, {0, 0}};
            SDL_Vertex b{{xR, top}, fc, {0, 0}};
            SDL_Vertex c{{xR, bot}, fc, {0, 0}};
            SDL_Vertex d{{xL, bot}, fc, {0, 0}};
            verts.push_back(a); verts.push_back(b); verts.push_back(c);
            verts.push_back(a); verts.push_back(c); verts.push_back(d);
        };
        auto flushLayer = [&]() {
            if (!verts.empty()) SDL_RenderGeometry(r, nullptr, verts.data(), (int)verts.size(), nullptr, 0);
            verts.clear();
        };

        // Every layer's color fades with the column's loudness (see the color
        // pipeline above); geometry doesn't, and the minimum thickness keeps
        // silence visible as a thin line.
        const float pos = levelT(shading.level) * 4.0f; // 0..4 across the 5 anchors
        const FadeCurve fadeCurve = curveForLevel(shading.level);
        const float minH = (float)ss;

        // 1. Dark rim: the column's true min..max.
        verts.reserve((size_t)texW * 6);
        for (int xt = 0; xt < texW; ++xt) {
            const auto& c = cols[xt];
            if (!c.valid) continue;
            float colPeak = std::max(std::fabs(c.mn), std::fabs(c.mx));
            SDL_Color dc = colorForStep(kRimBase, stepIntensity(colPeak, shading.globalPeak, fadeCurve), bgIsWhite);
            float top = (centerY - c.mx * ampScale) * ss;
            float bot = (centerY - c.mn * ampScale) * ss;
            if (bot - top < minH) { float mid = (top + bot) * 0.5f; top = mid - minH * 0.5f; bot = mid + minH * 0.5f; }
            addBar(xt, top, bot, dc);
        }
        flushLayer();
        // 2. White core: symmetric, out to the level-selected anchor.
        verts.reserve((size_t)texW * 6);
        for (int xt = 0; xt < texW; ++xt) {
            const auto& c = cols[xt];
            if (!c.valid) continue;
            float colPeak = std::max(std::fabs(c.mn), std::fabs(c.mx));
            SDL_Color wc = colorForStep(kCoreBase, stepIntensity(colPeak, shading.globalPeak, fadeCurve), bgIsWhite);
            float anchors[5] = { c.ms, c.rms, c.cubeM, c.quadM, colPeak };
            float rad = coreBoundary(anchors, pos) * ampScale * ss;
            addBar(xt, centerY * ss - rad, centerY * ss + rad, wc);
        }
        flushLayer();
        // 3. Hot spine: mean square, independent of level. Additive blending
        // makes it read as "hot" rather than as another flat gray.
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
        verts.reserve((size_t)texW * 6);
        for (int xt = 0; xt < texW; ++xt) {
            const auto& c = cols[xt];
            if (!c.valid) continue;
            float colPeak = std::max(std::fabs(c.mn), std::fabs(c.mx));
            SDL_Color hc = colorForStep(kHotBase, stepIntensity(colPeak, shading.globalPeak, fadeCurve), bgIsWhite);
            float rMs = std::min(c.ms, colPeak) * ampScale * ss;
            float top = centerY * ss - rMs, bot = centerY * ss + rMs;
            if (bot - top < minH) { float mid = (top + bot) * 0.5f; top = mid - minH * 0.5f; bot = mid + minH * 0.5f; }
            addBar(xt, top, bot, hc);
        }
        flushLayer();
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    } else {
        // Segments are filled quads ~2 screen pixels (2*ss texels) wide: a
        // hairline would be 1 texel = 1/ss pixel and downsample to faint gray.
        constexpr float kLineWidthPx = 2.0f;
        const float halfW = kLineWidthPx * ss * 0.5f;
        const SDL_FColor fc{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};

        double wxL = toWX(0.0,          panX, scaleX);
        double wxR = toWX((double)winW, panX, scaleX);
        long i0 = std::clamp(clampedIndex(std::floor(wxL * sampleRate) - 1.0, N), 0L, N - 1);
        long i1 = std::clamp(clampedIndex(std::ceil (wxR * sampleRate) + 1.0, N), 0L, N - 1);

        auto toTex = [&](long i, float& sx, float& sy) {
            sx = (float)(toSX((double)i / sampleRate, panX, scaleX) * ss);
            sy = (centerY - ys[i] * ampScale) * ss;
        };
        if (i1 == i0) {
            float sx, sy; toTex(i0, sx, sy);
            SDL_FRect dot{sx - halfW, sy - halfW, halfW * 2.0f, halfW * 2.0f};
            SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
            SDL_RenderFillRect(r, &dot);
        } else {
            static thread_local std::vector<SDL_Vertex> verts;
            verts.clear();
            verts.reserve((size_t)(i1 - i0) * 6);

            float px, py; toTex(i0, px, py);
            for (long i = i0 + 1; i <= i1; ++i) {
                float sx, sy; toTex(i, sx, sy);

                float dx = sx - px, dy = sy - py;
                float len = std::sqrt(dx * dx + dy * dy);
                float nx, ny; // unit perpendicular
                if (len > 1e-6f) { nx = -dy / len * halfW; ny = dx / len * halfW; }
                else             { nx = halfW; ny = 0.0f; }

                SDL_Vertex a{{px + nx, py + ny}, fc, {0, 0}};
                SDL_Vertex b{{px - nx, py - ny}, fc, {0, 0}};
                SDL_Vertex c{{sx - nx, sy - ny}, fc, {0, 0}};
                SDL_Vertex d{{sx + nx, sy + ny}, fc, {0, 0}};
                verts.push_back(a); verts.push_back(b); verts.push_back(c);
                verts.push_back(a); verts.push_back(c); verts.push_back(d);

                px = sx; py = sy;
            }
            SDL_RenderGeometry(r, nullptr, verts.data(), (int)verts.size(), nullptr, 0);
        }
    }
}
