#pragma once

// Hierarchical min/max + power-sum pyramid over a sample buffer, so
// drawWaveform's zoomed-out envelope path (waveform_render.hpp) can answer an
// arbitrary [i0, i1] range query in O(log range) bin merges instead of an
// O(range) raw-sample scan. How it works:
//
//   - BinStat is a commutative monoid (min/max + plain sums are associative),
//     so bins merge cleanly both bottom-up (build) and in arbitrary
//     combination (query).
//   - Bin j at level k covers *exactly* raw range
//     [j*2^(k+1), min((j+1)*2^(k+1)-1, N-1)] by construction, with no
//     padding needed for non-power-of-two N -- the ceil-based pairing in
//     buildPyramid() propagates any leftover odd sample up as a
//     partial-count bin, and the ranges tile with no gaps or overlaps at
//     every level.
//   - queryPyramid() greedily consumes the largest aligned block that fits
//     at each step (the same "staircase" decomposition a Fenwick tree uses
//     for range sums), which is exact -- not an approximation -- because of
//     that tiling invariant.
//
// Sums are kept in float, not double: bins are combined by pairwise/tree
// merging (leaf to root), which is cascade summation and has an
// O(log n * epsilon) relative error bound rather than naive linear
// summation's O(n * epsilon) -- for N in the millions that's still ~6
// significant decimal digits, well beyond what a screen pixel (or the
// source 16-bit PCM) needs, and it halves the pyramid's memory footprint.

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct BinStat {
    float mn = 0.0f, mx = 0.0f;   // true min/max over the bin
    float sumSq = 0.0f;           // sum of v^2
    float sumCube = 0.0f;         // sum of |v|^3
    float sumQuad = 0.0f;         // sum of v^4
    uint32_t count = 0;           // samples covered; 0 = empty/identity
};

inline BinStat mergeBins(const BinStat& a, const BinStat& b) {
    if (a.count == 0) return b;
    if (b.count == 0) return a;
    return BinStat{
        std::min(a.mn, b.mn), std::max(a.mx, b.mx),
        a.sumSq + b.sumSq, a.sumCube + b.sumCube, a.sumQuad + b.sumQuad,
        a.count + b.count
    };
}

// Aggregates raw samples ys[i0..i1] (inclusive) into a single leaf bin. Used
// both for level-0 construction (1-2 samples) and for the single-raw-sample
// fallback in queryPyramid (no level stores bin-size-1, so m==0 steps read
// straight from the buffer).
inline BinStat makeLeaf(const std::vector<float>& ys, long i0, long i1) {
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

struct WaveformPyramid {
    // levels[k] holds bins of size 2^(k+1) raw samples; levels[0][j] covers
    // raw samples [2j, 2j+1] (clipped at the buffer end). Built until a
    // level collapses to a single bin covering the whole buffer.
    std::vector<std::vector<BinStat>> levels;
    long n = 0;
};

// O(N) total across all levels (N + N/2 + N/4 + ... ), run once when the WAV
// loads -- not per frame.
inline WaveformPyramid buildPyramid(const std::vector<float>& ys) {
    WaveformPyramid pyr;
    pyr.n = (long)ys.size();
    if (pyr.n == 0) return pyr;

    std::vector<BinStat> level0((size_t)(pyr.n + 1) / 2);
    for (size_t j = 0; j < level0.size(); ++j) {
        long i0 = (long)j * 2, i1 = std::min(i0 + 1, pyr.n - 1);
        level0[j] = makeLeaf(ys, i0, i1);
    }
    pyr.levels.push_back(std::move(level0));

    while (pyr.levels.back().size() > 1) {
        const auto& prev = pyr.levels.back();
        std::vector<BinStat> next((prev.size() + 1) / 2);
        for (size_t j = 0; j < next.size(); ++j) {
            size_t a = j * 2, b = a + 1;
            next[j] = (b < prev.size()) ? mergeBins(prev[a], prev[b]) : prev[a];
        }
        pyr.levels.push_back(std::move(next));
    }
    return pyr;
}

// Exact aggregation over raw samples [i0, i1] (inclusive), 0 <= i0 <= i1 <
// pyr.n. `ys` must be the same buffer the pyramid was built from -- it's only
// touched for the m==0 (single raw sample) steps, since no level stores
// bin-size-1 bins.
inline BinStat queryPyramid(const WaveformPyramid& pyr, const std::vector<float>& ys,
                             long i0, long i1) {
    BinStat acc{};
    long i = i0;
    while (i <= i1) {
        unsigned long long remaining = (unsigned long long)(i1 - i + 1);
        // std::countr_zero(0) is well-defined (returns the type's bit width,
        // 64 here) rather than UB, so i==0 needs no special case: it just
        // means "no alignment constraint yet", correctly deferring to kFit.
        int kAlign = std::countr_zero((unsigned long long)i);
        int kFit = 63 - std::countl_zero(remaining);
        int m = std::min({kAlign, kFit, (int)pyr.levels.size()});

        BinStat block = (m == 0) ? makeLeaf(ys, i, i) : pyr.levels[m - 1][(size_t)(i >> m)];
        acc = mergeBins(acc, block);
        i += (1L << m);
    }
    return acc;
}
