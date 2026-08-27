#pragma once

#include "BacktestReport.h"

#include "DualMomFixedEtfAlgorithm.h"
#include "atlib/marketdata/MarketData.h"
#include "atlib/papertrading/papertrading.h"

struct BacktestConfig {
    BrokerCostScheme broker_cost_scheme;
    Provider provider;
    chr::year_month_day first_day, last_day;
    std::vector<pair<string, double>> initial_portfolio, initial_desired_portfolio;
};

// If no market_date supplied, the backtest will create its own one.
expected<BacktestReport, string> run_backtest(
  const BacktestConfig& bc,
  const dual_mom_fixed_etf_algorithm::Config& ac,
  unique_ptr<MarketData> maybe_market_data,
  bool maintain_initial_desired_portfolio =
    false // Benchmark can set to true to keep the same portfolio throughout, instead of invoking the algorithm.
);
