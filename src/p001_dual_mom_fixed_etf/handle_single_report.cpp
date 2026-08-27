#include "handle_single_report.h"

#include "GridReport.h"

#include "atlib/chrono.h"

#include <magic_enum/magic_enum.hpp>
#include <meadow/matlab.h>

int handle_single_report(
  string_view universe,
  string_view lookback,
  dual_mom_fixed_etf_algorithm::RebalanceDay timing,
  const expected<BacktestReport, string>& result
)
{
    if (!result) {
        println("ERROR: {}", result.error());
        return EXIT_FAILURE;
    }

    FILE* f = fopen("/tmp/result.m", "wt");
    print(f, "day=[");
    vector<double> tick_datenums;
    vector<string> tick_labels;
    const auto& ph = result->portfolio_history;
    auto last_ymd = chr::year_month_day(ph.trading_days.front().date);
    for (auto& td : ph.trading_days) {
        const auto d = td.date;
        print(f, " {}", matlab::datenum(d));
        const auto ymd = chr::year_month_day(d);
        if (ymd.year() != last_ymd.year() && ymd.month() == chr::month(1)) {
            tick_datenums.push_back(matlab::datenum(d));
            tick_labels.push_back(format("{}", ymd.year()));
            last_ymd = ymd;
        } else if (ymd.month() != last_ymd.month()) {
            tick_datenums.push_back(matlab::datenum(d));
            if ((static_cast<unsigned>(ymd.month()) - 1) % 3 == 0) {
                tick_labels.push_back(format("{:%b}", ymd.month()));
            } else {
                tick_labels.emplace_back("|");
            }
            last_ymd = ymd;
        }
    }
    println(f, "]';");
    println(f, "tick_datenums={}';", tick_datenums);
    print(f, "tick_labels=[");
    bool first = true;
    for (const auto& s : tick_labels) {
        if (first) {
            first = false;
        } else {
            print(f, "; ");
        }
        print(f, "'{}'", s);
    }
    println(f, "];");
    println(f, "cash={}';", ph.cash_for_trading_days());
    println(f, "total={}';", ph.total_for_trading_days());
    for (const auto& k : ph.equity_position_values.keys()) {
        println(f, "eq_{}={}';", k, ph.equity_position_values_for_trading_days(k));
    }
    print(f, "plot(day, total, ':'");
    for (const auto& s : ph.equity_position_values.keys()) {
        print(f, ", day, eq_{}", s);
    }
    println(f, "), grid");
    print(f, "legend('total'");
    for (const auto& s : ph.equity_position_values.keys()) {
        print(f, ", '{}'", s);
    }
    println(f, ")");
    println(f, "set(gca, 'xtick', tick_datenums);");
    println(f, "set(gca, 'xticklabel', tick_labels);");
    fclose(f);

    println("==== REPORT ====");
    const auto df = ph.trading_days.front().date;
    const auto db = ph.trading_days.back().date;
    const auto ymdf = chr::year_month_day(df);
    const auto ymdb = chr::year_month_day(db);
    println("Period: {} .. {}", df, db);
    println(
      "{} calendar days, {} months, {:.2f} years, {} trading days",
      (db - df).count(),
      (ymdb.year() / ymdb.month() - ymdf.year() / ymdf.month()) / chr::months(1),
      years_between_days(df, db),
      ph.trading_days.size()
    );
    println("CAGR: {:.2f}%", 100 * ph.cagr());
    auto [max_drawdown, longest_underwater_days] = ph.max_drawdown_and_longest_underwater_days();
    println("max drawdown: {:.2f}%, longest underwater: {} days", 100 * max_drawdown, longest_underwater_days);
    println("Num market orders: {}", ph.num_market_orders);
    if (const auto N = result->rebalance_day_idcs.size(); N >= 2) {
        const auto avg = ifcast<double>((ph.trading_days[result->rebalance_day_idcs.back()].date
                                         - ph.trading_days[result->rebalance_day_idcs.front()].date)
                                          .count())
                       / ifcast<double>(N - 1);
        println("Avg cal. days between rebal.: {:.2f}", avg);
    }

    {
        const auto sh_da = ph.sharpe_daily(SharpeAggregation::arithmetic);
        const auto sh_dg = ph.sharpe_daily(SharpeAggregation::geometric);
        const auto idcs = ph.monthly_sharpe_indices();
        const auto sh_ma = ph.sharpe_through_selected_days(SharpeAggregation::arithmetic, idcs);
        const auto sh_mg = ph.sharpe_through_selected_days(SharpeAggregation::geometric, idcs);
        println("+---------------+------------+-----------+");
        println("|               | arithmetic | geometric |");
        println("+---------------+------------+-----------+");
        println("| Sharpe daily  | {:>10.2f} | {:>9.2f} |", sh_da.value_or(NAN), sh_dg.value_or(NAN));
        println("| Sharpe rebal. | {:>10.2f} | {:>9.2f} |", sh_ma.value_or(NAN), sh_mg.value_or(NAN));
        println("+---------------+------------+-----------+");
    }

    // Worst rolling 12-month:
    if (const auto worst_return = ph.worst_12_month_return()) {
        println("Worst 12-month return: {:.2f}%", 100 * *worst_return);
    }
    for (auto&& [k, v] : result->num_days_with_dividends) {
        println("{}: {} dividend days", k, v);
    }
    println();

    println("==== EQUITY CURVE ACROSS EXECUTION DAYS ====");
    print_grid_report_as_csv({});
    print_csv_report_line(universe, lookback, timing, *result);

    const double total0 = result->portfolio_history.trading_days.front().total;
    vector<string> assets;
    println("");
    println("date,value,position");
    if (!ph.trading_days.empty()) {
        vector<size_t> idcs;
        idcs.reserve(result->rebalance_day_idcs.size() + 1);
        idcs.push_back(0); // Always add the initial trading day.
        for (auto ix : result->rebalance_day_idcs) {
            if (ix + 1 < ph.trading_days.size()) {
                idcs.push_back(ix + 1); // Execution day is right after rebalance day.
            }
        }
        // Add the last trading day, if needed.
        if (idcs.back() != ph.trading_days.size() - 1) {
            idcs.push_back(ph.trading_days.size() - 1);
        }
        for (const auto ix : idcs) {
            const auto& td = result->portfolio_history.trading_days[ix];

            assets.clear();
            for (auto&& [k, ivs] : result->portfolio_history.equity_position_values) {
                const auto vs = ivs.get_values_for_key_range(ix, ix + 1);
                assert(vs.size() == 1);
                if (vs.front() != 0) {
                    assets.push_back(k);
                }
            }
            print("{},{:.2f},", td.date, 100.0 * td.total / total0);
            if (assets.empty()) {
                println();
            } else if (assets.size() == 1) {
                println("{}", assets.front());
            } else {
                println("{}", assets);
            }
        }
    }
    println("=== SWITCH SEQUENCE ===");
    println("{}", result->switch_sequence);
    if (result->pending_market_orders.empty()) {
        println("==== NO PENDING ORDERS ====");
    } else {
        println("==== PENDING ORDERS ====");
        for (const auto& x : result->pending_market_orders) {
            println(
              "MarketOrder{{.symbol = \"{}\", .unit = {}, .quantity = {}}}",
              x.symbol,
              magic_enum::enum_name(x.unit),
              x.quantity
            );
        }
    }
    return EXIT_SUCCESS;
}
