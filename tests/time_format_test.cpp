#include <gtest/gtest.h>

#include "../time_format.hpp"

#include <cmath>

// Regression: seconds used to be split off before rounding, so the readout
// showed 60 seconds instead of rolling over to the next minute.
TEST(FmtTime, RoundsIntoTheNextMinuteInsteadOfShowingSixtySeconds) {
    EXPECT_EQ(fmtTime(59.9996), "01:00.000");
    EXPECT_EQ(fmtTime(119.9999), "02:00.000");
    EXPECT_EQ(fmtTime(59.9994), "00:59.999");
}

TEST(FmtTime, FormatsOrdinaryNegativeAndDegenerateTimes) {
    EXPECT_EQ(fmtTime(0.0), "00:00.000");
    EXPECT_EQ(fmtTime(12.3456), "00:12.346");
    EXPECT_EQ(fmtTime(754.5), "12:34.500");
    EXPECT_EQ(fmtTime(6000.0), "100:00.000");      // minutes just keep growing
    EXPECT_EQ(fmtTime(-1.5), "-00:01.500");        // cursor left of the recording
    EXPECT_EQ(fmtTime(-0.0004), "00:00.000");      // rounds to zero: no "-00:00.000"
    EXPECT_EQ(fmtTime(std::nan("")), "--:--.---");
    EXPECT_FALSE(fmtTime(1e300).empty());          // clamped, not UB
}
