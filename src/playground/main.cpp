#include "atlib/marketdata/MarketData.h"
#include "atlib/marketdata/total_return.h"
#include "meadow/file.h"

using Period = variant<chr::days, chr::weeks, chr::months>;

namespace
{
// `day` minus one `period`, on the calendar and not in days: a month back from a
// month-end is the next month-end, whatever the two months are worth in days.
chr::local_days minus(chr::local_days day, Period period)
{
    return switch_variant(
      period,
      [day](chr::days d) -> chr::local_days {
          return day - d;
      },
      [day](chr::weeks w) -> chr::local_days {
          return day - w;
      },
      [day](chr::months m) -> chr::local_days {
          const chr::year_month_day ymd{day};
          const chr::year_month back = chr::year_month(ymd.year(), ymd.month()) - m;
          // A day-of-month the shorter month does not have -- 31 March back one
          // month -- lands on its last day. Anything else would have to skip a
          // month or spill into the next one.
          const chr::year_month_day candidate = back / ymd.day();
          return chr::local_days(candidate.ok() ? candidate : chr::year_month_day(back / chr::last));
      }
    );
}

string to_string(Period period)
{
    return switch_variant(
      period,
      [](chr::days d) -> string {
          return format("{} days", d.count());
      },
      [](chr::weeks w) -> string {
          return format("{} weeks", w.count());
      },
      [](chr::months m) -> string {
          return format("{} months", m.count());
      }
    );
}

// A calendar year, averaged over the leap cycle. The windows here are
// calendar windows -- twelve months back from a date -- so what divides
// them is calendar days and not the 252 trading days a daily series would
// be annualized by.
constexpr double k_days_per_year = 365.2425;

// What a full year at this window's rate would come to.
//
// Compounding rather than scaling, because a factor is multiplicative: the
// year is however many of these windows fit into it, and they compose by
// being multiplied.
//
// Measured over the window that was actually walked, not the period that
// was asked for. The window opens at the last session on or before one
// period back, so it runs a few days long, and using the nominal period
// here would credit those extra days' growth to the rest of the year too.
double annualized(double factor, chr::local_days from, chr::local_days to)
{
    return std::pow(factor, k_days_per_year / ifcast<double>((to - from).count()));
}

// Total return factors for `symbol` on every trading day in
// [`begin_day`, `end_day`], each measuring the `period` that ends on that
// day. With `annualize`, each factor is restated as an annual rate --
// comparable across periods, and meaningless for a period short enough
// that a year of it is not a thing that could happen.
vector<pair<chr::local_days, double>> return_factors_over_time(
  string_view symbol, chr::year_month_day begin_day, chr::year_month_day end_day, Period period, bool annualize
)
{
    CHECK(begin_day.ok());
    CHECK(end_day.ok());

    // WORKSPACE_DIR comes from CMakeLists.txt; see there.
    const MarketDataConfig config{.workspace_dir = WORKSPACE_DIR};
    const EquityHistory history =
      TRY_OR_FAIL(equity_history(config, Provider::tiingo, symbol, chr::local_days(end_day)));

    // The days to report on are the trading days in the range, which is the set
    // of bars -- equity_history() has already dropped everything after
    // `end_day`, so only the near end has to be found.
    const auto first = ra::lower_bound(history.bars, chr::local_days(begin_day), {}, &DailyBar::date);

    vector<pair<chr::local_days, double>> returns;
    returns.reserve(ucast(history.bars.end() - first));

    for (auto bar = first; bar != history.bars.end(); ++bar) {
        // One period back is a calendar date and usually not a trading day --
        // a weekend for two days in seven -- so the window opens at the last
        // session on or before it. Back rather than forward, so the window
        // covers the whole period rather than falling short of it, and so the
        // choice does not depend on which side happens to be nearer.
        const auto after_start = ra::upper_bound(history.bars, minus(bar->date, period), {}, &DailyBar::date);
        if (after_start == history.bars.begin()) {
            // The window opens before this symbol listed. Every instrument has
            // one period of these at the start of its history; there is no
            // return to report, and a partial one would be a different quantity
            // silently mixed in with the rest.
            continue;
        }

        const auto start = after_start - 1;
        const double factor = TRY_OR_FAIL(total_return_factor_close_to_close(history, start->date, bar->date));
        returns.emplace_back(bar->date, annualize ? annualized(factor, start->date, bar->date) : factor);
    }

    return returns;
}
} // namespace

int main()
{
    using namespace std::literals;
    const auto from = chr::year_month_day(2020y, chr::January, 1d);
    const auto to = chr::year_month_day(2025y, chr::December, 31d);

    optional<int> local_days_origin;
    string mfile;
    int c = 1;
    vector<string> labels;
    for (const auto symbol : {"SPY", "EFA", "IEF", "BIL"}) {
        for (const auto period : {// Period{chr::months(3)},
                                  // Period{chr::months(6)},
                                  // Period{chr::months(9)},
                                  Period{chr::months(3)}
             }) {
            const auto r = return_factors_over_time(symbol, from, to, period, true);
            if (!local_days_origin) {
                local_days_origin = iicast<int>(r.front().first.time_since_epoch().count());
            }
            vector<int> days;
            vector<double> return_factors;
            days.reserve(r.size());
            return_factors.reserve(r.size());
            for (auto& x : r) {
                days.push_back(iicast<int>(x.first.time_since_epoch().count()) - *local_days_origin);
                return_factors.push_back(x.second);
            }
            mfile += format("x{} = [{:n}];\n", c, days);
            mfile += format("y{} = [{:n}];\n", c, return_factors);
            labels.push_back(format("{} - {}", symbol, to_string(period)));
            ++c;
        }
    }
    mfile += "plot(";
    for (int i = 1; i <= c - 1; i++) {
        if (i != 1)
            mfile += ", ";
        mfile += format("x{}, y{}", i, i);
    }
    mfile += "), grid;\n";
    mfile += "legend(";
    for (int i = 1; i <= c - 1; i++) {
        if (i != 1)
            mfile += ", ";
        mfile += format("\"{}\"", labels[sucast(i) - 1]);
    }
    mfile += ");\n";
    CHECK(write_string_to_file(mfile, "/tmp/returns.m"));

    return 0;
}
