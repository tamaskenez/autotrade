#include "GridReport.h"

#include <magic_enum/magic_enum.hpp>

namespace
{
using RebalanceDay = dual_mom_fixed_etf_algorithm::RebalanceDay;

string toupper(string_view s)
{
    string result(s);
    for (char& c : result) {
        c = static_cast<char>(std::toupper(c));
    }
    return result;
}

// Print a 4x4 ascii table:
// - print `top_left` in the top-left cell
// - the remaining 3 cells of the first row are: "look6", "look9", "look12"
// - the first cells of the 3 bottom rows are: "month-end", "day-10", "day-15"
// - fill the remaining bottom-right 3x3 part with the items from `xs`, row-major, 2 decimal digits precision
void print_report_table(string_view top_left, array<double, 9> xs)
{
    constexpr string_view k_separator = "+-----------+--------+--------+--------+";
    constexpr array<string_view, 3> k_row_labels = {"month-end", "day-10", "day-15"};

    println("{}", k_separator);
    println("| {:<9} | {:>6} | {:>6} | {:>6} |", top_left, "look6", "look9", "look12");
    println("{}", k_separator);
    for (size_t row = 0; row < k_row_labels.size(); ++row) {
        println(
          "| {:<9} | {:>6.2f} | {:>6.2f} | {:>6.2f} |", k_row_labels[row], xs[3 * row], xs[3 * row + 1], xs[3 * row + 2]
        );
    }
    println("{}", k_separator);
}

enum class Value {
    cagr,
    max_drawdown,
    month_end_sharpe,
    worst_rolling_12,
    trade_count
};

const char* top_left(Value v)
{
    switch (v) {
    case Value::cagr:
        return "CAGR (%)";
    case Value::max_drawdown:
        return "MaxDD";
    case Value::month_end_sharpe:
        return "Sharpe";
    case Value::worst_rolling_12:
        return "Worst12m";
    case Value::trade_count:
        return "Trades";
    }
}
} // namespace

void print_grid_report(const vector<UniverseResult>& urs)
{
    for (const auto& ur : urs) {
        println("Universe: {}", toupper(magic_enum::enum_name(ur.universe)));
        for (const auto ev : magic_enum::enum_values<Value>()) {
            array<double, 9> xs;
            size_t i = 0;
            for (const auto timing :
                 {RebalanceDay::last_trading_day_of_month, RebalanceDay::month_10th, RebalanceDay::month_15th}) {
                for (const auto lookback : {chr::months(6), chr::months(9), chr::months(12)}) {
                    const auto key = GridKey{.timing = timing, .lookback = lookback};
                    auto it = ra::find(ur.cells, key, &pair<GridKey, BacktestReport>::first);
                    CHECK(it != ur.cells.end());
                    double v = NAN;
                    const auto& ph = it->second.portfolio_history;
                    switch (ev) {
                    case Value::cagr:
                        v = 100 * ph.cagr();
                        break;
                    case Value::max_drawdown:
                        v = 100 * ph.max_drawdown_and_longest_underwater_days().first;
                        break;
                    case Value::month_end_sharpe:
                        // Calculated across last days of months.
                        v = ph.sharpe_through_selected_days(SharpeAggregation::arithmetic, ph.monthly_sharpe_indices())
                              .value_or(NAN);
                        break;
                    case Value::worst_rolling_12:
                        v = 100 * ph.worst_12_month_return().value_or(NAN);
                        break;
                    case Value::trade_count:
                        v = ph.num_market_orders;
                        break;
                    }
                    xs[i++] = v;
                }
            }
            print_report_table(top_left(ev), xs);
        }
        println();
    }
}

namespace
{
const char* timing_for_csv(RebalanceDay timing)
{
    switch (timing) {
    case RebalanceDay::first_trading_day_of_month:
        return "day1";
    case RebalanceDay::last_trading_day_of_month:
        return "month_end";
    case RebalanceDay::first_trading_day_of_week:
        return "week1";
    case RebalanceDay::last_trading_day_of_week:
        return "week_end";
    case RebalanceDay::month_10th:
        return "day10";
    case RebalanceDay::month_15th:
        return "day15";
    }
    std::unreachable();
}
} // namespace

void print_csv_report_line(string_view universe, string_view lookback, RebalanceDay timing, const BacktestReport& br)
{
    const auto& ph = br.portfolio_history;
    println(
      "{},{},{},{:.2f},{:.2f},{:.2f},{:.2f},{},{}",
      universe,
      lookback,
      timing_for_csv(timing),
      100 * ph.cagr(),
      100 * ph.max_drawdown_and_longest_underwater_days().first,
      ph.sharpe_through_selected_days(SharpeAggregation::arithmetic, ph.monthly_sharpe_indices()).value_or(NAN),
      100 * ph.worst_12_month_return().value_or(NAN),
      ph.num_market_orders,
      ph.num_portfolio_changes
    );
}

void print_grid_report_as_csv(const vector<UniverseResult>& urs)
{
    println("universe,lookback,timing,cagr,maxdd,sharpe,worst12m,trades,switches");
    for (const auto& ur : urs) {
        const auto universe = toupper(magic_enum::enum_name(ur.universe));
        for (const auto lookback : {chr::months(6), chr::months(9), chr::months(12)}) {
            for (const auto timing :
                 {RebalanceDay::last_trading_day_of_month, RebalanceDay::month_10th, RebalanceDay::month_15th}) {
                const auto key = GridKey{.timing = timing, .lookback = lookback};
                auto it = ra::find(ur.cells, key, &pair<GridKey, BacktestReport>::first);
                CHECK(it != ur.cells.end());
                print_csv_report_line(universe, std::to_string(lookback.count()), timing, it->second);
            }
        }
    }
}
