#include "BacktestReport.h"

#include "common.h"

#include <meadow/finance.h>

NODIS double BacktestReport::cagr() const
{
    return pow(total.back() / total.front(), 1.0 / years_between_days(local_days.front(), local_days.back())) - 1;
}

NODIS pair<double, int> BacktestReport::max_drawdown_and_longest_underwater_days() const
{
    double running_peak = total.front();
    double max_drawdown = 0;
    int longest_underwater_days = 0;
    int underwater_days = 0;
    for (auto t : total) {
        if (t >= running_peak) {
            running_peak = t;
            underwater_days = 0;
        } else {
            const double drawdown = t / running_peak - 1;
            max_drawdown = std::min(max_drawdown, drawdown);
            longest_underwater_days = std::max(longest_underwater_days, ++underwater_days);
        }
    }
    return {max_drawdown, longest_underwater_days};
}

NODIS double BacktestReport::sharpe_daily(SharpeAggregation aggregation) const
{
    vector<double> asset_return_factor, cash_proxy_return_factor;
    const auto num_periods = signed_subtract(total.size(), 1);
    CHECK(num_periods > 0);
    asset_return_factor.reserve(sucast(num_periods));
    cash_proxy_return_factor.reserve(sucast(num_periods));
    for (size_t i = 1; i < total.size(); i++) {
        asset_return_factor.push_back(total[i] / total[i - 1]);
        cash_proxy_return_factor.push_back(cash_proxy_level[i] / cash_proxy_level[i - 1]);
    }
    const auto periods_per_year =
      ifcast<double>(num_periods) / years_between_days(local_days.front(), local_days.back());
    return sharpe(
      asset_return_factor, cash_proxy_return_factor, periods_per_year, SharpeInputType::return_factor, aggregation
    );
}

NODIS double BacktestReport::sharpe_through_rebalance_days(SharpeAggregation aggregation) const
{
    vector<double> asset_return_factor, cash_proxy_return_factor;
    const auto N = rebalance_day_idcs.size();
    const auto num_periods = signed_subtract(N, 1);
    CHECK(num_periods > 0);
    asset_return_factor.reserve(sucast(num_periods));
    cash_proxy_return_factor.reserve(sucast(num_periods));

    for (size_t ixix = 1; ixix < N; ++ixix) {
        const auto ix = rebalance_day_idcs[ixix];
        const auto prev_ix = rebalance_day_idcs[ixix - 1];
        asset_return_factor.push_back(total[ix] / total[prev_ix]);
        cash_proxy_return_factor.push_back(cash_proxy_level[ix] / cash_proxy_level[prev_ix]);
    }
    const auto periods_per_year =
      ifcast<double>(num_periods)
      / years_between_days(local_days[rebalance_day_idcs.front()], local_days[rebalance_day_idcs.back()]);
    return sharpe(
      asset_return_factor, cash_proxy_return_factor, periods_per_year, SharpeInputType::return_factor, aggregation
    );
}
