#include <atlib/marketdata/MarketData.h>

#include <gtest/gtest.h>

#include <unistd.h>

using namespace std::literals;

// Hermetic by construction: every test writes its own payload into a temporary
// workspace and never reads `_md/raw`.
//
// Not merely hygiene. The real cache is gitignored vendor data, so a test reading
// it does not run on a fresh clone -- and a miss would not fail, it would
// *download*, which needs a credential and the network and would make the suite's
// result depend on both. A synthetic payload also lets a closure be placed
// wherever a test wants one, which the real calendar does not.
//
// What that leaves uncovered is stated at the bottom of this file.

namespace
{

// A calendar with the three shapes that matter, and nothing else:
//
//   2024-01-01  absent   New Year's Day, before the first bar
//   2024-01-02  bar      first bar -- inception
//   2024-01-03  bar
//   2024-01-04  bar
//   2024-01-05  bar
//   2024-01-06  absent   Saturday
//   2024-01-07  absent   Sunday
//   2024-01-08  bar
//   2024-01-09  absent   a mid-week closure, the Hurricane Sandy shape
//   2024-01-10  bar
//   2024-01-11  bar
//   2024-01-12  bar      last bar
constexpr string_view k_symbol = "TEST";

constexpr std::array k_dates{
  "2024-01-02"sv,
  "2024-01-03"sv,
  "2024-01-04"sv,
  "2024-01-05"sv,
  "2024-01-08"sv,
  "2024-01-10"sv,
  "2024-01-11"sv,
  "2024-01-12"sv,
};

chr::local_days day(int y, unsigned m, unsigned d)
{
    return chr::local_days(chr::year_month_day(chr::year(y), chr::month(m), chr::day(d)));
}

// A Tiingo response carrying `dates` and nothing of interest besides. The prices
// are all the same because no test here looks at one: this exercises which days
// exist, not what they were worth.
string tiingo_payload(span<const string_view> dates)
{
    string rows;
    for (const auto date : dates) {
        if (!rows.empty()) {
            rows += ',';
        }
        rows += format(
          R"({{"date":"{}T00:00:00.000Z","open":10.0,"high":10.5,"low":9.5,)"
          R"("close":10.0,"volume":1000.0,"divCash":0.0,"splitFactor":1.0}})",
          date
        );
    }
    return format("[{}]", rows);
}

class MarketDataTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* const test = ::testing::UnitTest::GetInstance()->current_test_info();
        config.workspace_dir =
          fs::temp_directory_path() / format("atlib_{}_{}_{}", test->test_suite_name(), test->name(), getpid());

        std::error_code ec;
        fs::remove_all(config.workspace_dir, ec);

        write_payload(k_symbol, tiingo_payload(k_dates));
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(config.workspace_dir, ec);
    }

    void write_payload(string_view symbol, string_view payload) const
    {
        ASSERT_TRUE(write_cache(cache_path(config, Provider::tiingo, symbol), payload).has_value());
    }

    fs::path payload_path(string_view symbol) const
    {
        return cache_path(config, Provider::tiingo, symbol);
    }

    MarketData market_data() const
    {
        return MarketData(config, Provider::tiingo);
    }

    // Fails the test rather than returning, so a test body reads as a statement
    // about the calendar and not as error plumbing.
    static DailyBarAvailability availability(MarketData& md, chr::local_days d)
    {
        const auto got = md.daily_bar_availability(k_symbol, d);
        EXPECT_TRUE(got.has_value()) << (got ? string{} : got.error());
        // A value no test expects, so a swallowed error cannot pass for one.
        return got.value_or(DailyBarAvailability::after_last_bar);
    }

    MarketDataConfig config;
};

using enum DailyBarAvailability;

// ---------------------------------------------------------------- the calendar

TEST_F(MarketDataTest, TradingDayIsAvailable)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 12));

    EXPECT_EQ(availability(md, day(2024, 1, 3)), available);
    EXPECT_EQ(availability(md, day(2024, 1, 8)), available);
    EXPECT_EQ(availability(md, day(2024, 1, 2)), available) << "the first bar itself";
    EXPECT_EQ(availability(md, day(2024, 1, 12)), available) << "the last bar itself";
}

TEST_F(MarketDataTest, WeekendIsNotTraded)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 12));

    EXPECT_EQ(availability(md, day(2024, 1, 6)), not_traded);
    EXPECT_EQ(availability(md, day(2024, 1, 7)), not_traded);
}

// The Hurricane Sandy / 9-11 shape: a weekday inside the history with no bar.
// Nothing but the absence of the row says the exchange was shut, which is the
// point -- there is no holiday table to consult or to get wrong.
TEST_F(MarketDataTest, MidWeekClosureIsNotTraded)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 12));

    EXPECT_EQ(availability(md, day(2024, 1, 9)), not_traded);
    EXPECT_EQ(availability(md, day(2024, 1, 8)), available);
    EXPECT_EQ(availability(md, day(2024, 1, 10)), available);
}

TEST_F(MarketDataTest, BeforeInceptionIsBeforeFirstBar)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 12));

    EXPECT_EQ(availability(md, day(2023, 12, 29)), before_first_bar);
    EXPECT_EQ(availability(md, day(2024, 1, 1)), before_first_bar) << "a holiday, but also before inception";
}

// --------------------------------------------------------------- the as_of guard

// The boundary is inclusive: a backtest standing at the close of D is entitled to
// D's bar. Off by one here and every decision reads a day late.
TEST_F(MarketDataTest, AsOfIsInclusive)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 8));

    EXPECT_EQ(availability(md, day(2024, 1, 8)), available);
}

// The guard moves where the history *ends*, not just what can be read out of it.
//
// Both halves ask about the same date and get different answers, and the second
// one is the honest answer: standing on 2024-01-09, nothing has yet shown that a
// bar ever follows it, so calling it a closure would be reading Monday's row a
// caller is not entitled to. This is the test that distinguishes a correct
// visible() from one that ignores the guard.
TEST_F(MarketDataTest, GuardMovesWhereTheHistoryEnds)
{
    MarketData md = market_data();

    md.set_as_of(day(2024, 1, 12));
    ASSERT_EQ(availability(md, day(2024, 1, 9)), not_traded) << "with the whole month visible, a known closure";

    md.set_as_of(day(2024, 1, 9));
    EXPECT_EQ(availability(md, day(2024, 1, 9)), after_last_bar) << "standing on it, the history simply stops here";
}

// The same thing at a weekend, where the gap is two days rather than one.
TEST_F(MarketDataTest, GuardInsideAWeekendReportsAfterLastBar)
{
    MarketData md = market_data();

    md.set_as_of(day(2024, 1, 7));

    EXPECT_EQ(availability(md, day(2024, 1, 6)), after_last_bar);
    EXPECT_EQ(availability(md, day(2024, 1, 5)), available) << "the last bar the guard admits";
}

// Guard before everything: the visible range is empty, and every date the guard
// allows is necessarily before the first bar there is.
TEST_F(MarketDataTest, GuardBeforeInceptionSeesNoHistory)
{
    MarketData md = market_data();

    md.set_as_of(day(2024, 1, 1));

    EXPECT_EQ(availability(md, day(2024, 1, 1)), before_first_bar);
    EXPECT_EQ(availability(md, day(2023, 12, 29)), before_first_bar);
}

TEST_F(MarketDataTest, GuardMovesInBothDirections)
{
    MarketData md = market_data();

    md.set_as_of(day(2024, 1, 4));
    EXPECT_EQ(availability(md, day(2024, 1, 4)), available);

    md.set_as_of(day(2024, 1, 12));
    EXPECT_EQ(availability(md, day(2024, 1, 11)), available);

    md.set_as_of(day(2024, 1, 4));
    EXPECT_EQ(availability(md, day(2024, 1, 2)), available) << "history survived the guard moving backwards";
}

TEST_F(MarketDataTest, ClearedGuardExposesEverything)
{
    MarketData md = market_data();

    md.set_as_of(day(2024, 1, 4));
    md.clear_as_of();

    EXPECT_EQ(availability(md, day(2024, 1, 12)), available);
}

// The guard is not a filter that answers "no": asking past it is a caller bug and
// terminates. This is the property the whole class exists for, so it is worth the
// cost of a death test.
using MarketDataDeathTest = MarketDataTest;

TEST_F(MarketDataDeathTest, QueryPastAsOfTerminates)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 8));

    EXPECT_DEATH((void)md.daily_bar_availability(k_symbol, day(2024, 1, 10)), "");
}

// ------------------------------------------------------------------- the cache

// The payload is deleted after the first query, so a second query that still
// answers can only be answering from memory. That is the whole point of the
// class: a backtest steps the guard over hundreds of months and must not re-read
// and re-parse the file each time.
TEST_F(MarketDataTest, PayloadIsParsedOncePerRun)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 12));

    ASSERT_EQ(availability(md, day(2024, 1, 3)), available);

    ASSERT_TRUE(fs::remove(payload_path(k_symbol)));

    EXPECT_EQ(availability(md, day(2024, 1, 4)), available);
    EXPECT_EQ(availability(md, day(2024, 1, 9)), not_traded);
}

// ------------------------------------------------------------------- rejections

TEST_F(MarketDataTest, UnusableSymbolIsRejected)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 12));

    // Rejected before anything is opened: as a path component this would climb
    // out of the workspace.
    EXPECT_FALSE(md.daily_bar_availability("../etc", day(2024, 1, 3)).has_value());
    EXPECT_FALSE(md.daily_bar_availability("", day(2024, 1, 3)).has_value());
}

// Reported, not silently replaced by a download. A cached payload arrived by a
// rename from a complete temporary, so a corrupt one did not get that way on its
// own, and overwriting it would erase the evidence of whatever did.
TEST_F(MarketDataTest, UnparseablePayloadIsReported)
{
    write_payload("BROKEN", "{not json");

    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 12));

    const auto got = md.daily_bar_availability("BROKEN", day(2024, 1, 3));
    ASSERT_FALSE(got.has_value());
    EXPECT_NE(got.error().find("BROKEN"), string::npos) << got.error();
}

} // namespace

// Deliberately not covered here, because covering it would mean a network call:
// every path that downloads. That is the symbol with no cached payload, the day
// past the end of one, and the failed-download error being remembered rather than
// retried per query. Reaching them needs a seam for the fetch that does not exist
// yet -- and inventing one to reach them would put an injection point in
// production code for the tests' benefit, which is the trade the rest of this
// library has so far declined to make.
//
// A consequence worth naming: `after_last_bar` is only reached above with a guard
// set, because without one the last bar of the payload is the last bar there is
// and going past it downloads. So the tests pin down what the guard does to the
// end of the history, and say nothing about what a live caller sees when it runs
// off the end of a real payload.
