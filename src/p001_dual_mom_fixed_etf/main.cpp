#include "Backtest.h"

int main()
{
    using namespace std::chrono_literals;
    const auto bc = BacktestConfig{
      .provider = Provider::tiingo, .start_date = 2004y / chr::January / 1d, .end_date = 2014y / chr::December / 31d
    };
    const auto ac = dual_mom_fixed_etf_algorithm::Config{
      .equities = {"SPY", "EFA"},
      .defensive_asset = "IEF",
      .cash_proxy = "DTB3",
      .lookback_period = chr::months(12),
      .rebalance_day = dual_mom_fixed_etf_algorithm::RebalanceDay::last_trading_day_of_month,
      .max_portfolio_size = 1
    };
    const auto result = run_backtest(bc, ac);
    if (!result) {
        println("ERROR: {}", result.error());
        return EXIT_FAILURE;
    }

    return 0;
}
