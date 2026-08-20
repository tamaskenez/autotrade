// Compares an ETF against the mutual fund used to extend its history backwards, month by month over the beginning of
// their common history, where the two must agree if the splice is to be trusted.

#include "atlib/marketdata/MarketData.h"

#include <meadow/finance.h>

using namespace std::chrono_literals;

namespace
{
// The payload cache reaches past this date for every symbol below, so a run needs no download.
constexpr auto k_as_of = chr::local_days(2026y / chr::August / 1d);

// The last month reported on, when the overlap reaches that far.
constexpr auto k_last_month = 2025y / chr::December;

struct MonthlyStats {
    ptrdiff_t num_returns;
    double return_;      // Over the whole month, 0.01 is +1%.
    double daily_sharpe; // NaN when the month has fewer than two daily returns.
};

// Daily total-return factors of `bars` within `month`, anchored on the last bar before `month` when there is one, so
// that the first day of the month contributes its return as well.
vector<double> month_return_factors(const vector<DailyBar>& bars, chr::year_month month)
{
    const auto month_first_day = chr::local_days(month / 1d);
    const auto month_last_day = chr::local_days(month / chr::last);
    const auto end = ra::upper_bound(bars, month_last_day, {}, &DailyBar::date);
    auto it = ra::lower_bound(bars, month_first_day, {}, &DailyBar::date);
    if (it != bars.begin()) {
        --it;
    }
    vector<double> factors;
    for (auto next = std::next(it); next < end; it = next++) {
        factors.push_back(next->close / it->close);
    }
    return factors;
}

MonthlyStats monthly_stats(const vector<DailyBar>& bars, chr::year_month month)
{
    const auto factors = month_return_factors(bars, month);
    auto stats = MonthlyStats{.num_returns = uscast(factors.size()), .return_ = 0.0, .daily_sharpe = 0.0};
    stats.return_ = std::accumulate(BEGIN_END(factors), 1.0, std::multiplies<>()) - 1.0;
    stats.daily_sharpe = factors.size() >= 2
                         ? sharpe(factors, 1.0, 1.0, SharpeInputType::return_factor, SharpeAggregation::geometric)
                         : std::numeric_limits<double>::quiet_NaN();
    return stats;
}

chr::year_month year_month_of(chr::local_days d)
{
    const auto ymd = chr::year_month_day(d);
    return ymd.year() / ymd.month();
}

string format_sharpe(double s)
{
    return std::isnan(s) ? "-" : format("{:.3f}", s);
}

double mean_of(span<const double> v)
{
    CHECK(!v.empty());
    return std::accumulate(BEGIN_END(v), 0.0) / ifcast<double>(v.size());
}

// Sample covariance, normalized by size() - 1.
double covariance(span<const double> x, span<const double> y)
{
    CHECK(x.size() == y.size() && x.size() >= 2);
    const auto mx = mean_of(x);
    const auto my = mean_of(y);
    double sum = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        sum += (x[i] - mx) * (y[i] - my);
    }
    return sum / ifcast<double>(x.size() - 1);
}

double compound_return(span<const double> returns)
{
    return std::accumulate(
             BEGIN_END(returns),
             1.0,
             [](double acc, double r) {
                 return acc * (1 + r);
             }
           )
         - 1;
}

// How the proxy's monthly returns relate to the equity's over the reported period. The equity is the reference in
// every ratio: what the proxy has to reproduce for the spliced history to stand in for it.
struct SpliceStats {
    double correlation;
    double mean_diff_annualized; // mean(proxy - equity) per month, times 12.
    double vol_ratio;            // std(proxy) / std(equity).
    double beta;                 // cov(proxy, equity) / var(equity).
    double worst_month;          // The signed proxy - equity difference of the month where it was largest in absolute
                                 // value.
    chr::year_month worst_month_date;
    double equity_total_return, proxy_total_return;
};

SpliceStats splice_stats(span<const double> e, span<const double> m, chr::year_month first_month)
{
    CHECK(e.size() == m.size() && e.size() >= 2);
    vector<double> diff;
    diff.reserve(e.size());
    for (size_t i = 0; i < e.size(); ++i) {
        diff.push_back(m[i] - e[i]);
    }
    const auto worst = ra::max_element(diff, {}, [](double d) {
        return std::abs(d);
    });
    const auto var_e = covariance(e, e);
    return SpliceStats{
      .correlation = covariance(m, e) / std::sqrt(covariance(m, m) * var_e),
      .mean_diff_annualized = 12 * mean_of(diff),
      .vol_ratio = std::sqrt(covariance(m, m) / var_e),
      .beta = covariance(m, e) / var_e,
      .worst_month = *worst,
      .worst_month_date = first_month + chr::months(worst - diff.begin()),
      .equity_total_return = compound_return(e),
      .proxy_total_return = compound_return(m)
    };
}

expected<SpliceStats, string>
validate_splice(const MarketDataConfig& config, Provider provider, string_view symbol, string_view proxy_symbol)
{
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(auto eh, equity_history(config, provider, symbol, k_as_of));
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(auto proxy_eh, equity_history(config, provider, proxy_symbol, k_as_of));
    if (eh.bars.empty() || proxy_eh.bars.empty()) {
        return unexpected(format("{} or {} has no daily bars.", symbol, proxy_symbol));
    }
    // Compare what a holder earned, not what the price did: for a bond fund the distributions are most of the return.
    eh.adjust();
    proxy_eh.adjust();

    const auto overlap_first_day = std::max(eh.bars.front().date, proxy_eh.bars.front().date);
    const auto overlap_last_day = std::min(eh.bars.back().date, proxy_eh.bars.back().date);
    if (overlap_last_day < overlap_first_day) {
        return unexpected(format("{} and {} have no overlapping history.", symbol, proxy_symbol));
    }
    // The month the overlap starts in is left out: the equity's first bar falls inside it, so its return would cover
    // part of the month against the proxy's whole one.
    const auto first_month = year_month_of(overlap_first_day) + chr::months(1);
    const auto last_month = std::min(year_month_of(overlap_last_day), k_last_month);
    if (last_month < first_month + chr::months(1)) {
        return unexpected(format("{} and {} overlap over fewer than two full months.", symbol, proxy_symbol));
    }

    println(
      "\n{} vs {}: overlap {:%F} .. {:%F}, {} months reported",
      symbol,
      proxy_symbol,
      overlap_first_day,
      overlap_last_day,
      (last_month - first_month).count() + 1
    );
    println(
      "{:>7} | {:>4} {:>8} {:>7} | {:>4} {:>8} {:>7} | {:>8}",
      "month",
      "days",
      symbol,
      "sharpe",
      "days",
      proxy_symbol,
      "sharpe",
      "diff"
    );
    vector<double> e, m; // Monthly returns of the equity and of the proxy mutual fund.
    for (auto month = first_month; month <= last_month; month += chr::months(1)) {
        const auto s = monthly_stats(eh.bars, month);
        const auto ps = monthly_stats(proxy_eh.bars, month);
        e.push_back(s.return_);
        m.push_back(ps.return_);
        println(
          "{:%Y-%m} | {:>4} {:>7.2f}% {:>7} | {:>4} {:>7.2f}% {:>7} | {:>7.2f}%",
          month,
          s.num_returns,
          100 * s.return_,
          format_sharpe(s.daily_sharpe),
          ps.num_returns,
          100 * ps.return_,
          format_sharpe(ps.daily_sharpe),
          100 * (s.return_ - ps.return_)
        );
    }
    return splice_stats(e, m, first_month);
}

void print_stats_header()
{
    println(
      "\n{:>13} | {:>7} | {:>13} | {:>9} | {:>6} | {:>11} | {:>17} | {:>13} | {:>12}",
      "pair",
      "corr",
      "mean_diff_ann",
      "vol_ratio",
      "beta",
      "worst_month",
      "worst_month_date",
      "equity_total",
      "proxy_total"
    );
}

void print_stats_row(string_view symbol, string_view proxy_symbol, const SpliceStats& s)
{
    println(
      "{:>13} | {:>7.4f} | {:>12.2f}% | {:>9.3f} | {:>6.3f} | {:>10.2f}% | {:>17} | {:>12.1f}% | {:>11.1f}%",
      format("{}/{}", symbol, proxy_symbol),
      s.correlation,
      100 * s.mean_diff_annualized,
      s.vol_ratio,
      s.beta,
      100 * s.worst_month,
      format("{:%Y-%m}", s.worst_month_date),
      100 * s.equity_total_return,
      100 * s.proxy_total_return
    );
}
} // namespace

int main()
{
    const auto config = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    const auto ief = TRY_OR_FAIL(validate_splice(config, Provider::tiingo, "IEF", "VFITX"));
    const auto efa = TRY_OR_FAIL(validate_splice(config, Provider::tiingo, "EFA", "VGTSX"));
    print_stats_header();
    print_stats_row("IEF", "VFITX", ief);
    print_stats_row("EFA", "VGTSX", efa);
    return EXIT_SUCCESS;
}
