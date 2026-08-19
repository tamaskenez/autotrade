#pragma once

#include "atlib/papertrading/PortfolioHistory.h"

struct BacktestReport {
    PortfolioHistory portfolio_history;
    vector<size_t> rebalance_day_idcs;
};
