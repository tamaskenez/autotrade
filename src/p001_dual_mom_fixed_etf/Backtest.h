#pragma once

#include "BacktestReport.h"

#include "DualMomFixedEtfAlgorithm.h"
#include "atlib/marketdata/MarketData.h"
#include "atlib/papertrading/papertrading.h"

#include <meadow/finance.h>

struct BacktestConfig {
    BrokerCostScheme broker_cost_scheme;
    Provider provider;
    // The backtest starts at the first rebalance day on or after the 1st day of the start_month
    // and ends on the last rebalance day on or before the first trading day of the end_month.
    chr::year_month start_month, end_month;
};

expected<BacktestReport, string> run_backtest(const BacktestConfig& bc, const dual_mom_fixed_etf_algorithm::Config& ac);
