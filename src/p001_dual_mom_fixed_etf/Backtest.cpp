#include "Backtest.h"

#include "atlib/papertrading/papertrading.h"

#include <magic_enum/magic_enum.hpp>

expected<BacktestResult, string> run_backtest(const BacktestConfig& bc, dual_mom_fixed_etf_algorithm::Config ac)
{
    constexpr auto k_initial_cash = 100.0;
    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = MarketData(mdcfg, Provider::tiingo);

    const auto end_date = chr::local_days(bc.end_date);
    vector<string> previous_desired_portfolio;
    auto portfolio = Portfolio{.cash = k_initial_cash};
    for (auto past_trading_day = chr::local_days(bc.start_date); past_trading_day <= end_date; ++past_trading_day) {
        market_data.set_as_of(past_trading_day);
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto& maybe_rebalance_day,
          dual_mom_fixed_etf_algorithm::get_past_trading_day_to_rebalance_after(market_data, ac, past_trading_day)
        );

        const auto r = switch_variant(
          maybe_rebalance_day,
          [&](chr::local_days rebalance_day) -> expected<void, string> {
              TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                const auto& response, dual_mom_fixed_etf_algorithm::rebalance(market_data, ac, rebalance_day)
              );
              if (response.desired_portfolio == previous_desired_portfolio) {
                  println("[{}] -> {} NO CHANGE", past_trading_day, rebalance_day);
              } else {
                  println("[{}] -> {} NEW PORTFOLIO: {}", past_trading_day, rebalance_day, response.desired_portfolio);
                  previous_desired_portfolio = response.desired_portfolio;
                  TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                    const auto& market_orders,
                    market_orders_from_portfolio_change(
                      bc.broker_cost_scheme, market_data, portfolio, response.desired_portfolio, rebalance_day
                    )
                  );
                  for (const auto& mo : market_orders) {
                      println(
                        "- {} {}: {} {}",
                        mo.quantity > 0 ? " BUY" : "SELL",
                        mo.symbol,
                        abs(mo.quantity),
                        magic_enum::enum_name(mo.unit)
                      );
                  }
              }
              return {};
          },
          [&](UNUSED const dual_mom_fixed_etf_algorithm::NotARebalanceDay& nard) -> expected<void, string> {
#if 0
                println("\t[{}] SKIP: {}", past_trading_day,
                        nard.why);
#endif
              return {};
          }
        );
        if (!r) {
            return unexpected(r.error());
        }
    }

    return {};
}
