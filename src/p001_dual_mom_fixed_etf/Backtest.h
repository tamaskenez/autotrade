#pragma once

#include "DualMomFixedEtfAlgorithm.h"
#include "atlib/marketdata/MarketData.h"
#include "atlib/papertrading/papertrading.h"

struct BacktestConfig {
    BrokerCostScheme broker_cost_scheme;
    Provider provider;
    chr::year_month_day start_date, end_date; // inclusive

    // TODO: transaction cost
};

struct BacktestResult {
    vector<chr::local_days> local_days;
    vector<double> cash, total;
    std::flat_map<string, vector<double>> equities;
};

expected<BacktestResult, string> run_backtest(const BacktestConfig& bc, const dual_mom_fixed_etf_algorithm::Config& ac);
