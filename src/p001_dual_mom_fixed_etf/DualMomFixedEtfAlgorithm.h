#pragma once
#include "atlib/papertrading/papertrading.h"

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
    int lookback_months;
    RebalanceDay rebalance_day;
    int max_portfolio_size = 1;
};

struct Response {
    string info;
    // The response contains the symbols of assets to buy (positive money) or sell (negative money)
    vector<pair<string, double>> assets_to_buy_or_sell;
};

// It's not error if no daily bars available for the past trading day, caller doesn't need to check if past_trading_day
// is a trading day or holiday. It's an error if some assets have daily bars while others don't.
expected<Response, string> execute_after_trading_day(
  chr::local_days past_trading_day, MarketData& market_data, const Config& config, const Portfolio& portfolio
);
} // namespace dual_mom_fixed_etf_algorithm
