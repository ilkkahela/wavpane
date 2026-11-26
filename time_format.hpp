#pragma once

// Time formatting for the cursor readout in wav_main.cpp, kept in a header so
// it can be unit-tested (tests/time_format_test.cpp).

#include <cmath>
#include <format>
#include <string>

// "MM:SS.mmm" (minutes keep growing past 99). Rounds to whole milliseconds
// *before* splitting into minutes and seconds -- splitting first and letting
// printf round the seconds turns 59.9996 into "00:60.000".
inline std::string fmtTime(double seconds) {
    if (!std::isfinite(seconds)) return "--:--.---";
    // Clamp so llround stays well inside long long (~292 million years).
    const double absSec = std::fmin(std::fabs(seconds), 1e12);
    const long long ms = std::llround(absSec * 1000.0);
    const bool neg = seconds < 0.0 && ms > 0; // no "-00:00.000"
    const long long mins = ms / 60000;
    const long long rem  = ms % 60000;
    return std::format("{}{:02}:{:02}.{:03}", neg ? "-" : "", mins, rem / 1000, rem % 1000);
}
