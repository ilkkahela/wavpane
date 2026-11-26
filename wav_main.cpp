#include <SDL3/SDL.h>
#include "audio_loader.hpp"
#include "waveform_render.hpp"
#include "hover_stats.hpp"
#include "time_format.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

#ifndef WAVPANE_VERSION
#define WAVPANE_VERSION "dev"
#endif

// ─── Text ─────────────────────────────────────────────────────────────────────

// Single color for all on-screen text (readouts, HUD panels, help screen).
constexpr SDL_Color kTextColor{210, 210, 230, 255};
// Waveform color in the zoomed-in sample-line mode (the zoomed-out envelope
// uses its own fixed layer palette, see waveform_render.hpp).
constexpr SDL_Color kLineColor{255, 255, 255, 230};

// ─── Hover readout ────────────────────────────────────────────────────────────
// A histogram of the samples under the cursor in a panel docked to the left
// edge. The panel shares the main view's amplitude axis: every 2px row of the
// screen is one bin, and bars grow rightward with the count. Amber lines mark
// ±RMS. See hover_stats.hpp for the math.

constexpr SDL_Color kRmsColor{255, 180, 60, 255};
constexpr SDL_Color kCursorYColor{0, 230, 255, 255}; // cyan: amplitude crosshair + its readout
constexpr float kHistPanelW = 160.0f; // at 100% display scale

static void drawHistogramPanel(SDL_Renderer* r, const HoverStats& s, const ValueHistogram& h,
                               float panelW, float panY, float scaleY, int winH, bool logScale,
                               const EnvelopeShading& shading) {
    const float x0 = 0.0f;
    SDL_FRect bg{x0, 0.0f, panelW, (float)winH};
    SDL_SetRenderDrawColor(r, 12, 12, 24, 255);
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 90, 90, 130, 255);
    SDL_RenderLine(r, x0 + panelW - 1.0f, 0.0f, x0 + panelW - 1.0f, (float)winH);

    auto valueToY = [&](float v) { return panY - v * scaleY; };

    // Bars are colored like a sideways slice of the main view's envelope at
    // this window: |value| inside the mean square gets the hot spine, inside
    // the level-selected core boundary the white core, beyond it the dark
    // rim -- same layers, blend modes and loudness fade as drawWaveform, so
    // keys 1-9 move the white/dark split here exactly as they do there.
    const int nBins = (int)h.bins.size();
    uint32_t maxCount = 0;
    for (uint32_t c : h.bins) maxCount = std::max(maxCount, c);
    if (nBins > 0 && maxCount > 0) {
        const float barMax = panelW * (148.0f / 160.0f); // leaves a margin before the edge
        const float denom = logScale ? std::log1p((float)maxCount) : (float)maxCount;
        const float anchors[5] = {s.ms, s.rms, s.cubeM, s.quadM, s.peak};
        const float boundary = coreBoundary(anchors, levelT(shading.level) * 4.0f);
        const SteppedIntensity step = stepIntensity(s.peak, shading.globalPeak, curveForLevel(shading.level));
        const float binW = h.width();

        // limit < 0: every nonzero bin (the rim covers the full extent).
        auto drawLayer = [&](SDL_Color base, float limit) {
            const SDL_Color c = colorForStep(base, step, /*bgIsWhite=*/false);
            SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255); // fully opaque bars
            for (int k = 0; k < nBins; ++k) {
                if (h.bins[k] == 0) continue;
                if (limit >= 0.0f && std::fabs(h.center(k)) > limit) continue;
                float v = logScale ? std::log1p((float)h.bins[k]) : (float)h.bins[k];
                float len = std::max(1.0f, std::round(v / denom * barMax));
                float yTop = std::round(valueToY(h.lo + (float)(k + 1) * binW));
                float yBot = std::round(valueToY(h.lo + (float)k * binW));
                SDL_FRect bar{x0, yTop, len, std::max(1.0f, yBot - yTop)};
                SDL_RenderFillRect(r, &bar);
            }
        };
        drawLayer(kRimBase, -1.0f);
        drawLayer(kCoreBase, boundary);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
        drawLayer(kHotBase, std::min(s.ms, s.peak));
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    }

    const float xR = x0 + panelW - 1.0f;
    SDL_SetRenderDrawColor(r, 90, 90, 130, 255);
    SDL_RenderLine(r, x0, std::round(panY), xR, std::round(panY));
    if (s.rms > 0.0f) {
        SDL_SetRenderDrawColor(r, kRmsColor.r, kRmsColor.g, kRmsColor.b, 255);
        for (float v : {-s.rms, s.rms}) SDL_RenderLine(r, x0, std::round(valueToY(v)), xR, std::round(valueToY(v)));
    }
}

// ─── Help ─────────────────────────────────────────────────────────────────────
// Shared by the in-app help screen (F1) and the terminal usage text.

struct HelpLine { const char* key; const char* desc; };
constexpr HelpLine kHelpLines[] = {
    {"F1",           "Toggle the help screen"},
    {"F",            "Toggle fullscreen"},
    {"Esc",          "Close help, leave fullscreen, or quit"},
    {"Home",         "Reset view (pan/zoom)"},
    {"Tab",          "Toggle background color: black / white"},
    {"G",            "Toggle background grid"},
    {"H",            "Toggle hover histogram (left edge)"},
    {"L",            "Hover histogram counts: linear / log"},
    {"+ / =",        "Increase antialiasing quality (2x/4x/8x/16x, as far as the GPU allows)"},
    {"-",            "Decrease antialiasing quality"},
    {"1-9",          "Move the white/dark border, gamma-spaced (1=dark data, 8=near white, 9=flat white)"},
    {"",             ""},
    {"Left-drag",    "Pan the view"},
    {"Scroll wheel", "Zoom time + amplitude"},
    {"Shift+Scroll", "Zoom time axis only"},
    {"Drop a file",  "Open a WAV file dragged onto the window"},
};

static void printUsage(std::FILE* out) {
    std::fprintf(out,
        "wavpane %s - interactive WAV waveform viewer\n"
        "\n"
        "Usage: wavpane FILE.wav\n"
        "       wavpane --help\n"
        "\n"
        "Opens FILE.wav in a window. While it's open, drag another WAV file\n"
        "onto the window to switch to it.\n"
        "\n"
        "Controls:\n",
        WAVPANE_VERSION);
    for (const auto& l : kHelpLines) {
        if (l.key[0] == '\0') { std::fputc('\n', out); continue; }
        std::fprintf(out, "  %-14s %s\n", l.key, l.desc);
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // A file argument is required; without one, explain usage and exit.
    if (argc < 2) { printUsage(stderr); return 1; }
    if (const std::string a = argv[1]; a == "-h" || a == "--help") { printUsage(stdout); return 0; }

    // Identifier = Wayland app_id / X11 WM_CLASS: lets the desktop match the
    // window to wavpane.desktop (taskbar icon, grouping).
    SDL_SetAppMetadata("wavpane", WAVPANE_VERSION, "wavpane");

    // ── SDL init ──────────────────────────────────────────────────────────────
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    // HIGH_PIXEL_DENSITY: render at the display's real resolution. Without
    // it, a Wayland compositor with 125-200% scaling upscales a low-res
    // buffer and everything looks blurry. All drawing below is therefore in
    // physical pixels (SDL_GetWindowSizeInPixels, events converted with
    // SDL_ConvertEventToRenderCoordinates), and fixed UI sizes are
    // multiplied by uiScale.
    const int WIN_W = 1280, WIN_H = 800; // in points (logical size)
    SDL_Window* window = SDL_CreateWindow(
        "wavpane", WIN_W, WIN_H, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { SDL_Quit(); return 1; }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (!SDL_SetRenderVSync(renderer, -1)) SDL_SetRenderVSync(renderer, 1);

    // A high supersample factor on a wide window can ask for a texture wider
    // than the GPU actually supports (e.g. 16x on a 1280px window is a 20480px
    // texture) — SDL_CreateTexture then just silently returns null and that
    // quality level renders nothing. Query the real limit and clamp against it
    // instead of failing quietly.
    int maxTexSize = 4096;
    {
        SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
        if (props) {
            Sint64 v = SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0);
            if (v > 0) maxTexSize = (int)v;
        }
    }

    int winW = WIN_W, winH = WIN_H; // in pixels
    SDL_GetWindowSizeInPixels(window, &winW, &winH);

    // Display scale (1.0 = 100%, 2.0 = 200%; from the compositor on Wayland,
    // Xft.dpi / GDK_SCALE on X11). Fixed UI sizes are multiplied by it.
    // Text scales are rounded to whole numbers so the 8x8 debug font stays
    // pixel-crisp.
    float uiScale = 1.0f;
    auto updateUiScale = [&]() {
        const float sc = SDL_GetWindowDisplayScale(window);
        uiScale = (sc > 0.0f && std::isfinite(sc)) ? sc : 1.0f;
    };
    updateUiScale();
    auto textScale = [&](float base) { return std::max(1.0f, std::round(base * uiScale)); };

    // The open file; empty until one is loaded (command line or drop).
    LoadedAudio audio;
    bool haveAudio = false;

    // Horizontal state is double: deep zoom far into a file pushes panX to
    // ~1e9 px, beyond float's pixel precision (see toSX).
    double scaleX, panX, tScaleX, tPanX;
    float  scaleY, panY, tScaleY, tPanY;

    auto resetView = [&]() {
        const float margin = 50.0f * uiScale;
        scaleX = std::max(1.0, (double)winW - 2.0 * margin) / std::max(audio.duration, 1e-6);
        scaleY = std::max(1.0f, (float)winH - 2.0f * margin) * 0.5f; // amplitude ±1 must fit above AND below center
        panX   = margin;
        panY   = (float)winH * 0.5f;         // zero-amplitude line at vertical center
        tScaleX = scaleX; tScaleY = scaleY; tPanX = panX; tPanY = panY;
    };
    resetView();

    bool  dragging = false;
    float  dragSMX = 0, dragSMY = 0, dragSPY = 0;
    double dragSPX = 0;
    float mouseX = 0, mouseY = 0;
    bool  mouseInside = false;

    // Hover readout (H toggles, L switches the histogram's count axis between
    // linear and log). The measured window is the cursor's column
    // +/- kHoverHalfPx (times uiScale), widened to kHoverMinSamples when
    // zoomed far in.
    constexpr float    kHoverHalfPx     = 4.5f;
    constexpr long     kHoverMinSamples = 64;
    constexpr uint32_t kHoverMaxBinned  = 1u << 17;  // 131,072 samples
    bool showHover = true;
    bool hoverLogScale = false;
    HoverStats     hover;              // recomputed when [hoverI0, hoverI1] changes
    ValueHistogram hoverHist;          // ...or when the bins' screen layout does
    HistLayout     hoverLayout;
    long hoverI0 = -1, hoverI1 = -1;

    const Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 lastTick = SDL_GetPerformanceCounter();

    bool showHelp = false; // toggled via F1

    // "Press F1 for help" flashes at the top of the screen for 2.5s on startup.
    const Uint64 startTick = lastTick;
    const Uint64 startHintUntilTick = startTick + (Uint64)(2.5 * (double)perfFreq);

    // Adaptive supersampling for waveform antialiasing. The waveform is drawn
    // into an offscreen texture at `ss` times the window's resolution, then
    // blitted back at 1x with linear filtering; the GPU's bilinear downfilter
    // does the actual antialiasing. Kept transparent so grid/axis/text (drawn
    // at native resolution, unaffected) show through underneath and stay crisp.
    //
    // The factor is capped by the GPU's max texture size (see maxSS): with
    // the common 16384px limit, a 1920px-wide window tops out at 8x. Memory
    // grows with its square -- 8x on 1920x1080 is a ~530 MB texture -- so the
    // default is a modest 4x.
    //
    // Redoing that at a high factor (e.g. 16x -> 256 samples per output pixel)
    // every single frame would cost real GPU time for no visible benefit while
    // the view is static. So: while the user is actively panning/zooming,
    // redraw every frame into a small, cheap `waveTexLo` (fixed low factor) for
    // snappiness; once the view has been still for a moment, redraw once into
    // `waveTexHi` at the configured high-fidelity factor and just keep
    // re-blitting that cached texture (a near-free textured quad) until the
    // view actually changes again.
    int ssQuality = 4;                    // configurable via +/- : 2, 4, 8, or 16
    // Factor for the interactive pass. `ss` is also the envelope's column
    // width (each texture column bins sampleRate / (ss*scaleX) samples), so
    // this low value looks coarser and softer than the settled pass -- an
    // accepted trade for staying cheap while it's redrawn every frame.
    constexpr int kSSInteractive = 2;

    // Background: near-black or white, toggled via Tab.
    constexpr SDL_Color kBgDark{10, 10, 16, 255}, kBgWhite{255, 255, 255, 255};
    bool bgIsWhite = false;
    bool showGrid = true; // toggled via 'G'

    // Keys 1-9 set shading.level: where the white core ends and the dark rim
    // begins. See EnvelopeShading in waveform_render.hpp.
    EnvelopeShading shading{ 1.0f, 5 }; // globalPeak is set per file in openFile

    SDL_Texture* waveTexLo = nullptr; int waveTexLoW = 0, waveTexLoH = 0;
    SDL_Texture* waveTexHi = nullptr; int waveTexHiW = 0, waveTexHiH = 0;
    // Holds the hi-res pass after mip-chain downsampling to native resolution
    // (see downsampleMipChain below) so the per-frame blit stays a cheap 1:1
    // copy instead of redoing the downsample every frame.
    SDL_Texture* waveTexHiFinal = nullptr; int waveTexHiFinalW = 0, waveTexHiFinalH = 0;

    // Cache for downsampleMipChain's intermediate halving-step textures below:
    // reused across frames instead of SDL_CreateTexture/SDL_DestroyTexture-ing
    // them on every hi-quality redraw. Indexed by step (0 = first halving of
    // the oversized source); a step's texture is only recreated when the size
    // it needs actually changes (window resize, AA quality change, etc).
    std::vector<SDL_Texture*> mipChainTemps;
    std::vector<std::pair<int, int>> mipChainSizes;

    bool  hiResValid  = false;
    int   cachedWinW = 0, cachedWinH = 0, cachedSSQuality = 0, cachedLevel = 0;
    double cachedPanX = 0, cachedScaleX = 0;
    float  cachedPanY = 0, cachedScaleY = 0;
    bool   cachedBgIsWhite = false;

    // Largest power-of-two ss that keeps winW*ss and winH*ss within
    // maxTexSize (a power of two, so downsampleMipChain halves cleanly).
    auto maxSS = [&]() {
        int m = std::min(maxTexSize / std::max(winW, 1), maxTexSize / std::max(winH, 1));
        return (int)std::bit_floor((unsigned)std::max(1, m));
    };
    auto clampSS = [&](int ss) { return std::clamp(ss, 1, maxSS()); };

    auto ensureTex = [&](SDL_Texture*& tex, int& tw, int& th, int ss) -> bool {
        int needW = winW * ss, needH = winH * ss;
        // Same size as last time: keep the texture -- or, if that size failed
        // to allocate, don't retry (and re-log) it every frame.
        if (tw == needW && th == needH) return false;
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                 SDL_TEXTUREACCESS_TARGET, needW, needH);
        tw = needW; th = needH;
        if (tex) {
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        } else {
            SDL_Log("waveform texture alloc failed at %dx%d: %s", needW, needH, SDL_GetError());
        }
        return true;
    };

    // A single LINEAR-filtered blit straight from an ss-times-oversized
    // texture down to native size only ever samples a 2x2 texel neighborhood
    // in *source* space -- at ss=8 that covers 1/16th of each axis's true
    // footprint, so most of the supersampled detail is just aliased past
    // rather than averaged in. Repeatedly halving instead (each step is a
    // true 2:1 reduction, where a 2x2 LINEAR sample legitimately covers the
    // whole source footprint of each output texel) approximates a proper box
    // filter over the full ss x ss neighborhood. Renders into `dst`
    // (already sized dstW x dstH); leaves the render target reset to the
    // screen afterward.
    auto downsampleMipChain = [&](SDL_Texture* src, int srcW, int srcH,
                                   SDL_Texture* dst, int dstW, int dstH) {
        SDL_Texture* cur = src;
        int curW = srcW, curH = srcH;
        size_t step = 0;
        while (curW > dstW * 2 && curH > dstH * 2) {
            int nextW = std::max(dstW, curW / 2);
            int nextH = std::max(dstH, curH / 2);

            if (step >= mipChainTemps.size()) {
                mipChainTemps.push_back(nullptr);
                mipChainSizes.push_back({0, 0});
            }
            if (mipChainSizes[step] != std::pair(nextW, nextH)) {
                if (mipChainTemps[step]) SDL_DestroyTexture(mipChainTemps[step]);
                mipChainTemps[step] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                                         SDL_TEXTUREACCESS_TARGET, nextW, nextH);
                mipChainSizes[step] = {nextW, nextH};
                if (mipChainTemps[step]) {
                    SDL_SetTextureScaleMode(mipChainTemps[step], SDL_SCALEMODE_LINEAR);
                    SDL_SetTextureBlendMode(mipChainTemps[step], SDL_BLENDMODE_BLEND);
                }
            }
            SDL_Texture* next = mipChainTemps[step];
            if (!next) break;

            SDL_SetRenderTarget(renderer, next);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, cur, nullptr, nullptr);
            cur = next; curW = nextW; curH = nextH;
            ++step;
        }
        // Fewer steps needed than before (e.g. AA quality lowered, window
        // shrunk) -- drop the now-unused trailing cached textures instead of
        // holding onto oversized ones forever.
        while (mipChainTemps.size() > step) {
            SDL_DestroyTexture(mipChainTemps.back());
            mipChainTemps.pop_back();
            mipChainSizes.pop_back();
        }

        SDL_SetRenderTarget(renderer, dst);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, cur, nullptr, nullptr);
        SDL_SetRenderTarget(renderer, nullptr);
    };

    // Error banner (failed open), shown for a few seconds.
    std::string statusMsg;
    Uint64 statusUntil = 0;

    // Loads `path` and swaps it in. On failure the current file (if any)
    // stays open and the error is shown in the window.
    auto openFile = [&](const std::string& path) {
        try {
            audio = loadAudio(path);
        } catch (const std::exception& e) {
            SDL_Log("Could not open %s: %s", path.c_str(), e.what());
            statusMsg = std::format("Could not open {}: {}",
                                    std::filesystem::path(path).filename().string(), e.what());
            statusUntil = SDL_GetPerformanceCounter() + 6 * perfFreq;
            return;
        }
        haveAudio = true;
        shading.globalPeak = audio.globalPeak;
        resetView();
        hover = {}; hoverHist = {}; hoverLayout = {};
        hoverI0 = hoverI1 = -1;
        hiResValid = false;
        statusMsg.clear();
        SDL_SetWindowTitle(window, std::format("{} - wavpane", audio.name).c_str());
    };

    // Text label at `scale`, top-left at (x, y) in pixels.
    auto drawText = [&](float x, float y, float scale, const std::string& text) {
        SDL_SetRenderScale(renderer, scale, scale);
        SDL_RenderDebugText(renderer, x / scale, y / scale, text.c_str());
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    };

    // Drawn last in every frame, with or without a file: the startup hint,
    // the error banner and the help screen.
    auto drawOverlays = [&](Uint64 now) {
        // ── Startup hint ──────────────────────────────────────────────────
        // "Press F1 for help": a single slow fade in and back out over the
        // full 2.5s window, not a repeating blink.
        if (now < startHintUntilTick) {
            const float hintScale = textScale(3.0f);
            const float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * hintScale;
            const std::string hint = "Press F1 for help";
            float hw = (float)hint.size() * ch;
            float hx = ((float)winW - hw) * 0.5f;
            float hy = 20.0f * uiScale;

            constexpr float kPi = 3.14159265358979323846f;
            constexpr double kHintDuration = 2.5;
            double tSec = (double)(now - startTick) / (double)perfFreq;
            float flash = std::sin((float)(tSec / kHintDuration) * kPi); // one rise-and-fall over the whole window
            Uint8 alpha = (Uint8)std::clamp(flash * 255.0f, 0.0f, 255.0f);

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(alpha * 0.6f));
            SDL_FRect bg{hx - 10.0f * uiScale, hy - 6.0f * uiScale, hw + 20.0f * uiScale, ch + 12.0f * uiScale};
            SDL_RenderFillRect(renderer, &bg);
            SDL_SetRenderDrawColor(renderer, kTextColor.r, kTextColor.g, kTextColor.b, alpha);
            drawText(hx, hy, hintScale, hint);
        }

        // ── Error banner ──────────────────────────────────────────────────
        if (!statusMsg.empty() && now < statusUntil) {
            const float sc = textScale(2.0f);
            const float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * sc;
            const float pad = 10.0f * uiScale;
            // Cut to the window width rather than run off the edge.
            const size_t maxChars = (size_t)std::max(4.0f, ((float)winW - 4.0f * pad) / ch);
            std::string msg = statusMsg;
            if (msg.size() > maxChars) msg = msg.substr(0, maxChars - 3) + "...";
            const float w = (float)msg.size() * ch + 2.0f * pad;
            const float x = std::round(((float)winW - w) * 0.5f);
            const float y = 64.0f * uiScale;
            SDL_SetRenderDrawColor(renderer, 110, 24, 24, 235);
            SDL_FRect bg{x, y, w, ch + 2.0f * pad};
            SDL_RenderFillRect(renderer, &bg);
            SDL_SetRenderDrawColor(renderer, 255, 120, 120, 255);
            SDL_RenderRect(renderer, &bg);
            SDL_SetRenderDrawColor(renderer, 255, 235, 235, 255);
            drawText(x + pad, y + pad, sc, msg);
        }

        // ── Help screen (F1) ─────────────────────────────────────────────
        if (showHelp) {
            const float helpScale = textScale(2.0f);
            const float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * helpScale;
            const float u = uiScale;
            const auto& lines = kHelpLines;
            constexpr int numLines = (int)(sizeof(lines) / sizeof(lines[0]));

            const float keyColW = 15.0f * ch;
            // Sized to the longest description rather than a fixed constant,
            // since doubling ch would otherwise overflow a fixed panel width.
            size_t maxDescLen = std::string("HELP  (F1 or Esc to close)").size();
            for (const auto& l : lines) maxDescLen = std::max(maxDescLen, std::string(l.desc).size());
            const float panelW = keyColW + (float)maxDescLen * ch + 32.0f * u;
            const float lineH = ch + 6.0f * u;
            const float panelH = lineH * (float)(numLines + 2) + 20.0f * u;
            const float px = ((float)winW - panelW) * 0.5f;
            const float py = ((float)winH - panelH) * 0.5f;

            SDL_SetRenderDrawColor(renderer, 12, 12, 22, 235);
            SDL_FRect hbg{px, py, panelW, panelH};
            SDL_RenderFillRect(renderer, &hbg);
            SDL_SetRenderDrawColor(renderer, 90, 90, 130, 255);
            SDL_RenderRect(renderer, &hbg);

            SDL_SetRenderDrawColor(renderer, kTextColor.r, kTextColor.g, kTextColor.b, kTextColor.a);
            drawText(px + 12.0f * u, py + 10.0f * u, helpScale, "HELP  (F1 or Esc to close)");
            for (int i = 0; i < numLines; ++i) {
                if (lines[i].key[0] == '\0') continue; // spacer row
                float ly = py + 10.0f * u + lineH * (float)(i + 1.5f);
                drawText(px + 16.0f * u, ly, helpScale, lines[i].key);
                drawText(px + 16.0f * u + keyColW, ly, helpScale, lines[i].desc);
            }
        }
    };

    // Shown while no file is open (only if the file from the command line
    // failed to load): a drop zone.
    auto drawEmptyState = [&]() {
        const float u = uiScale;
        const float inset = 40.0f * u;
        SDL_SetRenderDrawColor(renderer, 90, 90, 130, 255);
        if ((float)winW > 2.0f * inset && (float)winH > 2.0f * inset) {
            SDL_FRect zone{inset, inset, (float)winW - 2.0f * inset, (float)winH - 2.0f * inset};
            SDL_RenderRect(renderer, &zone);
        }
        const std::string l1 = "Drop a WAV file here";
        const std::string l2 = "(could not open the file from the command line)";
        const float s1 = textScale(3.0f), s2 = textScale(2.0f);
        const float ch1 = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * s1;
        const float ch2 = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * s2;
        const float gap = 16.0f * u;
        const float y1 = std::round(((float)winH - (ch1 + gap + ch2)) * 0.5f);
        SDL_SetRenderDrawColor(renderer, kTextColor.r, kTextColor.g, kTextColor.b, kTextColor.a);
        drawText(std::round(((float)winW - (float)l1.size() * ch1) * 0.5f), y1, s1, l1);
        SDL_SetRenderDrawColor(renderer, kTextColor.r, kTextColor.g, kTextColor.b, 150);
        drawText(std::round(((float)winW - (float)l2.size() * ch2) * 0.5f), y1 + ch1 + gap, s2, l2);
    };

    // Open the file given on the command line. If that fails, the error is
    // shown in the window, which stays empty until a file is dropped on it.
    openFile(argv[1]);

    // A drop can carry several files; only the first one is opened.
    bool dropTaken = false;

    bool running = true;
    SDL_Event ev;

    // Restart the frame clock so setup time isn't counted as frame 1's dt.
    lastTick = SDL_GetPerformanceCounter();

    while (running) {
        const Uint64 now = SDL_GetPerformanceCounter();
        const float  dt  = (float)(now - lastTick) / (float)perfFreq;
        lastTick = now;
        SDL_GetWindowSizeInPixels(window, &winW, &winH);

        while (SDL_PollEvent(&ev)) {
            // Mouse/wheel/drop positions arrive in points; everything here
            // draws in pixels.
            SDL_ConvertEventToRenderCoordinates(renderer, &ev);
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;

            case SDL_EVENT_KEY_DOWN:
                switch (ev.key.key) {
                // Fullscreen is SDL's borderless desktop mode (no fullscreen
                // display mode is set): the window covers the whole monitor,
                // panels and title bar included, without a video mode switch.
                // The state is read back from the window flags, since the
                // window manager can also change it.
                case SDLK_F:
                    SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
                    break;
                case SDLK_ESCAPE: // innermost first: help, then fullscreen, then quit
                    if (showHelp)
                        showHelp = false;
                    else if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)
                        SDL_SetWindowFullscreen(window, false);
                    else
                        running = false;
                    break;
                case SDLK_HOME: resetView(); break;
                case SDLK_F1:
                    showHelp = !showHelp;
                    break;
                case SDLK_TAB:
                    bgIsWhite = !bgIsWhite;
                    break;
                case SDLK_G:
                    showGrid = !showGrid;
                    break;
                case SDLK_H:
                    showHover = !showHover;
                    break;
                case SDLK_L:
                    hoverLogScale = !hoverLogScale;
                    break;
                case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS:
                    // Only to levels the GPU can render at this window size.
                    if (clampSS(ssQuality) * 2 <= std::min(16, maxSS())) ssQuality = clampSS(ssQuality) * 2;
                    break;
                case SDLK_MINUS: case SDLK_KP_MINUS:
                    ssQuality = std::max(clampSS(ssQuality) / 2, 2);
                    break;
                case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: case SDLK_5:
                case SDLK_6: case SDLK_7: case SDLK_8: case SDLK_9:
                    shading.level = (int)(ev.key.key - SDLK_1) + 1;
                    break;
                default: break;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    dragSMX = ev.button.x; dragSMY = ev.button.y;
                    dragSPX = tPanX; dragSPY = tPanY;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT) dragging = false;
                break;

            case SDL_EVENT_WINDOW_MOUSE_ENTER: mouseInside = true;  break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: mouseInside = false; break;

            case SDL_EVENT_MOUSE_MOTION:
                mouseX = ev.motion.x; mouseY = ev.motion.y;
                mouseInside = true;
                if (dragging) {
                    float dx = ev.motion.x - dragSMX;
                    float dy = ev.motion.y - dragSMY;
                    panX = tPanX = dragSPX + dx;
                    panY = tPanY = dragSPY + dy;
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL: {
                // Scale by the actual (fractional) delta, not one fixed step
                // per event, so a smooth-scrolling trackpad's burst of small
                // events zooms in proportion to the gesture.
                float wheelY = std::clamp(ev.wheel.y, -8.0f, 8.0f);
                float f = std::pow(1.12f, wheelY);
                double nsx = std::clamp(tScaleX * (double)f, 1e-2, 1e9);
                double wx  = ((double)ev.wheel.mouse_x - tPanX) / tScaleX;
                tPanX   = ev.wheel.mouse_x - wx * nsx;
                tScaleX = nsx;
                // Shift held: zoom time only, leave the amplitude scale as-is.
                bool timeOnly = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                if (!timeOnly) {
                    float nsy = std::clamp(tScaleY * f, 1.0f, 1e7f);
                    float wy  = (tPanY - ev.wheel.mouse_y) / tScaleY;
                    tPanY   = ev.wheel.mouse_y + wy * nsy;
                    tScaleY = nsy;
                }
                break;
            }

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                // In pixels (RESIZED is in points). Diff against the last
                // size seen, and record it: a drag resize can deliver
                // several events in one frame, and each diffed against the
                // frame-start size would double-shift.
                panX += (ev.window.data1 - winW) / 2.0; tPanX = panX;
                panY += (ev.window.data2 - winH) / 2.0f; tPanY = panY;
                winW = ev.window.data1; winH = ev.window.data2;
                break;

            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: // e.g. moved to another monitor
                updateUiScale();
                break;

            case SDL_EVENT_DROP_BEGIN:
            case SDL_EVENT_DROP_COMPLETE:
                dropTaken = false;
                break;
            case SDL_EVENT_DROP_FILE:
                if (!dropTaken && ev.drop.data) {
                    dropTaken = true;
                    openFile(ev.drop.data);
                }
                break;
            }
        }

        // Minimized / zero-size window: nothing to draw (and zero-size render
        // textures can't be created). Without vsync pacing a present, idle.
        if (winW <= 0 || winH <= 0) { SDL_Delay(16); continue; }

        {
            // Ease pan/scale toward their targets. dt is capped so a stalled
            // frame eases instead of snapping straight to the target.
            const float dtClamped = std::min(dt, 0.05f);
            const float t = 1.0f - std::pow(0.0001f, dtClamped);
            panX  += (tPanX  - panX)  * t;  panY  += (tPanY  - panY)  * t;
            scaleX += (tScaleX-scaleX) * t; scaleY += (tScaleY-scaleY) * t;
        }

        // Settled = the pan/zoom easing has actually converged (not merely
        // "no input for N ms"); re-rendering the hi-res pass while it's still
        // gliding flickers at the peak edges.
        const bool isSettled = !dragging
            && std::fabs(tPanX - panX) < 0.05f
            && std::fabs(tPanY - panY) < 0.05f
            && std::fabs(tScaleX - scaleX) < std::max(1.0, std::fabs(scaleX)) * 0.0005
            && std::fabs(tScaleY - scaleY) < std::max(1.0f, std::fabs(scaleY)) * 0.0005f;
        const int ssQualityEff     = clampSS(ssQuality);
        const int kSSInteractiveEff = clampSS(kSSInteractive);

        const double worldX = toWX(mouseX, panX, scaleX);

        // ── Background ────────────────────────────────────────────────────────
        const SDL_Color& bg = bgIsWhite ? kBgWhite : kBgDark;
        SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
        SDL_RenderClear(renderer);

        if (!haveAudio) {
            drawEmptyState();
            drawOverlays(now);
            SDL_RenderPresent(renderer);
            continue;
        }

        // ── Background grid ───────────────────────────────────────────────────
        // Inkscape-style: major lines snapped to a "nice" 1/2/5 x10^n step
        // chosen so majors land ~kGridTargetPx apart on screen at the current
        // zoom, each major cell subdivided into 10 evenly spaced minor lines.
        // Toggled via 'G'.
        const float kGridTargetPx = 80.0f * uiScale;
        auto niceStep = [](double targetWorldSpan) -> double {
            if (!(targetWorldSpan > 0.0) || !std::isfinite(targetWorldSpan)) return 1.0;
            double exponent = std::floor(std::log10(targetWorldSpan));
            double fraction = targetWorldSpan / std::pow(10.0, exponent);
            double niceFraction = (fraction < 1.5) ? 1.0 : (fraction < 3.0) ? 2.0 : (fraction < 7.0) ? 5.0 : 10.0;
            return niceFraction * std::pow(10.0, exponent);
        };
        // Walks major cells covering [worldLo, worldHi], calling plot(v,
        // isMajor) for the major itself (k=0) and its 9 minor
        // subdivisions -- avoids float-equality checks for "is this a
        // major line" that a single merged loop would need.
        auto drawAxisLines = [&](double step, double worldLo, double worldHi, auto&& plot) {
            double minor = step / 10.0;
            long m0 = (long)std::floor(worldLo / step) - 1;
            long m1 = (long)std::ceil (worldHi / step) + 1;
            for (long m = m0; m <= m1; ++m) {
                double majorV = (double)m * step;
                for (int k = 0; k < 10; ++k) {
                    double v = majorV + (double)k * minor;
                    if (v < worldLo - minor || v > worldHi + minor) continue;
                    plot(v, k == 0);
                }
            }
        };
        // On dark: a light tint, bright enough that the waveform's darkest
        // rung still reads as black against it. On white: a darker tint
        // at lower alpha, since contrast there is already high.
        auto plotColor = [&](bool major, bool onDark) {
            const SDL_Color gridColor  = onDark ? SDL_Color{115, 115, 150, 255} : SDL_Color{45, 45, 68, 255};
            const int       majorAlpha = onDark ? 120 : 90;
            const int       minorAlpha = onDark ? 45 : 35;
            SDL_SetRenderDrawColor(renderer, gridColor.r, gridColor.g, gridColor.b,
                                    major ? majorAlpha : minorAlpha);
        };

        // Horizontal lines (amplitude axis) across [x0, x1]. Also redrawn over
        // the opaque histogram panel, which would otherwise hide them.
        auto drawHGrid = [&](float x0, float x1, bool onDark) {
            double stepY = niceStep((double)kGridTargetPx / std::max(1e-6, (double)scaleY));
            double worldYLo = ((double)panY - (double)winH) / (double)scaleY;
            double worldYHi = (double)panY / (double)scaleY;
            drawAxisLines(stepY, worldYLo, worldYHi, [&](double v, bool major) {
                float sy = panY - (float)v * scaleY;
                plotColor(major, onDark);
                SDL_RenderLine(renderer, x0, sy, x1, sy);
            });
        };

        if (showGrid) {
            // Vertical lines: time axis.
            double stepX = niceStep((double)kGridTargetPx / std::max(1e-6, std::fabs(scaleX)));
            double worldXLo = toWX(0.0, panX, scaleX);
            double worldXHi = toWX((double)winW, panX, scaleX);
            if (worldXLo > worldXHi) std::swap(worldXLo, worldXHi);
            drawAxisLines(stepX, worldXLo, worldXHi, [&](double v, bool major) {
                float sx = (float)toSX(v, panX, scaleX);
                plotColor(major, !bgIsWhite);
                SDL_RenderLine(renderer, sx, 0.0f, sx, (float)winH);
            });

            drawHGrid(0.0f, (float)winW, !bgIsWhite);
        }

        // Zero line: native resolution, drawn every frame.
        SDL_SetRenderDrawColor(renderer, 90, 90, 130, 255);
        SDL_RenderLine(renderer, 0.0f, panY, (float)winW, panY);

        // Waveform: adaptive-quality supersampled pass (see the comment at
        // ssQuality above).
        auto renderPass = [&](SDL_Texture* tex, int ss) {
            SDL_SetRenderTarget(renderer, tex);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            drawWaveform(renderer, audio.mono, audio.pyramid, audio.sampleRate, panX, scaleX, panY, scaleY,
                         winW, kLineColor, ss, shading, bgIsWhite);
            SDL_SetRenderTarget(renderer, nullptr);
        };

        ensureTex(waveTexLo, waveTexLoW, waveTexLoH, kSSInteractiveEff);
        bool hiRecreated = ensureTex(waveTexHi, waveTexHiW, waveTexHiH, ssQualityEff);
        bool hiFinalRecreated = ensureTex(waveTexHiFinal, waveTexHiFinalW, waveTexHiFinalH, 1);

        if (!isSettled) {
            hiResValid = false; // stale the moment the view starts moving again
            if (waveTexLo) {
                renderPass(waveTexLo, kSSInteractiveEff);
                SDL_RenderTexture(renderer, waveTexLo, nullptr, nullptr);
            }
        } else {
            bool viewChanged =
                   winW         != cachedWinW || winH != cachedWinH
                || ssQualityEff != cachedSSQuality
                || shading.level  != cachedLevel
                || bgIsWhite    != cachedBgIsWhite
                || std::fabs(panX - cachedPanX) > 0.02f
                || std::fabs(panY - cachedPanY) > 0.02f
                || std::fabs(scaleX - cachedScaleX) > std::max(1.0, std::fabs(cachedScaleX)) * 0.0002
                || std::fabs(scaleY - cachedScaleY) > std::max(1.0f, std::fabs(cachedScaleY)) * 0.0002f;

            if (waveTexHi && waveTexHiFinal
                && (hiRecreated || hiFinalRecreated || !hiResValid || viewChanged)) {
                renderPass(waveTexHi, ssQualityEff);
                downsampleMipChain(waveTexHi, waveTexHiW, waveTexHiH,
                                    waveTexHiFinal, waveTexHiFinalW, waveTexHiFinalH);
                cachedWinW = winW; cachedWinH = winH; cachedSSQuality = ssQualityEff;
                cachedLevel = shading.level;
                cachedBgIsWhite = bgIsWhite;
                cachedPanX = panX; cachedPanY = panY; cachedScaleX = scaleX; cachedScaleY = scaleY;
                hiResValid = true;
            }
            if (waveTexHiFinal) SDL_RenderTexture(renderer, waveTexHiFinal, nullptr, nullptr);
        }

        // ── Cursor amplitude readout ────────────────────────────────────────
        // The amplitude under the mouse, in cyan in the top-right corner (the
        // matching cyan line is drawn after the histogram panel, over it).
        {
            const float worldY = (panY - mouseY) / scaleY;

            const float kAmpScale = textScale(2.0f);
            const float pad = 6.0f * uiScale;
            const float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * kAmpScale;
            std::string ampLabel = std::format("{:+.4f}", worldY);
            float lw = (float)ampLabel.size() * ch;
            float bx = (float)winW - lw - pad * 2.0f - pad;
            float by = pad;

            SDL_SetRenderDrawColor(renderer, 15, 15, 30, 210);
            SDL_FRect bg{bx, by, lw + pad * 2.0f, ch + pad * 2.0f};
            SDL_RenderFillRect(renderer, &bg);
            SDL_SetRenderDrawColor(renderer, kCursorYColor.r, kCursorYColor.g, kCursorYColor.b, 200);
            SDL_RenderRect(renderer, &bg);

            SDL_SetRenderDrawColor(renderer, kCursorYColor.r, kCursorYColor.g, kCursorYColor.b, kCursorYColor.a);
            drawText(bx + pad, by + pad, kAmpScale, ampLabel);
        }

        // ── Cursor time readout ─────────────────────────────────────────────────
        // A vertical line follows the mouse; the time under it (large) and its
        // sample index (smaller, dimmer, bottom-aligned) sit in one box
        // centered along the bottom. The index is padded to the file's widest
        // index so the box doesn't change width as the digit count changes.
        {
            const float kTimeScale = textScale(4.0f), kSampleScale = textScale(2.0f);
            const float pad = 8.0f * uiScale, gap = 18.0f * uiScale;
            const float chT = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * kTimeScale;
            const float chS = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * kSampleScale;

            SDL_SetRenderDrawColor(renderer, 90, 90, 120, 130);
            SDL_RenderLine(renderer, mouseX, 0.0f, mouseX, (float)winH);

            const std::string timeStr = fmtTime(worldX);
            // Clamped before rounding so a cursor far off the recording can't overflow `long`.
            const double samplePos = std::clamp(worldX * audio.sampleRate, -1e15, 1e15);
            const long sampleIdx = std::lround(samplePos);
            const int idxWidth = (int)std::to_string((long)audio.mono.size()).size() + 1; // +1 for a '-' left of the start
            const std::string sampleStr = std::format("#{:>{}}", sampleIdx, idxWidth);

            const float tw = (float)timeStr.size() * chT;
            const float sw = (float)sampleStr.size() * chS;
            const float boxW = pad + tw + gap + sw + pad;
            const float boxH = pad + chT + pad;
            const float bx = std::round(((float)winW - boxW) * 0.5f); // centered, independent of mouseX
            const float by = (float)winH - boxH - 10.0f * uiScale;

            SDL_SetRenderDrawColor(renderer, 15, 15, 30, 210);
            SDL_FRect bg{bx, by, boxW, boxH};
            SDL_RenderFillRect(renderer, &bg);
            SDL_SetRenderDrawColor(renderer, 80, 80, 120, 200);
            SDL_RenderRect(renderer, &bg);

            SDL_SetRenderDrawColor(renderer, kTextColor.r, kTextColor.g, kTextColor.b, kTextColor.a);
            drawText(bx + pad, by + pad, kTimeScale, timeStr);

            SDL_SetRenderDrawColor(renderer, kTextColor.r, kTextColor.g, kTextColor.b, 150);
            const float sy = by + pad + chT - chS; // bottom edges line up with the time text
            drawText(bx + pad + tw + gap, sy, kSampleScale, sampleStr);
        }

        // ── Hover readout ─────────────────────────────────────────────────────
        // Shown whenever the cursor is over audio -- no open/close animation,
        // and it keeps updating live during drags and zoom easing.
        bool hoverActive = false;
        if (showHover && mouseInside && !showHelp) {
            const double halfPx = (double)(kHoverHalfPx * uiScale);
            const double tL = toWX((double)mouseX - halfPx, panX, scaleX);
            const double tR = toWX((double)mouseX + halfPx, panX, scaleX);
            long i0, i1;
            if (hoverSampleRange(tL, tR, audio.sampleRate, (long)audio.mono.size(), kHoverMinSamples, i0, i1)) {
                const bool rangeChanged = (i0 != hoverI0 || i1 != hoverI1);
                if (rangeChanged) {
                    hover = computeHoverStats(audio.mono, audio.pyramid, i0, i1);
                    hoverI0 = i0; hoverI1 = i1;
                }
                if (hover.valid) {
                    const HistLayout layout = histLayout(hover.peak, panY, scaleY, winH);
                    if (rangeChanged || !sameLayout(layout, hoverLayout, scaleY)) {
                        hoverHist = computeHistogram(audio.mono, i0, i1, layout.lo, layout.hi,
                                                     layout.nBins, kHoverMaxBinned);
                        hoverLayout = layout;
                    }
                    hoverActive = true;
                }
            }
        }

        if (hoverActive) {
            // Shade the exact span being measured (it widens visibly when
            // zoomed in past kHoverMinSamples).
            float bx0 = (float)toSX((double)hover.i0 / audio.sampleRate, panX, scaleX);
            float bx1 = (float)toSX((double)(hover.i1 + 1) / audio.sampleRate, panX, scaleX);
            SDL_FRect band{bx0, 0.0f, std::max(1.0f, bx1 - bx0), (float)winH};
            SDL_SetRenderDrawColor(renderer, 120, 140, 210, 38);
            SDL_RenderFillRect(renderer, &band);
        }
        if (hoverActive && !hoverHist.bins.empty()) {
            const float panelW = std::round(kHistPanelW * uiScale);
            drawHistogramPanel(renderer, hover, hoverHist, panelW, panY, scaleY, winH, hoverLogScale, shading);
            if (showGrid) drawHGrid(0.0f, panelW - 1.0f, /*onDark=*/true);
        }

        // Cyan amplitude crosshair: last, so it crosses the histogram panel too.
        SDL_SetRenderDrawColor(renderer, kCursorYColor.r, kCursorYColor.g, kCursorYColor.b, 150);
        SDL_RenderLine(renderer, 0.0f, mouseY, (float)winW, mouseY);

        drawOverlays(now);

        SDL_RenderPresent(renderer);
    }

    if (waveTexLo) SDL_DestroyTexture(waveTexLo);
    if (waveTexHi) SDL_DestroyTexture(waveTexHi);
    if (waveTexHiFinal) SDL_DestroyTexture(waveTexHiFinal);
    for (auto* t : mipChainTemps) SDL_DestroyTexture(t);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
