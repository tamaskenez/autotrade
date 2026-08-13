#pragma once

#include <meadow/cppext.h>

class MarketData;

namespace dual_mom_fixed_etf_algorithm
{
enum class RebalanceDay {
    first_trading_day_of_month,
    last_trading_day_of_month,
    first_trading_day_of_week,
    last_trading_day_of_week
};

struct Config {
    vector<string> equities;
    string defensive_asset;

    // The hurdle the equity legs have to beat, as a FRED series id -- DTB3 for
    // this project.
    //
    // A parameter rather than a constant so the hurdle can be varied like any
    // other axis. Note what it cannot express: it names a *rate* series, read
    // through MarketData::total_cash_return_factor(), so it can be swapped for
    // another FRED rate but not for an ETF such as BIL, which is priced through
    // the equity query instead. Comparing against BIL -- the check BACKTEST.md
    // step 7 wants for the roll bias -- needs this to grow a second alternative,
    // not just a different string.
    string cash_proxy;

    variant<chr::months, chr::weeks, chr::days> lookback_period;
    RebalanceDay rebalance_day;

    // How many equities may be held at once. 1 is the strategy as written; above
    // that, the selected assets are held in equal weight.
    int max_portfolio_size = 1;
};

struct Response {
    vector<string> desired_portfolio;
};

// An ordinary day on which nothing is due -- not a failure. `why` is for logging
// and debugging, and nothing should branch on its text.
//
// A type rather than a bare string, because the function below already reports
// failures as a string: with both spelled `string`, `std::get_if<string>` and
// `.error()` are equally plausible at a call site, and confusing them silently
// turns a routine non-rebalance day into an error or an error into a routine day.
// Neither mistake would fail to compile. This one cannot be made.
struct NotARebalanceDay {
    string why;
};

// This algorithm rebalances the portfolio at certain days during year. Here's how to execute it:
// - Call get_past_trading_day_to_rebalance_after after each day. It can be called on non-trading days, too. The
//   important is that it must be called only when you are sure that if `past_day` was a trading day, then all the
//   data for that day has already been uploaded by the data provider.
// - If the answer is `chr::local_days`, call `rebalance`, by passing the day as `past_trading_day`. Otherwise it is a
//   `NotARebalanceDay` carrying the reason, which is for logging and debugging.
// An error, as opposed to a `NotARebalanceDay`, means something went wrong and not that today is uneventful.

// `past_trading_day` (and days before it which might be queried according to the config.rebalance_day) can't be
// before the first (historical bar) of any asset.
expected<variant<chr::local_days, NotARebalanceDay>, string>
get_past_trading_day_to_rebalance_after(MarketData& market_data, const Config& config, chr::local_days past_day);

// `past_trading_day` must have been a trading day with all the asset's daily bars available.
expected<Response, string> rebalance(MarketData& market_data, const Config& config, chr::local_days past_trading_day);
} // namespace dual_mom_fixed_etf_algorithm
