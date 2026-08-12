#include <atlib/marketdata/total_return.h>

#include <gtest/gtest.h>

using namespace std::literals;

// The cash leg's arithmetic, tested on hand-built histories rather than through
// MarketData: the conversion is the part that can be silently wrong, and it needs
// no file, no cache and no guard to exercise.

namespace
{

chr::local_days day(int y, unsigned m, unsigned d)
{
    return chr::local_days(chr::year_month_day(chr::year(y), chr::month(m), chr::day(d)));
}

// One observation per calendar day over [first, last], all at `value`.
RateHistory flat(double value, chr::local_days first, chr::local_days last)
{
    RateHistory history{.symbol = "TESTRATE", .convention = RateConvention::discount_360, .observations = {}};
    for (auto d = first; d <= last; ++d) {
        history.observations.push_back({.date = d, .value = value});
    }
    return history;
}

double must(const expected<double, string>& r)
{
    EXPECT_TRUE(r.has_value()) << (r ? string{} : r.error());
    return r.value_or(std::numeric_limits<double>::quiet_NaN());
}

} // namespace

// The headline: a discount quote is not a return, and reading it as one loses
// money that the hurdle is supposed to have. If this test ever passes with the
// naive reading, the conversion has been removed.
TEST(CashReturnFactor, DiscountQuoteIsNotReadAsAReturn)
{
    const auto from = day(2024, 1, 1);
    const auto to = day(2024, 12, 31); // 365 days
    const auto history = flat(5.0, from, to);

    const double got = must(cash_return_factor(history, from, to));

    // Accruing the quote itself, act/360 -- what you get by forgetting the basis.
    const double naive = std::pow(1.0 + 0.05 / 360.0, 365.0);

    EXPECT_GT(got, naive) << "the discount basis was not undone";
    EXPECT_NEAR(got - 1.0, 0.052680, 1e-6) << "5% discount -> 5.268% of realised growth";
    EXPECT_NEAR((got - naive) * 1e4, 6.83, 0.05) << "about 7bp a year at a 5% level";
}

TEST(CashReturnFactor, ZeroRateIsFlatAndNegativeRateLosesMoney)
{
    const auto from = day(2024, 1, 1);
    const auto to = day(2024, 6, 30);

    EXPECT_DOUBLE_EQ(must(cash_return_factor(flat(0.0, from, to), from, to)), 1.0);
    EXPECT_LT(must(cash_return_factor(flat(-0.05, from, to), from, to)), 1.0) << "rates have gone negative before";
}

// Half-open at `from`, like the equity factor, so windows chain by multiplication
// and the two legs can be compared over identical windows.
TEST(CashReturnFactor, AdjacentWindowsChain)
{
    const auto from = day(2024, 1, 1);
    const auto mid = day(2024, 5, 17);
    const auto to = day(2024, 12, 31);
    const auto history = flat(4.25, from, to);

    const double whole = must(cash_return_factor(history, from, to));
    const double parts = must(cash_return_factor(history, from, mid)) * must(cash_return_factor(history, mid, to));

    EXPECT_NEAR(whole, parts, 1e-12);
}

// Weekends and bank holidays are absent from the source and still accrue: the gaps
// are not a calendar, they are days FRED did not publish.
TEST(CashReturnFactor, GapsAreForwardFilledAndStillAccrue)
{
    RateHistory sparse{.symbol = "TESTRATE", .convention = RateConvention::discount_360, .observations = {}};
    sparse.observations.push_back({.date = day(2024, 1, 1), .value = 5.0});
    // Nothing until the 11th; the 5% must carry across the gap.
    sparse.observations.push_back({.date = day(2024, 1, 11), .value = 5.0});

    const auto dense = flat(5.0, day(2024, 1, 1), day(2024, 1, 11));

    EXPECT_DOUBLE_EQ(
      must(cash_return_factor(sparse, day(2024, 1, 1), day(2024, 1, 11))),
      must(cash_return_factor(dense, day(2024, 1, 1), day(2024, 1, 11)))
    );
}

// A change takes effect on the day it is published, not the day after.
TEST(CashReturnFactor, RateChangeAppliesFromItsOwnDate)
{
    RateHistory history{.symbol = "TESTRATE", .convention = RateConvention::discount_360, .observations = {}};
    history.observations.push_back({.date = day(2024, 1, 1), .value = 0.0});
    history.observations.push_back({.date = day(2024, 1, 2), .value = 5.0});

    // The single day 01-01 -> 01-02 accrues at the 01-02 value, not the 01-01 one.
    const double got = must(cash_return_factor(history, day(2024, 1, 1), day(2024, 1, 2)));
    EXPECT_GT(got, 1.0);
    EXPECT_NEAR(got, 1.000140666760, 1e-12);
}

TEST(CashReturnFactor, WindowMustRunForwards)
{
    const auto history = flat(5.0, day(2024, 1, 1), day(2024, 12, 31));

    EXPECT_FALSE(cash_return_factor(history, day(2024, 6, 1), day(2024, 1, 1)));
    EXPECT_FALSE(cash_return_factor(history, day(2024, 6, 1), day(2024, 6, 1)));
}

// There is no rate in force to carry forward, and inventing one would fabricate
// the hurdle rather than measure it.
TEST(CashReturnFactor, StartBeforeTheFirstObservationIsAnError)
{
    const auto history = flat(5.0, day(2024, 1, 1), day(2024, 12, 31));

    const auto got = cash_return_factor(history, day(2023, 12, 1), day(2024, 6, 1));
    ASSERT_FALSE(got.has_value());
    EXPECT_NE(got.error().find("2024-01-01"), string::npos) << got.error();
}

TEST(CashReturnFactor, EmptyHistoryIsAnError)
{
    const RateHistory history{.symbol = "TESTRATE", .convention = RateConvention::discount_360, .observations = {}};

    EXPECT_FALSE(cash_return_factor(history, day(2024, 1, 1), day(2024, 6, 1)));
}
