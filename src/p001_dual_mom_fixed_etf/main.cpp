#include "Backtest.h"
#include "GridReport.h"
#include "benchmarks.h"
#include "handle_single_report.h"

using namespace std::chrono_literals;

enum class BacktestCommand {
    single_custom,
    single_default,
    grid,
    benchmark_spy,
    benchmark_60_40
};

enum class BacktestPeriod {
    in_sample,
    validation,
    out_of_sample
};

constexpr auto k_backtest_period = BacktestPeriod::in_sample;
constexpr auto k_backtest_command = BacktestCommand::single_default;
constexpr auto k_broker_cost_scheme = BrokerCostScheme::flat_10bp;
constexpr auto k_provider = Provider::tiingo;

namespace
{
expected<BacktestReport, string> run_custom_backtest(
  chr::year_month start_month, chr::year_month end_month, const dual_mom_fixed_etf_algorithm::Config& ac
)
{
    const auto bc = BacktestConfig{
      .broker_cost_scheme = k_broker_cost_scheme,
      .provider = k_provider,
      .start_month = start_month,
      .end_month = end_month
    };
    return run_backtest(bc, ac, nullptr);
}

using RebalanceDay = dual_mom_fixed_etf_algorithm::RebalanceDay;

expected<BacktestReport, string> run_benchmark(BenchmarkType benchmark_type)
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
      .provider = k_provider,
      .start_month = start_month,
      .end_month = end_month
    };

    const auto ac = dual_mom_fixed_etf_algorithm::Config{
      .equities = get_all_assets(benchmark_type), // Used only for determining trading days.
      .defensive_asset = nullopt,
      .cash_proxy = "DTB3",
      .lookback_period = chr::days(1), // Not used here.
      .rebalance_day = RebalanceDay::last_trading_day_of_month,
      .max_portfolio_size = 1 // Not used here.
    };

    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = make_unique<MarketData>(mdcfg, bc.provider);

    for (auto& e : get_all_assets(benchmark_type)) {
        if (e == "EIF") {
            TRY_OR_FAIL(market_data->prepend_equity_with_proxy(
              "IEF", chr::local_days(end_month / chr::day(1)), "VFITX", chr::days(180)
            ));
        } else if (e == "SPY") {
            // Nothing to do.
        } else {
            LOG(FATAL) << format("Unexpected equity: {}", e);
        }
    }

    return run_backtest(bc, ac, MOVE(market_data), benchmark_type);
}

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
      .provider = k_provider,
      .start_month = start_month,
      .end_month = end_month
    };

    vector<string> equities;
    optional<string> defensive_asset;

    switch (universe) {
    case Universe::full:
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
        defensive_asset.reset();
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

expected<UniverseResult, string> run_backtest_grid_for_universe(Universe universe)
{
    UniverseResult ur{.universe = universe};
    for (const auto lookback : {chr::months(6), chr::months(9), chr::months(12)}) {
        for (const auto timing :
             {RebalanceDay::last_trading_day_of_month, RebalanceDay::month_10th, RebalanceDay::month_15th}) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(auto result, run_backtest_grid_cell(universe, lookback, timing));
            // TODO store result into ur.
            const auto key = GridKey{.timing = timing, .lookback = lookback};
            ur.cells.emplace_back(key, MOVE(result));
        }
    }
    return ur;
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
          run_backtest_grid_cell(Universe::full, chr::months(12), RebalanceDay::last_trading_day_of_month);
        return handle_single_report(result);
    }
    case BacktestCommand::grid: {
        std::vector<UniverseResult> urs;
        for (const auto universe : {Universe::full, Universe::drop_efa, Universe::drop_spy, Universe::drop_ief}) {
            const auto ur_or = run_backtest_grid_for_universe(universe);
            LOG_IF(FATAL, !ur_or) << format("run_backtest_grid_for_universe failed: {}", ur_or.error());
            urs.emplace_back(MOVE(*ur_or));
        }
        print_grid_report(urs);
        return 0;
    }
    case BacktestCommand::benchmark_spy: {
        const auto r = run_benchmark(BenchmarkType::spy);
        LOG_IF(FATAL, !r) << r.error();
        return 0;
    }
    case BacktestCommand::benchmark_60_40: {
        const auto r = run_benchmark(BenchmarkType::spy_60_ief_40);
        LOG_IF(FATAL, !r) << r.error();
    }
    } // switch
} // function
