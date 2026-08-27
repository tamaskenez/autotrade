#include "Backtest.h"

#include "atlib/papertrading/papertrading.h"

#include <magic_enum/magic_enum.hpp>
#include <meadow/math.h>

namespace
{
constexpr auto k_initial_cash = 100000.0;

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

expected<chr::local_days, string> first_trading_day_on_or_before(
  MarketData& market_data, chr::local_days day_arg, chr::local_days earliest_day, const vector<string>& all_assets
)
{
    using namespace std::chrono_literals;
    market_data.clear_as_of();
    for (auto day = day_arg; earliest_day <= day; --day) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const bool trading_day, is_trading_day(market_data, day, all_assets));
        if (trading_day) {
            return day;
        }
    }
    return unexpected(format("Couldn't find a trading day from {} back to {}.", day_arg, earliest_day));
}

expected<chr::local_days, string> first_trading_day_on_or_after(
  MarketData& market_data, chr::local_days day_arg, chr::local_days last_day, const vector<string>& all_assets
)
{
    using namespace std::chrono_literals;
    market_data.clear_as_of();
    for (auto day = day_arg; day <= last_day; ++day) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const bool trading_day, is_trading_day(market_data, day, all_assets));
        if (trading_day) {
            return day;
        }
    }
    return unexpected(format("Couldn't find a trading day from {} until {}.", day_arg, last_day));
}
} // namespace

expected<BacktestReport, string> run_backtest(
  const BacktestConfig& bc,
  const dual_mom_fixed_etf_algorithm::Config& ac,
  unique_ptr<MarketData> maybe_market_data,
  bool maintain_initial_desired_portfolio
)
{
    if (!maybe_market_data) {
        const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
        maybe_market_data = make_unique<MarketData>(mdcfg, bc.provider);
    }
    auto& market_data = *maybe_market_data;

    vector<string> all_assets = ac.equities;
    if (ac.defensive_asset) {
        all_assets.push_back(*ac.defensive_asset);
    }
    std::flat_map<string, double> asset_prices;

    CHECK(bc.first_day <= bc.last_day);
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
      const auto first_trading_day_within_window,
      first_trading_day_on_or_after(
        market_data, chr::local_days(bc.first_day), chr::local_days(bc.last_day), all_assets
      )
    );
    optional<chr::local_days> first_trading_day_before_window;
    if (!bc.initial_portfolio.empty()) {
        // We need to buy the initial portfolio at the close of the trading day strictly before bc.first_day. That day
        // will also serve as the first day, the initial record.
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          first_trading_day_before_window,
          first_trading_day_on_or_before(
            market_data,
            chr::local_days(bc.first_day) - chr::days(1),
            chr::local_days(bc.first_day) - chr::days(31),
            all_assets
          )
        );
    }
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
      const auto last_trading_day,
      first_trading_day_on_or_before(
        market_data, chr::local_days(bc.last_day), chr::local_days(bc.first_day), all_assets
      )
    );
    auto portfolio = Portfolio{.cash = k_initial_cash};
    optional<vector<MarketOrder>> pending_market_orders;
    if (!bc.initial_portfolio.empty()) {
        double sum_weights = 0;
        vector<string> all_initial_assets;
        for (auto&& [symbol, weight] : bc.initial_portfolio) {
            CHECK(std::isfinite(weight) && weight > 0);
            sum_weights += weight;
            all_initial_assets.push_back(symbol);
        }
        const double cash_per_unit_weight = portfolio.cash / sum_weights;
        for (auto&& [s, w] : bc.initial_portfolio) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(
              const auto& bar, market_data.daily_bar(s, first_trading_day_before_window.value())
            );
            const double cash = w * cash_per_unit_weight;
            const double shares = cash / bar.close;
            portfolio.cash -= cash;
            portfolio.equities[s] = shares;
        }
    }

    BacktestReport report;
    const auto dur = switch_variant(
      ac.lookback_period,
      [](chr::months x) {
          return format("{}m", x.count());
      },
      [](chr::weeks x) {
          return format("{}w", x.count());
      },
      [](chr::days x) {
          return format("{}d", x.count());
      }
    );
    report.switch_sequence = format("signal_date,exec_date,from,to,ret_{}_spy,ret_{}_efa,ret_{}_cash\n", dur, dur, dur);

    struct PendingSignalReport {
        chr::local_days date;
        std::flat_map<string, double> return_factors;
    };
    optional<PendingSignalReport> pending_signal_report;
    // The loop variable is called `past_trading_day` because it assumes that we have access to this day's data. So if
    // this day is a rebalance day, we can do it using this day's close prices.
    // However, different sections of the loop are to be interpreted as being executed at different times during the
    // day.
    // 1: applying corporate actions before everything else, it updates portfolio and pending orders
    // 2: submitting pending orders (in real life: before exchange opens).
    // 3: filling pending orders with today's opening prices
    // 4: rebalancing (in real life: after exchange closes and all data is uploaded by the provider and before next
    // day's opening
    const auto report_this_trading_day = [&report, &portfolio, &market_data, &all_assets, &asset_prices, &ac](
                                           chr::local_days past_trading_day
                                         ) -> expected<void, string> {
        double cash_proxy_level = 1.0;
        if (!report.portfolio_history.trading_days.empty()) {
            const auto& tdb = report.portfolio_history.trading_days.back();
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(
              const auto crf, market_data.total_cash_return_factor(ac.cash_proxy, tdb.date, past_trading_day)
            );
            cash_proxy_level = tdb.cash_proxy_level * crf;
        }
        // Report.
        for (const auto& s : all_assets) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& bar, market_data.daily_bar(s, past_trading_day));
            asset_prices[s] = bar.close;
        }
        report.portfolio_history.make_snapshot(past_trading_day, cash_proxy_level, portfolio, asset_prices);
        return {};
    };
    const auto apply_corporate_actions_and_dtb3 =
      [&portfolio,
       &market_data,
       &pending_market_orders,
       &ac,
       first_trading_day = first_trading_day_before_window.value_or(first_trading_day_within_window),
       &report](chr::local_days past_trading_day) -> expected<void, string> {
        // Apply DTB3 return factor on cash.
        if (past_trading_day != first_trading_day) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(
              const auto cash_return_factor,
              market_data.total_cash_return_factor(ac.cash_proxy, past_trading_day - chr::days(1), past_trading_day)
            );
            portfolio.cash *= cash_return_factor;
        }
        // Apply corporate actions, both on portfolio and pending orders.
        std::flat_map<string, int> dividends_paid;
        for (auto&& [symbol, shares] : portfolio.equities) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& ca, market_data.corporate_actions(symbol, past_trading_day));
            shares *= ca.split_factor;
            // Note that receiving the dividend immediately and potentially investing it tomorrow is not realistic:
            // dividends are paid weeks later which creates a slight advantage of the backtesting compared to live
            // trading.
            portfolio.cash += ca.distribution_amount * shares;
            CHECK(ca.distribution_amount >= 0);
            if (ca.distribution_amount > 0) {
                dividends_paid[symbol] = 1;
            }
        }
        for (const auto& s : dividends_paid.keys()) {
            ++report.num_days_with_dividends[s];
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

    if (first_trading_day_before_window) {
        TRY_OR_RETURN_UNEXPECTED(report_this_trading_day(*first_trading_day_before_window));
    }

    if (!bc.initial_desired_portfolio.empty()) {
        const auto effective_rebalance_day = first_trading_day_before_window.value_or(first_trading_day_within_window);
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          auto market_orders,
          market_orders_from_portfolio_change(
            bc.broker_cost_scheme, market_data, portfolio, bc.initial_desired_portfolio, effective_rebalance_day
          )
        );
        if (!market_orders.empty()) {
            pending_market_orders = MOVE(market_orders);
            pending_signal_report = PendingSignalReport{
              .date = effective_rebalance_day, .return_factors = {}
            }; // Empty return_factors will print NANs.
        }
    }
    const auto first_day = first_trading_day_before_window ? *first_trading_day_before_window + chr::days(1)
                                                           : first_trading_day_within_window;
    for (auto past_day = first_day; past_day <= last_trading_day; ++past_day) {
        market_data.set_as_of(past_day);

        TRY_OR_RETURN_UNEXPECTED(apply_corporate_actions_and_dtb3(past_day));
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const bool trading_day, is_trading_day(market_data, past_day, all_assets));
        if (trading_day) {
            if (pending_market_orders) {
                // Trading day, apply pending market orders as MOO.
                TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                  const auto& response,
                  apply_market_orders(
                    bc.broker_cost_scheme,
                    market_data,
                    past_day,
                    MarketOrderPriceType::open,
                    portfolio,
                    *pending_market_orders
                  )
                );
                report.portfolio_history.num_market_orders += iicast<int>(pending_market_orders->size());
                pending_market_orders.reset();
                // Track portfolio changes.
                {
                    vector<string> portfolio_before, portfolio_after;
                    for (auto&& [s, q] : portfolio.equities) {
                        if (q != 0) {
                            portfolio_before.push_back(s);
                        }
                    }
                    for (auto&& [s, q] : response.portfolio.equities) {
                        if (q != 0) {
                            portfolio_after.push_back(s);
                        }
                    }
                    ra::sort(portfolio_before);
                    ra::sort(portfolio_after);
                    if (portfolio_before != portfolio_after) {
                        ++report.portfolio_history.num_portfolio_changes;
                        if (pending_signal_report) {
                            double ret_spy = NAN;
                            double ret_efa = NAN;
                            double ret_cash = NAN;
                            for (auto&& [k, v] : pending_signal_report->return_factors) {
                                if (k == "SPY") {
                                    ret_spy = 100 * (v - 1);
                                } else if (k == "EFA") {
                                    ret_efa = 100 * (v - 1);
                                } else if (k == ac.cash_proxy) {
                                    ret_cash = 100 * (v - 1);
                                }
                            }
                            string from, to;
                            if (portfolio_before.size() == 1) {
                                from = portfolio_before[0];
                            } else if (portfolio_before.size() > 1) {
                                from = "?";
                            }
                            if (portfolio_after.size() == 1) {
                                to = portfolio_after[0];
                            } else if (portfolio_after.size() > 1) {
                                to = "?";
                            }
                            report.switch_sequence += format(
                              "{},{},{},{},{:.2f},{:.2f},{:.2f}\n",
                              pending_signal_report->date,
                              past_day,
                              from,
                              to,
                              ret_spy,
                              ret_efa,
                              ret_cash
                            );
                        }
                    }
                }
                pending_signal_report.reset();
                portfolio = response.portfolio;
                println("Portfolio after rebalance:");
                for (auto&& [symbol, q] : portfolio.equities) {
                    println("- {}: {:.3f} shares", symbol, q);
                }
                println("- cash: {}", portfolio.cash);
            }
            TRY_OR_RETURN_UNEXPECTED(report_this_trading_day(past_day));
        }

        // Note that we need ask for rebalance day even on days we know are not trading days. The reason is that
        // if our rebalance days are last-trading-day-of-month and day 31. is not a trading day, we can only figure
        // out that day 30 was a trading day by checking on the 31th.
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto maybe_rebalance_day_1,
          dual_mom_fixed_etf_algorithm::get_rebalance_day_for_past_day(
            market_data, ac.rebalance_day, all_assets, past_day
          )
        );

        using get_rebalance_day_t = variant<chr::local_days, dual_mom_fixed_etf_algorithm::NotARebalanceDay>;
        using expected_get_rebalance_day_t = expected<get_rebalance_day_t, string>;

        get_rebalance_day_t maybe_rebalance_day;

        {
            const auto maybe_rebalance_day_2_or = switch_variant(
              maybe_rebalance_day_1,
              [&](chr::local_days rebalance_day) -> expected_get_rebalance_day_t {
                  // Ignore the rebalance day if it's before the first day of the window.
                  if (rebalance_day < first_trading_day_within_window) {
                      return dual_mom_fixed_etf_algorithm::NotARebalanceDay{
                        .why = format(
                          "Rebalance day {} is before the first trading day within the window ({})",
                          rebalance_day,
                          first_trading_day_within_window
                        )
                      };
                  } else {
                      return rebalance_day;
                  }
              },
              [&](const dual_mom_fixed_etf_algorithm::NotARebalanceDay& narb) -> expected_get_rebalance_day_t {
                  // Also we need to cheat near the last day: in the case of last-trading-day-of-week/month policy we
                  // need to make sure the last_trading_day is recognized as a rebalance day, even if it's not the last
                  // calendar day of the week/month (in which case it would be returned only when asking on the last day
                  // of the week/month).
                  if (past_day != last_trading_day) {
                      return narb;
                  }
                  chr::local_days last_day_of_period;
                  switch (ac.rebalance_day) {
                  case dual_mom_fixed_etf_algorithm::RebalanceDay::first_trading_day_of_month:
                  case dual_mom_fixed_etf_algorithm::RebalanceDay::first_trading_day_of_week:
                  case dual_mom_fixed_etf_algorithm::RebalanceDay::month_10th:
                  case dual_mom_fixed_etf_algorithm::RebalanceDay::month_15th:
                      return narb;
                  case dual_mom_fixed_etf_algorithm::RebalanceDay::last_trading_day_of_month: {
                      const chr::year_month_day ymd(past_day);
                      last_day_of_period = chr::local_days(ymd.year() / ymd.month() / chr::last);
                  } break;
                  case dual_mom_fixed_etf_algorithm::RebalanceDay::last_trading_day_of_week:
                      last_day_of_period = past_day - (chr::weekday(past_day) - chr::Monday) + chr::days(6);
                      break;
                  }

                  market_data.set_as_of(last_day_of_period);
                  TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                    const auto maybe_rebalance_day_from_last_day,
                    dual_mom_fixed_etf_algorithm::get_rebalance_day_for_past_day(
                      market_data, ac.rebalance_day, all_assets, last_day_of_period
                    )
                  );
                  market_data.set_as_of(past_day);
                  return switch_variant(
                    maybe_rebalance_day_from_last_day,
                    [&](chr::local_days rebalance_day_from_last_day) -> expected_get_rebalance_day_t {
                        if (rebalance_day_from_last_day == past_day) {
                            return past_day;
                        } else {
                            return narb;
                        }
                    },
                    [&](const dual_mom_fixed_etf_algorithm::NotARebalanceDay&) -> expected_get_rebalance_day_t {
                        return narb;
                    }
                  );
              }
            );

            TRY_ASSIGN_OR_RETURN_UNEXPECTED(maybe_rebalance_day, MOVE(maybe_rebalance_day_2_or));
        }

        const auto rebalance_result = switch_variant(
          maybe_rebalance_day,
          [&](chr::local_days rebalance_day) -> expected<void, string> {
              if (pending_market_orders) {
                  return unexpected("Pending market orders should be cleared before rebalancing.");
              }
              vector<pair<string, double>> desired_portfolio;
              if (maintain_initial_desired_portfolio) {
                  desired_portfolio = bc.initial_desired_portfolio;
              } else {
                  TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                    auto response, dual_mom_fixed_etf_algorithm::rebalance(market_data, ac, rebalance_day)
                  );
                  CHECK(!pending_signal_report);
                  pending_signal_report =
                    PendingSignalReport{.date = rebalance_day, .return_factors = MOVE(response.return_factors)};
                  desired_portfolio = make_uniformly_weighted_portfolio(MOVE(response.desired_portfolio));
              }
              // Even if the rebalance day is not today, it must have been the last trade day.
              auto& tds = report.portfolio_history.trading_days;
              CHECK(!tds.empty() && tds.back().date == rebalance_day);
              report.rebalance_day_idcs.push_back(tds.size() - 1);
              TRY_ASSIGN_OR_RETURN_UNEXPECTED(
                auto market_orders,
                market_orders_from_portfolio_change(
                  bc.broker_cost_scheme, market_data, portfolio, desired_portfolio, rebalance_day
                )
              );
              if (market_orders.empty()) {
                  pending_signal_report.reset();
              } else {
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

    if (pending_market_orders) {
        report.pending_market_orders = *MOVE(pending_market_orders);
    }

    return report;
}
