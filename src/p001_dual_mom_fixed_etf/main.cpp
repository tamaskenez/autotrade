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
void prepend_equities_with_proxy(
  MarketData& market_data, const dual_mom_fixed_etf_algorithm::Config& ac, chr::local_days last_day
)
{
    constexpr auto k_skipped_first_days = chr::days(180);
    auto all_assets = ac.equities;
    if (ac.defensive_asset) {
        all_assets.push_back(*ac.defensive_asset);
    }
    for (const auto& e : all_assets) {
        if (e == "IEF") {
            TRY_OR_FAIL(market_data.prepend_equity_with_proxy("IEF", last_day, "VFITX", k_skipped_first_days));
        } else if (e == "EFA") {
            TRY_OR_FAIL(market_data.prepend_equity_with_proxy("EFA", last_day, "VGTSX", k_skipped_first_days));
        }
    }
}

expected<BacktestReport, string> run_custom_backtest(
  chr::year_month_day first_day,
  chr::year_month_day last_day,
  const dual_mom_fixed_etf_algorithm::Config& ac,
  std::vector<pair<string, double>> initial_weighted_portfolio = {}
)
{
    const auto bc = BacktestConfig{
      .broker_cost_scheme = k_broker_cost_scheme,
      .provider = k_provider,
      .first_day = first_day,
      .last_day = last_day,
      .initial_portfolio = MOVE(initial_weighted_portfolio)
    };

    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = make_unique<MarketData>(mdcfg, bc.provider);

    prepend_equities_with_proxy(*market_data, ac, chr::local_days(last_day));

    return run_backtest(bc, ac, MOVE(market_data));
}

using RebalanceDay = dual_mom_fixed_etf_algorithm::RebalanceDay;

pair<chr::year_month_day, chr::year_month_day> backtest_first_last_day(BacktestPeriod period)
{
    switch (period) {
    case BacktestPeriod::in_sample:
        // 1997-05-30 is the last trading day in that month, rebalance day for the last-month-of-day rule.
        return {1997y / chr::May / 30d, 2012y / chr::December / 31d};
    case BacktestPeriod::validation:
        return {2013y / chr::January / 1d, 2019y / chr::December / 31d};
    case BacktestPeriod::out_of_sample:
        return {2020y / chr::January / 1d, 2026y / chr::July / 31d};
    }
    std::unreachable();
}

expected<BacktestReport, string> run_benchmark(BenchmarkType benchmark_type)
{
    const auto [first_day, last_day] = backtest_first_last_day(k_backtest_period);

    const auto portfolio = get_benchmark_portfolio(benchmark_type);
    vector<pair<string, double>> initial_portfolio;
    switch (k_backtest_period) {
    case BacktestPeriod::in_sample:
        // No initial portfolio
        break;
    case BacktestPeriod::validation:
        initial_portfolio = portfolio;
        break;
    case BacktestPeriod::out_of_sample:
        break;
    }

    const auto bc = BacktestConfig{
      .broker_cost_scheme = k_broker_cost_scheme,
      .provider = k_provider,
      .first_day = first_day,
      .last_day = last_day,
      .initial_portfolio = initial_portfolio,
      .initial_desired_portfolio = portfolio
    };

    const auto ac = dual_mom_fixed_etf_algorithm::Config{
      .equities = get_benchmark_assets(benchmark_type), // Used only for determining trading days.
      .defensive_asset = nullopt,
      .cash_proxy = "DTB3",
      .lookback_period = chr::days(0), // Not used here.
      .rebalance_day = RebalanceDay::last_trading_day_of_month,
      .max_portfolio_size = 0 // Not used here.
    };

    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = make_unique<MarketData>(mdcfg, bc.provider);

    prepend_equities_with_proxy(*market_data, ac, chr::local_days(last_day));

    return run_backtest(bc, ac, MOVE(market_data), true);
}

expected<BacktestReport, string>
run_backtest_grid_cell(Universe universe, chr::months lookback_period, RebalanceDay rebalance_day)
{
    auto [first_day, last_day] = backtest_first_last_day(k_backtest_period);

    vector<string> equities;
    optional<string> defensive_asset;
    std::vector<pair<string, double>> initial_portfolio, desired_portfolio;

    switch (universe) {
    case Universe::full:
        equities = {"SPY", "EFA"};
        defensive_asset = "IEF";
        switch (k_backtest_period) {
        case BacktestPeriod::in_sample:
            // No initial portfolio
            break;
        case BacktestPeriod::validation:
            initial_portfolio = {
              {"SPY", 1.0}
            };
            desired_portfolio = {
              {"EFA", 1.0}
            };
            break;
        case BacktestPeriod::out_of_sample:
            break;
        }
        break;
    case Universe::drop_efa:
        equities = {"SPY"};
        defensive_asset = "IEF";
        switch (k_backtest_period) {
        case BacktestPeriod::in_sample:
            // No initial portfolio
            break;
        case BacktestPeriod::validation:
            initial_portfolio = {
              {"SPY", 1.0}
            };
            desired_portfolio = initial_portfolio;
            break;
        case BacktestPeriod::out_of_sample:
            break;
        }
        break;
    case Universe::drop_spy:
        equities = {"EFA"};
        defensive_asset = "IEF";
        switch (k_backtest_period) {
        case BacktestPeriod::in_sample:
            // No initial portfolio
            break;
        case BacktestPeriod::validation:
            initial_portfolio = {
              {"EFA", 1.0}
            };
            desired_portfolio = initial_portfolio;
            break;
        case BacktestPeriod::out_of_sample:
            break;
        }
        break;
    case Universe::drop_ief:
        equities = {"SPY", "EFA"};
        defensive_asset.reset();
        switch (k_backtest_period) {
        case BacktestPeriod::in_sample:
            // No initial portfolio
            break;
        case BacktestPeriod::validation:
            initial_portfolio = {
              {"SPY", 1.0}
            };
            desired_portfolio = {
              {"EFA", 1.0}
            };
            break;
        case BacktestPeriod::out_of_sample:
            break;
        }
        break;
    }

    const auto bc = BacktestConfig{
      .broker_cost_scheme = k_broker_cost_scheme,
      .provider = k_provider,
      .first_day = first_day,
      .last_day = last_day,
      .initial_portfolio = MOVE(initial_portfolio),
      .initial_desired_portfolio = MOVE(desired_portfolio)
    };

    const auto ac = dual_mom_fixed_etf_algorithm::Config{
      .equities = equities,
      .defensive_asset = defensive_asset,
      .cash_proxy = "DTB3",
      .lookback_period = lookback_period,
      .rebalance_day = rebalance_day,
      .max_portfolio_size = 1
    };

    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = make_unique<MarketData>(mdcfg, bc.provider);
    prepend_equities_with_proxy(*market_data, ac, chr::local_days(last_day));

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
        const auto lookback = switch_variant(ac.lookback_period, [](auto x) {
            return format("{}", x.count());
        });
        auto [first_day, last_day] = backtest_first_last_day(BacktestPeriod::validation);
        return handle_single_report(
          "CUSTOM",
          lookback,
          ac.rebalance_day,
          run_custom_backtest(
            first_day,
            last_day,
            ac,
            {
              {"EFA", 1.0}
        }
          )
        );
    }
    case BacktestCommand::single_default: {
        const auto result =
          run_backtest_grid_cell(Universe::full, chr::months(12), RebalanceDay::last_trading_day_of_month);
        return handle_single_report("FULL", "12", RebalanceDay::last_trading_day_of_month, result);
    }
    case BacktestCommand::grid: {
        std::vector<UniverseResult> urs;
        for (const auto universe : {Universe::full, Universe::drop_efa, Universe::drop_spy, Universe::drop_ief}) {
            const auto ur_or = run_backtest_grid_for_universe(universe);
            LOG_IF(FATAL, !ur_or) << format("run_backtest_grid_for_universe failed: {}", ur_or.error());
            urs.emplace_back(MOVE(*ur_or));
        }
        print_grid_report(urs);
        print_grid_report_as_csv(urs);
        return 0;
    }
    case BacktestCommand::benchmark_spy: {
        return handle_single_report(
          "BENCH_SPY", "n/a", RebalanceDay::last_trading_day_of_month, run_benchmark(BenchmarkType::spy)
        );
    }
    case BacktestCommand::benchmark_60_40: {
        return handle_single_report(
          "BENCH_6040", "n/a", RebalanceDay::last_trading_day_of_month, run_benchmark(BenchmarkType::spy_60_ief_40)
        );
    }
    } // switch
} // function
