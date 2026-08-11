#include "DualMomFixedEtfAlgorithm.h"
#include "atlib/marketdata/MarketData.h"

struct BacktestConfig {
    Provider provider;
    chr::year_month_day start_date, end_date; // inclusive

    // TODO: transaction cost
};

struct BacktestResult {
};

expected<BacktestResult, string> run_backtest(const BacktestConfig& bc, dual_mom_fixed_etf_algorithm::Config ac);
