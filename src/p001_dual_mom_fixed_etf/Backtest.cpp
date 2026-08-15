#include "Backtest.h"

#include "atlib/papertrading/papertrading.h"

#include <magic_enum/magic_enum.hpp>

#include "meadow/math.h"

namespace
{
expected<bool, string> is_trading_day(MarketData& market_data, chr::local_days day, const vector<string>& symbols)
{
    // Find out if this is a trading day. If it is, we act as if the exchange was about to open and we
    // submit the pending market orders as MOO.
    int num_available = 0;
    int num_not_traded = 0;
    for (const auto& s : symbols) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto dab, market_data.daily_bar_availability(s, day));
        switch (dab) {
        case DailyBarAvailability::before_first_bar:
            return unexpected(format("past_trading_day ({}) is before asset {} first bar.", day, s));
        case DailyBarAvailability::available:
            ++num_available;
            break;
        case DailyBarAvailability::not_traded:
        case DailyBarAvailability::after_last_bar:
            ++num_not_traded;
            break;
        }
    }
    if (sgn(num_available) == sgn(num_not_traded)) {
        return unexpected(format(
          "past_trading_day ({}) has {} traded and {} not traded assets, it should be all one or all the "
          "other.",
          day,
          num_available,
          num_not_traded
        ));
    }
    return num_available > 0;
}
} // namespace

expected<BacktestResult, string> run_backtest(const BacktestConfig& bc, const dual_mom_fixed_etf_algorithm::Config& ac)
{
    constexpr auto k_initial_cash = 100000.0;
    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = MarketData(mdcfg, bc.provider);

    const auto end_date = chr::local_days(bc.end_date);
    vector<string> previous_desired_portfolio;
    auto portfolio = Portfolio{.cash = k_initial_cash};
    optional<vector<MarketOrder>> pending_market_orders;
    BacktestResult report;
    vector<string> all_assets = ac.equities;
    all_assets.push_back(ac.defensive_asset);
    // The loop variable is called `past_trading_day` because it assumes that we have access to this day's data. So if
    // this day is a rebalance day, we can do it using this day's close prices.
    // However, different sections of the loop are to be interpreted as being executed at different times during the
    // day.
    // 1: applying corporate actions before everything else, it updates portfolio and pending orders
    // 2: submitting pending orders (in real life: before exchange opens).
    // 3: filling pending orders with today's opening prices
    // 4: rebalancing (in real life: after exchange closes and all data is uploaded by the provider and before next
    // day's opening
    for (auto past_trading_day = chr::local_days(bc.start_date); past_trading_day <= end_date; ++past_trading_day) {
        market_data.set_as_of(past_trading_day);

        // Apply corporate actions, both on portfolio and pending orders.
        for (auto&& [symbol, shares] : portfolio.equities) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& ca, market_data.corporate_actions(symbol, past_trading_day));
            shares *= ca.split_factor;
            // Note that receiving the dividend immediately and potentially investing it tomorrow is not realistic:
            // dividends are paid weeks later which creates a slight advantage of the backtesting compared to live
            // trading.
            portfolio.cash += ca.distribution_amount * shares;
        }
        if (pending_market_orders) {
            for (auto& mo : *pending_market_orders) {
                switch (mo.unit) {
                case MarketOrder::Unit::shares: {
                    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                      const auto& ca, market_data.corporate_actions(mo.symbol, past_trading_day)
                    );
                    mo.quantity *= ca.split_factor;
                } break;
                case MarketOrder::Unit::cash:
                    break;
                }
            }
        }

        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const bool trading_day, is_trading_day(market_data, past_trading_day, all_assets)
        );
        if (trading_day) {
            if (pending_market_orders) {
                // Trading day, apply pending market orders as MOO.
                TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                  const auto& response,
                  apply_market_orders(
                    bc.broker_cost_scheme,
                    market_data,
                    past_trading_day,
                    MarketOrderPriceType::open,
                    portfolio,
                    *pending_market_orders
                  )
                );
                pending_market_orders.reset();
                portfolio = response.portfolio;
                portfolio.clear_below(1e-6);
                println("New portfolio: cash: {}", portfolio.cash);
                for (auto&& [symbol, q] : portfolio.equities) {
                    println("- {}: {:.3f} shares", symbol, q);
                }
            }
            if (!portfolio.equities.empty()) {
                // Report.
                const auto ix = report.local_days.size();
                report.local_days.push_back(past_trading_day);
                report.cash.push_back(portfolio.cash);
                double total = portfolio.cash;
                for (auto&& [symbol, shares] : portfolio.equities) {
                    TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& bar, market_data.daily_bar(symbol, past_trading_day));
                    const auto q = shares * bar.close;
                    auto& es = report.equities[symbol];
                    if (es.size() <= ix) {
                        es.resize(ix + 1);
                    }
                    es[ix] = q;
                    total += q;
                }
                report.total.push_back(total);
            }
        }

        // Note that we need ask for rebalance day even on days we know are not trading days. The reason is that
        // if our rebalance days are last-trading-day-of-month and day 31. is not a trading day, we can only figure
        // out that day 30 was a trading day by checking on the 31th.
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto& maybe_rebalance_day,
          dual_mom_fixed_etf_algorithm::get_past_trading_day_to_rebalance_after(market_data, ac, past_trading_day)
        );

        const auto rebalance_result = switch_variant(
          maybe_rebalance_day,
          [&](chr::local_days rebalance_day) -> expected<void, string> {
              if (pending_market_orders) {
                  return unexpected("Pending market orders should be cleared before rebalancing.");
              }
              TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                const auto& response, dual_mom_fixed_etf_algorithm::rebalance(market_data, ac, rebalance_day)
              );
              if (response.desired_portfolio == previous_desired_portfolio) {
                  // println("[{}] -> {} NO CHANGE", past_trading_day, rebalance_day);
              } else {
                  println("[{}] -> {} NEW PORTFOLIO: {}", past_trading_day, rebalance_day, response.desired_portfolio);
                  previous_desired_portfolio = response.desired_portfolio;
                  TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                    auto market_orders,
                    market_orders_from_portfolio_change(
                      bc.broker_cost_scheme, market_data, portfolio, response.desired_portfolio, rebalance_day
                    )
                  );
                  for (const auto& mo : market_orders) {
                      println(
                        "- {} {}: {:.3f} {}",
                        mo.quantity > 0 ? " BUY" : "SELL",
                        mo.symbol,
                        abs(mo.quantity),
                        magic_enum::enum_name(mo.unit)
                      );
                  }
                  pending_market_orders = MOVE(market_orders);
              }
              return {};
          },
          [&](UNUSED const dual_mom_fixed_etf_algorithm::NotARebalanceDay& nard) -> expected<void, string> {
              return {};
          }
        );
        if (!rebalance_result) {
            return unexpected(rebalance_result.error());
        }
    }

    const auto s = report.local_days.size();
    CHECK(report.total.size() == s);
    CHECK(report.cash.size() == s);
    for (const auto&& [_, v] : report.equities) {
        CHECK(v.size() <= s);
        v.resize(s);
    }
    return report;
}
