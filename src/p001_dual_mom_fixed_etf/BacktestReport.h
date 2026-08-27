#pragma once

#include "atlib/papertrading/PortfolioHistory.h"
#include "atlib/papertrading/papertrading.h"

struct BacktestReport {
    PortfolioHistory portfolio_history;
    vector<size_t> rebalance_day_idcs;
    std::flat_map<string, int> num_days_with_dividends;
    string switch_sequence;
    vector<MarketOrder> pending_market_orders;
};
