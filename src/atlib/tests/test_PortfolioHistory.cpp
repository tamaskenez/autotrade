#include <atlib/papertrading/PortfolioHistory.h>

#include <gtest/gtest.h>

// worst_12_month_return() walks a start day forward and, for each, advances a
// second index to the first trading day a full year later. Both tests below are
// about *which* day the return is measured to, because that is the one thing the
// answer cannot be inspected for: every window produces a plausible-looking
// percentage, and a window read off the wrong end is a return the portfolio never
// had rather than an obviously broken number.

namespace
{

chr::local_days day(int y, unsigned m, unsigned d)
{
    return chr::local_days(chr::year_month_day(chr::year(y), chr::month(m), chr::day(d)));
}

PortfolioHistory history(const vector<pair<chr::local_days, double>>& totals)
{
    PortfolioHistory ph;
    for (const auto& [date, total] : totals) {
        ph.trading_days.push_back(PortfolioHistory::TradingDay{.date = date, .total = total});
    }
    return ph;
}

// Four windows, one of them a loss, with every window's endpoints half a year
// apart so that being one trading day off is not a rounding difference but a
// different pair of totals entirely.
//
// Only 2020-07-01 -> 2021-07-01 loses money: 100 -> 90, or -10% over the year.
// Measuring to the end of the *previous* start's window instead reads 2021-01-01
// against 2021-07-01, annualises 90/120 over half a year, and reports -44%.
TEST(PortfolioHistory, WorstWindowIsMeasuredToTheEndOfItsOwnWindow)
{
    const auto ph = history({
      {day(2020, 1, 1), 100.0},
      {day(2020, 7, 1), 100.0},
      {day(2021, 1, 1), 120.0},
      {day(2021, 7, 1), 90.0 },
      {day(2022, 1, 1), 150.0},
    });

    const auto worst = ph.worst_12_month_return();
    ASSERT_TRUE(worst.has_value());
    EXPECT_NEAR(*worst, -0.1, 1e-3);
}

// A portfolio that gains 0.05% every day has no losing window at all, so any
// non-positive answer is a window that was never measured. Zero is the specific
// value worth naming: it is what a window of no length returns, since pow(1.0,
// 1/0) is 1 -- so a first window that ends where it starts caps the worst return
// at 0% for every portfolio, and says so in a unit no reader would question.
TEST(PortfolioHistory, RisingPortfolioHasNoFlatWindow)
{
    vector<pair<chr::local_days, double>> totals;
    double total = 100.0;
    for (auto d = day(2020, 1, 1); d < day(2023, 1, 1); ++d) {
        totals.emplace_back(d, total);
        total *= 1.0005;
    }

    const auto worst = history(totals).worst_12_month_return();
    ASSERT_TRUE(worst.has_value());
    EXPECT_GT(*worst, 0.0);
    // Annualising divides out the window's length, so every window compounds the
    // daily rate over the same 365.2425-day year whichever days it spans.
    EXPECT_NEAR(*worst, pow(1.0005, 365.2425) - 1, 1e-3);
}

} // namespace
