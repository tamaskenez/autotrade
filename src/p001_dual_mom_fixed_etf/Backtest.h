#pragma once

#include "BacktestReport.h"

#include "DualMomFixedEtfAlgorithm.h"
#include "atlib/marketdata/MarketData.h"
#include "atlib/papertrading/papertrading.h"

struct BacktestConfig {
    BrokerCostScheme broker_cost_scheme;
    Provider provider;
    // Backtest has two modes:
    //
    // If initial_portfolio is empty:
    //     The backtest window starts on the first trading day on or after the first day and ends on the last trading
    //     day on or before last_day. The initial_desired_portfolio will be turned to market orders and executed on
    //     first trading day open.
    //
    // If initial_portfolio is not empty:
    //     The backtest window starts on the last trading day before first_day and ends on the last trading day on or
    //     before the last_day. The initial portfolio is purchased on the close of the extra preceding day. All metrics
    //     refer to the extended window. Even if the extra preceding day is a rebalance day, no rebalancing will be
    //     performed there.
    chr::year_month_day first_day, last_day;
    std::vector<pair<string, double>> initial_portfolio,
      initial_desired_portfolio; // Weighted portfolio (not fractions).
};

// If no market_date supplied, the backtest will create its own one.
expected<BacktestReport, string> run_backtest(
  const BacktestConfig& bc,
  const dual_mom_fixed_etf_algorithm::Config& ac,
  unique_ptr<MarketData> maybe_market_data,
  // Benchmark can set maintain_initial_desired_portfolio to true to keep the same portfolio throughout, instead of
  // invoking the algorithm on rebalance days.
  bool maintain_initial_desired_portfolio = false
);
