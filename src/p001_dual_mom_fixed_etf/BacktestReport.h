#pragma once

#include <meadow/cppext.h>
#include <meadow/finance.h>

#include <flat_map>

struct BacktestReport {
    vector<chr::local_days> local_days;
    vector<size_t> rebalance_day_idcs;
    vector<double> cash, total;
    // cash_proxy[0] is 1.0, cash_proxy[i] is cash_proxy[i-1] * return_factor_from_previous_day.
    vector<double> cash_proxy_level; // Cumulative product of the inter-day cash_proxy return factors.
    std::flat_map<string, vector<double>> equities;
    int num_market_orders = 0;

    NODIS double cagr() const;
    NODIS pair<double, int> max_drawdown_and_longest_underwater_days() const;

    NODIS double sharpe_daily(SharpeAggregation aggregation) const;

    NODIS double sharpe_through_rebalance_days(SharpeAggregation aggregation) const;
};
