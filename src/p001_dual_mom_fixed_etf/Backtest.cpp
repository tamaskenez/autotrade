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

expected<chr::local_days, string>
first_trading_day_of_month(MarketData& market_data, chr::year_month month, const vector<string>& all_assets)
{
    using namespace std::chrono_literals;
    const auto end_day = chr::local_days((month + chr::months(1)) / 1d);
    market_data.clear_as_of();
    for (auto day = chr::local_days(month / 1d); day < end_day; ++day) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const bool trading_day, is_trading_day(market_data, day, all_assets));
        if (trading_day) {
            return day;
        }
    }
    return unexpected(format("Couldn't find a trading day in the month {}.", month));
}

expected<chr::local_days, string> first_rebalance_day_of_month(
  MarketData& market_data, chr::year_month month, const dual_mom_fixed_etf_algorithm::Config& ac
)
{
    using namespace std::chrono_literals;
    const auto begin_day = chr::local_days(month / 1d);
    const auto end_day = chr::local_days((month + chr::months(1)) / 1d);
    market_data.clear_as_of();
    for (auto day = begin_day; day < end_day; ++day) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto& maybe_rebalance_day,
          dual_mom_fixed_etf_algorithm::get_rebalance_day_for_past_day(market_data, ac, day)
        );
        if (auto rebalance_day = switch_variant(
              maybe_rebalance_day,
              [&](chr::local_days d) -> optional<chr::local_days> {
                  return in_co_range(d, begin_day, end_day) ? optional(d) : nullopt;
              },
              [](dual_mom_fixed_etf_algorithm::NotARebalanceDay) -> optional<chr::local_days> {
                  return nullopt;
              }
            )) {
            return *rebalance_day;
        }
    }
    return unexpected(format("Couldn't find a rebalance day in the month {}.", month));
}
} // namespace

expected<BacktestReport, string> run_backtest(const BacktestConfig& bc, const dual_mom_fixed_etf_algorithm::Config& ac)
{
    constexpr auto k_initial_cash = 100000.0;
    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = MarketData(mdcfg, bc.provider);

    vector<string> all_assets = ac.equities;
    all_assets.push_back(ac.defensive_asset);

    CHECK(bc.start_month < bc.end_month);
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
      const auto start_date, first_rebalance_day_of_month(market_data, bc.start_month, ac)
    );
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
      const auto end_date, first_trading_day_of_month(market_data, bc.end_month, all_assets)
    );
    vector<string> previous_desired_portfolio;
    auto portfolio = Portfolio{.cash = k_initial_cash};
    optional<vector<MarketOrder>> pending_market_orders;
    BacktestReport report;
    // The loop variable is called `past_trading_day` because it assumes that we have access to this day's data. So if
    // this day is a rebalance day, we can do it using this day's close prices.
    // However, different sections of the loop are to be interpreted as being executed at different times during the
    // day.
    // 1: applying corporate actions before everything else, it updates portfolio and pending orders
    // 2: submitting pending orders (in real life: before exchange opens).
    // 3: filling pending orders with today's opening prices
    // 4: rebalancing (in real life: after exchange closes and all data is uploaded by the provider and before next
    // day's opening
    const auto report_this_trading_day =
      [&report, &portfolio, &market_data](chr::local_days past_trading_day) -> expected<void, string> {
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
        return {};
    };
    const auto apply_corporate_actions =
      [&portfolio, &market_data, &pending_market_orders](chr::local_days past_trading_day) -> expected<void, string> {
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
        return {};
    };

    for (auto past_trading_day = start_date; past_trading_day <= end_date; ++past_trading_day) {
        market_data.set_as_of(past_trading_day);

        TRY_OR_RETURN_UNEXPECTED(apply_corporate_actions(past_trading_day));
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
                report.num_market_orders += uscast(pending_market_orders->size());
                pending_market_orders.reset();
                portfolio = response.portfolio;
                portfolio.clear_below(1e-6);
                println("Portfolio after rebalance:");
                for (auto&& [symbol, q] : portfolio.equities) {
                    println("- {}: {:.3f} shares", symbol, q);
                }
                println("- cash: {}", portfolio.cash);
            }
            TRY_OR_RETURN_UNEXPECTED(report_this_trading_day(past_trading_day));
        }

        // Note that we need ask for rebalance day even on days we know are not trading days. The reason is that
        // if our rebalance days are last-trading-day-of-month and day 31. is not a trading day, we can only figure
        // out that day 30 was a trading day by checking on the 31th.
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto& maybe_rebalance_day,
          dual_mom_fixed_etf_algorithm::get_rebalance_day_for_past_day(market_data, ac, past_trading_day)
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
              // Even if the rebalance day is not today, it must have been the last trade day.
              CHECK(!report.local_days.empty() && report.local_days.back() == rebalance_day);
              report.rebalance_day_idcs.push_back(report.local_days.size() - 1);
              if (response.desired_portfolio == previous_desired_portfolio) {
                  // println("[{}] -> {} NO CHANGE", past_trading_day, rebalance_day);
              } else {
                  println("[{}] REBALANCE to {}", rebalance_day, response.desired_portfolio);
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
    }

    // Remove local days after the last rebalance day.
    if (!report.rebalance_day_idcs.empty()) {
        const auto N = report.rebalance_day_idcs.back() + 1;
        CHECK(N <= s);
        report.local_days.resize(N);
        report.total.resize(N);
        report.cash.resize(N);
        for (const auto&& [_, v] : report.equities) {
            v.resize(N);
        }
    }
    CHECK(
      report.rebalance_day_idcs.empty()
      || (report.rebalance_day_idcs.front() == 0 && report.rebalance_day_idcs.back() == report.local_days.size() - 1)
    );

    report.cash_proxy_level.reserve(report.local_days.size());
    report.cash_proxy_level.push_back(1.0);
    for (size_t i = 1; i < report.local_days.size(); ++i) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto crf,
          market_data.total_cash_return_factor(ac.cash_proxy, report.local_days[i - 1], report.local_days[i])
        );
        report.cash_proxy_level.push_back(report.cash_proxy_level.back() * crf);
    }
    return report;
}
