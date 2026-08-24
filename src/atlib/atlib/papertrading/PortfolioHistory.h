#pragma once

#include "AppendableIntervals.h"

#include <meadow/cppext.h>
#include <meadow/finance.h>

#include <flat_map>

struct Portfolio;

struct PortfolioHistory {
    void make_snapshot(
      chr::local_days date,
      double cash_proxy_level,
      const Portfolio& portfolio,
      const std::flat_map<string, double>& equity_prices
    );

    // Remove trading days and intervals from trading_days[n]. No-op if trading_days is already smaller.
    void trading_days_truncate(size_t n);

    // Return cash snapshots, the returned vector corresponds to the `trading_days` vector.
    NODIS vector<double> cash_for_trading_days() const;

    // Return total snapshots, the returned vector corresponds to the `trading_days` vector.
    NODIS vector<double> total_for_trading_days() const;

    // Return total[i]/total[i-1] snapshots, the returned vector corresponds to the `trading_days` vector but its one
    // item shorter since it represents periods between days.
    NODIS vector<double> return_factors_for_trading_days() const;

    NODIS vector<double> equity_position_values_for_trading_days(const string& symbol) const;

    // Return CAGR (Compound Annual Growth Rate) for the period trading_days.front() .. back()
    // trading_days must not be empty.
    NODIS double cagr() const;

    NODIS pair<double, int> max_drawdown_and_longest_underwater_days() const;

    // Needs at least 2 trading days.
    NODIS double sharpe_daily(SharpeAggregation aggregation) const;

    // Needs at least 2 indices.
    NODIS double
    sharpe_through_selected_days(SharpeAggregation aggregation, span<const size_t> trading_days_indices) const;

    NODIS optional<double> worst_12_month_return() const;

    struct TradingDay {
        chr::local_days date;
        double total = NAN;
        // cash_proxy[0] is 1.0, cash_proxy[i] is cash_proxy[i-1] * return_factor_from_previous_day.
        double cash_proxy_level = 1.0; // Cumulative product of the inter-day cash_proxy return factors.
    };

    vector<TradingDay> trading_days;

    // The following interval maps' keys are indices into `trading_days`.
    AppendableIntervals<size_t, double> cash{0.0};
    std::flat_map<string, AppendableIntervals<size_t, double>> equity_position_values;

    int num_market_orders = 0;
    int num_portfolio_changes = 0;
};
