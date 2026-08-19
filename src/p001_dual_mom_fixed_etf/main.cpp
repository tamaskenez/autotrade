#include "Backtest.h"

#include "atlib/chrono.h"

#include <meadow/matlab.h>

int main()
{
    using namespace std::chrono_literals;
    const auto bc = BacktestConfig{
      .provider = Provider::tiingo, .start_month = 2004y / chr::January, .end_month = 2015y / chr::January
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
        const auto sh_da = ph.sharpe_daily(SharpeAggregation::arithmetic);
        const auto sh_dg = ph.sharpe_daily(SharpeAggregation::geometric);
        const auto sh_ma = ph.sharpe_through_selected_days(SharpeAggregation::arithmetic, result->rebalance_day_idcs);
        const auto sh_mg = ph.sharpe_through_selected_days(SharpeAggregation::geometric, result->rebalance_day_idcs);
        println("+---------------+------------+-----------+");
        println("|               | arithmetic | geometric |");
        println("+---------------+------------+-----------+");
        println("| Sharpe daily  | {:>10.2f} | {:>9.2f} |", sh_da, sh_dg);
        println("| Sharpe rebal. | {:>10.2f} | {:>9.2f} |", sh_ma, sh_mg);
        println("+---------------+------------+-----------+");
    }
    // Worst rolling 12-month:
    if (const auto worst_return = ph.worst_12_month_return()) {
        println("Worst 12-month return: {:.2f}%", 100 * *worst_return);
    }

    return 0;
}
