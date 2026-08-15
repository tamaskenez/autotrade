#include "Backtest.h"
#include "meadow/matlab.h"

int main()
{
    using namespace std::chrono_literals;
    const auto bc = BacktestConfig{
      .provider = Provider::tiingo, .start_date = 2004y / chr::January / 1d, .end_date = 2014y / chr::December / 31d
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
    return 0;
}
