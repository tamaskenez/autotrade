#include <atlib/marketdata/MarketData.h>

#include <gtest/gtest.h>

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

// A payload with prices that move and corporate actions that land, which the flat
// one above deliberately does not have: against constant closes and no events, a
// return factor of 1.0 is indistinguishable from a function that returns 1.0.
struct Row {
    string_view date;
    double close;
    double div_cash;
    double split_factor;
};

// Chosen so the factors are exact and checkable by hand:
//
//   01-02 -> 01-03   110/100                     = 1.10
//   01-02 -> 01-04   121/100                     = 1.21
//   01-04 -> 01-08   (126 + 5)/121 = 131/121            <- dividend on the ex-date
//   01-08 -> 01-09   (66 * 2)/126  = 132/126            <- split, and the 01-08
//                                                          dividend excluded by the
//                                                          half-open boundary
//   01-02 -> 01-09   131/100 * 132/126           = 1.372380952...
constexpr string_view k_tr_symbol = "TR";

constexpr std::array k_tr_rows{
  Row{"2024-01-02"sv, 100.0, 0.0, 1.0},
  Row{"2024-01-03"sv, 110.0, 0.0, 1.0},
  Row{"2024-01-04"sv, 121.0, 0.0, 1.0},
  // 01-05 to 01-07 absent: the walk must carry an event over a gap.
  Row{"2024-01-08"sv, 126.0, 5.0, 1.0},
  Row{"2024-01-09"sv, 66.0,  0.0, 2.0},
};

string tiingo_payload(span<const Row> rows)
{
    string out;
    for (const auto& r : rows) {
        if (!out.empty()) {
            out += ',';
        }
        out += format(
          R"({{"date":"{}T00:00:00.000Z","open":{},"high":{},"low":{},)"
          R"("close":{},"volume":1000.0,"divCash":{},"splitFactor":{}}})",
          r.date,
          r.close,
          r.close,
          r.close,
          r.close,
          r.div_cash,
          r.split_factor
        );
    }
    return format("[{}]", out);
}

// A third payload shape, for the splice: open and close differ, because the
// anchor is taken from the opens and a series with open == close cannot tell a
// factor computed from the wrong one apart.
struct SpliceRow {
    string_view date;
    double open;
    double close;
    double div_cash;
    double split_factor;
};

// The prepended instrument. Listed late, and the first two bars are in the way on
// purpose: 01-03 falls inside the ramp-up window the test ignores, and 01-04
// carries a dividend, so the anchor can only be 01-05.
constexpr string_view k_new_symbol = "NEW";

constexpr std::array k_new_rows{
  SpliceRow{"2024-01-03"sv, 10.0, 10.5, 0.0, 1.0},
  SpliceRow{"2024-01-04"sv, 20.0, 21.0, 1.0, 1.0},
  SpliceRow{"2024-01-05"sv, 40.0, 44.0, 0.0, 1.0},
  SpliceRow{"2024-01-08"sv, 44.0, 45.0, 0.0, 1.0},
};

// The proxy, reaching four sessions further back, carrying one event of each kind
// before the anchor. Its 01-05 open of 8 against the 40 above makes the price
// factor exactly 5.
constexpr string_view k_old_symbol = "OLD";

constexpr std::array k_old_rows{
  SpliceRow{"2024-01-01"sv, 1.0, 1.1, 0.0, 2.0},
  SpliceRow{"2024-01-02"sv, 2.0, 2.2, 0.5, 1.0},
  SpliceRow{"2024-01-03"sv, 4.0, 4.4, 0.0, 1.0},
  SpliceRow{"2024-01-04"sv, 5.0, 5.5, 0.0, 1.0},
  SpliceRow{"2024-01-05"sv, 8.0, 8.8, 0.0, 1.0},
  SpliceRow{"2024-01-08"sv, 9.0, 9.9, 0.0, 1.0},
};

string tiingo_payload(span<const SpliceRow> rows)
{
    string out;
    for (const auto& r : rows) {
        if (!out.empty()) {
            out += ',';
        }
        out += format(
          R"({{"date":"{}T00:00:00.000Z","open":{},"high":{},"low":{},)"
          R"("close":{},"volume":1000.0,"divCash":{},"splitFactor":{}}})",
          r.date,
          r.open,
          std::max(r.open, r.close),
          std::min(r.open, r.close),
          r.close,
          r.div_cash,
          r.split_factor
        );
    }
    return format("[{}]", out);
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
        write_payload(k_tr_symbol, tiingo_payload(k_tr_rows));
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
    static DailyBarAvailability availability(MarketData& md, chr::local_days d, string_view symbol = k_symbol)
    {
        const auto got = md.daily_bar_availability(symbol, d);
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

// ------------------------------------------------ total return factor

class TotalReturnTest : public MarketDataTest
{
protected:
    double factor(MarketData& md, chr::local_days from, chr::local_days to)
    {
        const auto got = md.total_equity_return_factor_close_to_close(k_tr_symbol, from, to);
        EXPECT_TRUE(got.has_value()) << (got ? string{} : got.error());
        return got.value_or(std::numeric_limits<double>::quiet_NaN());
    }
};

TEST_F(TotalReturnTest, PriceOnlyWindows)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 9));

    EXPECT_DOUBLE_EQ(factor(md, day(2024, 1, 2), day(2024, 1, 3)), 110.0 / 100.0);
    EXPECT_DOUBLE_EQ(factor(md, day(2024, 1, 2), day(2024, 1, 4)), 121.0 / 100.0);
}

// The dividend is credited on its ex-date, and the window that ends there gets it.
TEST_F(TotalReturnTest, DividendIsIncludedByTheWindowEndingOnItsExDate)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 9));

    EXPECT_DOUBLE_EQ(factor(md, day(2024, 1, 4), day(2024, 1, 8)), 131.0 / 121.0);
}

// ...and the window *starting* there does not: the entry price is already ex, so
// the payment belongs to the earlier window. This is what lets adjacent windows
// chain without double-counting, so it is worth pinning separately.
TEST_F(TotalReturnTest, DividendIsExcludedByTheWindowStartingOnItsExDate)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 9));

    EXPECT_DOUBLE_EQ(factor(md, day(2024, 1, 8), day(2024, 1, 9)), (66.0 * 2.0) / 126.0);
}

// A 2-for-1 halves the price; undoing it must leave the factor showing the real
// gain and not a 50% loss.
TEST_F(TotalReturnTest, SplitIsUndone)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 9));

    EXPECT_GT(factor(md, day(2024, 1, 8), day(2024, 1, 9)), 1.0);
}

// The whole history in one call must equal the product of its parts -- the
// property the half-open boundary exists to provide.
TEST_F(TotalReturnTest, AdjacentWindowsChain)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 9));

    const double whole = factor(md, day(2024, 1, 2), day(2024, 1, 9));
    const double parts = factor(md, day(2024, 1, 2), day(2024, 1, 4)) * factor(md, day(2024, 1, 4), day(2024, 1, 8))
                       * factor(md, day(2024, 1, 8), day(2024, 1, 9));

    EXPECT_DOUBLE_EQ(whole, 131.0 / 100.0 * (132.0 / 126.0));
    EXPECT_NEAR(whole, parts, 1e-12);
}

TEST_F(TotalReturnTest, EndpointsMustBeTradingDays)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 9));

    // 01-06 is inside the range and has no bar; nearest-match would silently
    // return a plausible number instead.
    EXPECT_FALSE(md.total_equity_return_factor_close_to_close(k_tr_symbol, day(2024, 1, 6), day(2024, 1, 9)));
    EXPECT_FALSE(md.total_equity_return_factor_close_to_close(k_tr_symbol, day(2024, 1, 2), day(2024, 1, 6)));
    EXPECT_FALSE(md.total_equity_return_factor_close_to_close(k_tr_symbol, day(2023, 12, 1), day(2024, 1, 9)));
}

TEST_F(TotalReturnTest, WindowMustRunForwards)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 9));

    EXPECT_FALSE(md.total_equity_return_factor_close_to_close(k_tr_symbol, day(2024, 1, 9), day(2024, 1, 2)));
    EXPECT_FALSE(md.total_equity_return_factor_close_to_close(k_tr_symbol, day(2024, 1, 4), day(2024, 1, 4)));
}

// The guard applies to both ends, not just the later one.
using TotalReturnDeathTest = TotalReturnTest;

TEST_F(TotalReturnDeathTest, EitherEndpointPastAsOfTerminates)
{
    MarketData md = market_data();
    md.set_as_of(day(2024, 1, 4));

    EXPECT_DEATH((void)md.total_equity_return_factor_close_to_close(k_tr_symbol, day(2024, 1, 2), day(2024, 1, 9)), "");
    EXPECT_DEATH((void)md.total_equity_return_factor_close_to_close(k_tr_symbol, day(2024, 1, 8), day(2024, 1, 3)), "");
}

// ------------------------------------------------------- splicing on a proxy

class SpliceTest : public MarketDataTest
{
protected:
    void SetUp() override
    {
        MarketDataTest::SetUp();
        write_payload(k_new_symbol, tiingo_payload(k_new_rows));
        write_payload(k_old_symbol, tiingo_payload(k_old_rows));
    }

    // The splice every test below starts from: one calendar day of ramp-up
    // ignored, which drops 01-03 and puts the anchor on 01-05.
    static void splice(MarketData& md)
    {
        const auto done = md.prepend_equity_with_proxy(k_new_symbol, day(2024, 1, 8), k_old_symbol, chr::days(1));
        ASSERT_TRUE(done.has_value()) << (done ? string{} : done.error());
    }

    static DailyBar bar(MarketData& md, string_view symbol, chr::local_days d)
    {
        const auto got = md.daily_bar(symbol, d);
        EXPECT_TRUE(got.has_value()) << (got ? string{} : got.error());
        return got.value_or(DailyBar{});
    }

    static CorporateActions actions(MarketData& md, chr::local_days d)
    {
        const auto got = md.corporate_actions(k_new_symbol, d);
        EXPECT_TRUE(got.has_value()) << (got ? string{} : got.error());
        return got.value_or(CorporateActions{});
    }
};

// The history now starts where the proxy's does, at prices on the prepended
// instrument's scale: 2.0 * (40/8) = 10.0.
TEST_F(SpliceTest, HistoryReachesBackToTheProxy)
{
    MarketData md = market_data();
    splice(md);
    md.set_as_of(day(2024, 1, 8));

    EXPECT_EQ(availability(md, day(2024, 1, 2), k_new_symbol), available) << "before NEW listed";
    EXPECT_DOUBLE_EQ(bar(md, k_new_symbol, day(2024, 1, 2)).open, 10.0);
    EXPECT_DOUBLE_EQ(bar(md, k_new_symbol, day(2024, 1, 2)).close, 11.0);
}

// From the anchor forward the prepended instrument's own prices are untouched --
// scaling those instead of the proxy's would restate the live series.
TEST_F(SpliceTest, BarsFromTheAnchorAreUnscaled)
{
    MarketData md = market_data();
    splice(md);
    md.set_as_of(day(2024, 1, 8));

    EXPECT_DOUBLE_EQ(bar(md, k_new_symbol, day(2024, 1, 5)).open, 40.0);
    EXPECT_DOUBLE_EQ(bar(md, k_new_symbol, day(2024, 1, 8)).close, 45.0);
}

// 01-03 exists in both, and after a one-day ramp-up it is the proxy's bar that
// survives: NEW's own opens at 10, the scaled proxy's at 4.0 * 5 = 20.
TEST_F(SpliceTest, RampUpBarsAreReplacedByTheProxy)
{
    MarketData md = market_data();
    splice(md);
    md.set_as_of(day(2024, 1, 8));

    EXPECT_DOUBLE_EQ(bar(md, k_new_symbol, day(2024, 1, 3)).open, 20.0);
}

// A distribution is quoted in the units of the row it lands on, so it scales with
// the prices; a split ratio has no units and does not.
TEST_F(SpliceTest, ProxyEventsBeforeTheAnchorAreCarried)
{
    MarketData md = market_data();
    splice(md);
    md.set_as_of(day(2024, 1, 8));

    EXPECT_DOUBLE_EQ(actions(md, day(2024, 1, 2)).distribution_amount, 2.5);
    EXPECT_DOUBLE_EQ(actions(md, day(2024, 1, 1)).split_factor, 2.0);
}

// NEW's own 01-04 dividend goes with the bars it belonged to. Keeping it would
// credit a payment against the proxy's price series, which never made it.
TEST_F(SpliceTest, PrependedEventsBeforeTheAnchorAreDropped)
{
    MarketData md = market_data();
    splice(md);
    md.set_as_of(day(2024, 1, 8));

    EXPECT_DOUBLE_EQ(actions(md, day(2024, 1, 4)).distribution_amount, 0.0);
}

// The point of the whole operation: a return window that spans the splice.
//
//   01-02 -> 01-08   45/11, no event in the half-open window
//   01-01 -> 01-03   (11 + 2.5)/5.5 * 22/11, the carried dividend included
TEST_F(SpliceTest, ReturnsSpanTheSplice)
{
    MarketData md = market_data();
    splice(md);
    md.set_as_of(day(2024, 1, 8));

    const auto across = md.total_equity_return_factor_close_to_close(k_new_symbol, day(2024, 1, 2), day(2024, 1, 8));
    ASSERT_TRUE(across.has_value()) << (across ? string{} : across.error());
    EXPECT_DOUBLE_EQ(*across, 45.0 / 11.0);

    const auto early = md.total_equity_return_factor_close_to_close(k_new_symbol, day(2024, 1, 1), day(2024, 1, 3));
    ASSERT_TRUE(early.has_value()) << (early ? string{} : early.error());
    EXPECT_DOUBLE_EQ(*early, 13.5 / 5.5 * 2.0);
}

TEST_F(SpliceTest, ProxyIsLeftAlone)
{
    MarketData md = market_data();
    splice(md);
    md.set_as_of(day(2024, 1, 8));

    EXPECT_DOUBLE_EQ(bar(md, k_old_symbol, day(2024, 1, 2)).open, 2.0);
    EXPECT_DOUBLE_EQ(actions(md, day(2024, 1, 2)).distribution_amount, 2.5) << "NEW still carries the scaled copy";
    const auto proxy_actions = md.corporate_actions(k_old_symbol, day(2024, 1, 2));
    ASSERT_TRUE(proxy_actions.has_value()) << proxy_actions.error();
    EXPECT_DOUBLE_EQ(proxy_actions->distribution_amount, 0.5);
}

// A ramp-up window that swallows the overlap leaves no day the two share.
TEST_F(SpliceTest, NoUsableAnchorIsReported)
{
    MarketData md = market_data();

    const auto done = md.prepend_equity_with_proxy(k_new_symbol, day(2024, 1, 8), k_old_symbol, chr::days(365));
    EXPECT_FALSE(done.has_value());
}

TEST_F(SpliceTest, SelfProxyIsRejected)
{
    MarketData md = market_data();

    EXPECT_FALSE(md.prepend_equity_with_proxy(k_new_symbol, day(2024, 1, 8), k_new_symbol, chr::days(0)).has_value());
}

// The proxy starts no earlier, so there is nothing to prepend. Reported rather
// than silently doing nothing: a no-op splice is a mistake in the universe, and
// the backtest that follows would run on a history the caller thinks is longer.
TEST_F(SpliceTest, ProxyWithNoEarlierHistoryIsReported)
{
    MarketData md = market_data();

    const auto done = md.prepend_equity_with_proxy(k_old_symbol, day(2024, 1, 8), k_new_symbol, chr::days(0));
    EXPECT_FALSE(done.has_value());
}

// A failed splice must not leave a half-rewritten history behind.
TEST_F(SpliceTest, FailedSpliceLeavesTheCacheUntouched)
{
    MarketData md = market_data();
    ASSERT_FALSE(md.prepend_equity_with_proxy(k_new_symbol, day(2024, 1, 8), k_old_symbol, chr::days(365)));
    md.set_as_of(day(2024, 1, 8));

    EXPECT_EQ(availability(md, day(2024, 1, 2), k_new_symbol), before_first_bar);
    EXPECT_DOUBLE_EQ(bar(md, k_new_symbol, day(2024, 1, 3)).open, 10.0) << "the ramp-up bar is still there";
    EXPECT_DOUBLE_EQ(actions(md, day(2024, 1, 4)).distribution_amount, 1.0);
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
