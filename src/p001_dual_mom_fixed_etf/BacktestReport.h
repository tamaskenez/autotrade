#pragma once

#include "atlib/papertrading/PortfolioHistory.h"

struct BacktestReport {
    PortfolioHistory portfolio_history;
    vector<size_t> rebalance_day_idcs;
    std::flat_map<string, int> num_days_with_dividends;
};
