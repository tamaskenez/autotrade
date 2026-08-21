#include "Backtest.h"
#include "handle_single_report.h"

using namespace std::chrono_literals;

enum class BacktestCommand {
    single_custom,
    single_default,
    grid
};

enum class BacktestPeriod {
    in_sample,
    validation,
    out_of_sample
};

constexpr auto k_backtest_period = BacktestPeriod::in_sample;
constexpr auto k_backtest_command = BacktestCommand::single_default;
constexpr auto k_broker_cost_scheme = BrokerCostScheme::flat_10bp;

namespace
{
expected<BacktestReport, string> run_custom_backtest(
  chr::year_month start_month, chr::year_month end_month, const dual_mom_fixed_etf_algorithm::Config& ac
)
{
    const auto bc = BacktestConfig{
      .broker_cost_scheme = k_broker_cost_scheme,
      .provider = Provider::tiingo,
      .start_month = start_month,
      .end_month = end_month
    };
    return run_backtest(bc, ac, nullptr);
}

enum class Universe {
    all,
    drop_efa,
    drop_spy,
    drop_ief
};

using RebalanceDay = dual_mom_fixed_etf_algorithm::RebalanceDay;

expected<BacktestReport, string>
run_backtest_grid_cell(Universe universe, chr::months lookback_period, RebalanceDay rebelance_day)
{
    chr::year_month start_month{}, end_month{};

    switch (k_backtest_period) {
    case BacktestPeriod::in_sample:
        start_month = 1997y / chr::May;
        end_month = 2013y / chr::January;
        break;
    case BacktestPeriod::validation:
        start_month = 2013y / chr::January;
        end_month = 2019y / chr::January;
        break;
    case BacktestPeriod::out_of_sample:
        start_month = 2020y / chr::January;
        end_month = 2026y / chr::August;
        break;
    }

    const auto bc = BacktestConfig{
      .broker_cost_scheme = k_broker_cost_scheme,
      .provider = Provider::tiingo,
      .start_month = start_month,
      .end_month = end_month
    };

    vector<string> equities;
    string defensive_asset;

    switch (universe) {
    case Universe::all:
        equities = {"SPY", "EFA"};
        defensive_asset = "IEF";
        break;
    case Universe::drop_efa:
        equities = {"SPY"};
        defensive_asset = "IEF";
        break;
    case Universe::drop_spy:
        equities = {"EFA"};
        defensive_asset = "IEF";
        break;
    case Universe::drop_ief:
        equities = {"SPY", "EFA"};
        defensive_asset = "DTB3";
        break;
    }

    const auto ac = dual_mom_fixed_etf_algorithm::Config{
      .equities = equities,
      .defensive_asset = defensive_asset,
      .cash_proxy = "DTB3",
      .lookback_period = lookback_period,
      .rebalance_day = rebelance_day,
      .max_portfolio_size = 1
    };

    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = make_unique<MarketData>(mdcfg, bc.provider);
    TRY_OR_FAIL(
      market_data->prepend_equity_with_proxy("IEF", chr::local_days(end_month / chr::day(1)), "VFITX", chr::days(180))
    );
    TRY_OR_FAIL(
      market_data->prepend_equity_with_proxy("EFA", chr::local_days(end_month / chr::day(1)), "VGTSX", chr::days(180))
    );

    return run_backtest(bc, ac, MOVE(market_data));
}

struct UniverseResult {
};

expected<UniverseResult, string> run_backtest_grid_for_universe(Universe universe)
{
    UniverseResult ur;
    for (const auto lookback : {chr::months(6), chr::months(9), chr::months(12)}) {
        for (const auto timing :
             {RebalanceDay::first_trading_day_of_month, RebalanceDay::month_10th, RebalanceDay::month_15th}) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto result, run_backtest_grid_cell(universe, lookback, timing));
            // TODO store result into ur.
            (void)ur;
        }
    }
    return {};
}
} // namespace

int main()
{
    switch (k_backtest_command) {
    case BacktestCommand::single_custom: {
        const auto ac = dual_mom_fixed_etf_algorithm::Config{
          .equities = {"SPY", "EFA"},
          .defensive_asset = "IEF",
          .cash_proxy = "DTB3",
          .lookback_period = chr::months(12),
          .rebalance_day = dual_mom_fixed_etf_algorithm::RebalanceDay::last_trading_day_of_month,
          .max_portfolio_size = 1
        };
        return handle_single_report(run_custom_backtest(2004y / chr::January, 2015y / chr::January, ac));
    }
    case BacktestCommand::single_default: {
        const auto result =
          run_backtest_grid_cell(Universe::all, chr::months(12), RebalanceDay::last_trading_day_of_month);
        return handle_single_report(result);
    }
    case BacktestCommand::grid: {
        for (const auto universe : {Universe::all, Universe::drop_efa, Universe::drop_spy, Universe::drop_ief}) {
            const auto ur = run_backtest_grid_for_universe(universe);
        }
        return 0;
    }
    }
}
