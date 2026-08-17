#include "Backtest.h"
#include "common.h"

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
    auto last_ymd = chr::year_month_day(result->local_days.front());
    for (auto d : result->local_days) {
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
    println(f, "cash={}';", result->cash);
    println(f, "total={}';", result->total);
    for (auto&& [k, v] : result->equities) {
        println(f, "eq_{}={}';", k, v);
    }
    print(f, "plot(day, total, ':'");
    for (const auto& s : result->equities.keys()) {
        print(f, ", day, eq_{}", s);
    }
    println(f, "), grid");
    print(f, "legend('total'");
    for (const auto& s : result->equities.keys()) {
        print(f, ", '{}'", s);
    }
    println(f, ")");
    println(f, "set(gca, 'xtick', tick_datenums);");
    println(f, "set(gca, 'xticklabel', tick_labels);");
    fclose(f);

    println("==== REPORT ====");
    const auto df = result->local_days.front();
    const auto db = result->local_days.back();
    const auto ymdf = chr::year_month_day(df);
    const auto ymdb = chr::year_month_day(db);
    println("Period: {} .. {}", result->local_days.front(), result->local_days.back());
    println(
      "{} calendar days, {} months, {:.2f} years, {} trading days",
      (db - df).count(),
      (ymdb.year() / ymdb.month() - ymdf.year() / ymdf.month()) / chr::months(1),
      years_between_days(df, db),
      result->total.size()
    );
    println("CAGR: {:.2f}%", 100 * result->cagr());
    auto [max_drawdown, longest_underwater_days] = result->max_drawdown_and_longest_underwater_days();
    println("max drawdown: {:.2f}%, longest underwater: {} days", 100 * max_drawdown, longest_underwater_days);
    println("Num market orders: {}", result->num_market_orders);
    if (const auto N = result->rebalance_day_idcs.size(); N >= 2) {
        const auto avg = ifcast<double>((result->local_days[result->rebalance_day_idcs.back()]
                                         - result->local_days[result->rebalance_day_idcs.front()])
                                          .count())
                       / ifcast<double>(N - 1);
        println("Avg cal. days between rebal.: {:.2f}", avg);
        const auto sh_da = result->sharpe_daily(SharpeAggregation::arithmetic);
        const auto sh_dg = result->sharpe_daily(SharpeAggregation::geometric);
        const auto sh_ma = result->sharpe_through_rebalance_days(SharpeAggregation::arithmetic);
        const auto sh_mg = result->sharpe_through_rebalance_days(SharpeAggregation::geometric);
        println("+---------------+------------+-----------+");
        println("|               | arithmetic | geometric |");
        println("+---------------+------------+-----------+");
        println("| Sharpe daily  | {:>10.2f} | {:>9.2f} |", sh_da, sh_dg);
        println("| Sharpe rebal. | {:>10.2f} | {:>9.2f} |", sh_ma, sh_mg);
        println("+---------------+------------+-----------+");
    }
    // Worst rolling 12-month:
    if (const auto worst_return = result->worst_12_month_return()) {
        println("Worst 12-month return: {:.2f}%", 100 * *worst_return);
    }

    return 0;
}
