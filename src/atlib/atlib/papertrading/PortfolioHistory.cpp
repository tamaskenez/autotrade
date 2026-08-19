#include "PortfolioHistory.h"

#include "atlib/chrono.h"
#include "atlib/papertrading/papertrading.h"

void PortfolioHistory::trading_days_truncate(size_t n)
{
    if (trading_days.size() <= n) {
        return;
    }
    trading_days.resize(n);

    cash.erase_from(n);
    for (auto&& [_, v] : equity_position_values) {
        v.erase_from(n);
    }
}

void PortfolioHistory::make_snapshot(
  chr::local_days date,
  double cash_proxy_level,
  const Portfolio& portfolio,
  const std::flat_map<string, double>& equity_prices
)
{
    const auto ix = trading_days.size();
    cash.insert_at_end(ix, portfolio.cash);
    double total = portfolio.cash;
    for (auto&& [symbol, shares] : portfolio.equities) {
        auto it = equity_prices.find(symbol);
        CHECK(it != equity_prices.end());
        const auto q = shares * it->second;
        auto jt = equity_position_values.find(symbol);

        if (jt == equity_position_values.end()) {
            jt = equity_position_values.emplace(symbol, AppendableIntervals<size_t, double>(0.0)).first;
        }
        jt->second.insert_at_end(ix, q);
        total += q;
    }
    trading_days.push_back(TradingDay{.date = date, .total = total, .cash_proxy_level = cash_proxy_level});
}

vector<double> PortfolioHistory::cash_for_trading_days() const
{
    return cash.get_values_for_key_range(0, trading_days.size());
}

vector<double> PortfolioHistory::total_for_trading_days() const
{
    return trading_days | vi::transform(&TradingDay::total) | ra::to<vector<double>>();
}

vector<double> PortfolioHistory::equity_position_values_for_trading_days(const string& symbol) const
{
    auto it = equity_position_values.find(symbol);
    CHECK(it != equity_position_values.end());
    const auto& v = it->second;
    return v.get_values_for_key_range(0, trading_days.size());
}

double PortfolioHistory::cagr() const
{
    CHECK(!trading_days.empty());
    if (trading_days.size() == 1) {
        return 0.0;
    }
    const auto& f = trading_days.front();
    const auto& b = trading_days.back();
    return pow(b.total / f.total, 1.0 / years_between_days(f.date, b.date)) - 1;
}

pair<double, int> PortfolioHistory::max_drawdown_and_longest_underwater_days() const
{
    if (trading_days.empty()) {
        return {0.0, 0};
    }

    double running_peak = trading_days.front().total;
    double max_drawdown = 0;
    int longest_underwater_days = 0;
    int underwater_days = 0;
    for (auto& td : trading_days) {
        if (td.total >= running_peak) {
            running_peak = td.total;
            underwater_days = 0;
        } else {
            const double drawdown = td.total / running_peak - 1;
            max_drawdown = std::min(max_drawdown, drawdown);
            longest_underwater_days = std::max(longest_underwater_days, ++underwater_days);
        }
    }
    return {max_drawdown, longest_underwater_days};
}

double PortfolioHistory::sharpe_daily(SharpeAggregation aggregation) const
{
    vector<double> asset_return_factor, cash_proxy_return_factor;
    const auto N = trading_days.size();
    const auto num_periods = signed_subtract(N, 1);
    CHECK(num_periods > 0);
    asset_return_factor.reserve(sucast(num_periods));
    cash_proxy_return_factor.reserve(sucast(num_periods));
    for (size_t i = 1; i < N; i++) {
        const auto& tdi = trading_days[i];
        const auto& tdi_prev = trading_days[i - 1];
        asset_return_factor.push_back(tdi.total / tdi_prev.total);
        cash_proxy_return_factor.push_back(tdi.cash_proxy_level / tdi_prev.cash_proxy_level);
    }
    const auto periods_per_year =
      ifcast<double>(num_periods) / years_between_days(trading_days.front().date, trading_days.back().date);
    return sharpe(
      asset_return_factor, cash_proxy_return_factor, periods_per_year, SharpeInputType::return_factor, aggregation
    );
}

double PortfolioHistory::sharpe_through_selected_days(
  SharpeAggregation aggregation, span<const size_t> trading_days_indices
) const
{
    vector<double> asset_return_factor, cash_proxy_return_factor;
    const auto N = trading_days_indices.size();
    const auto num_periods = signed_subtract(N, 1);
    CHECK(num_periods > 0);
    asset_return_factor.reserve(sucast(num_periods));
    cash_proxy_return_factor.reserve(sucast(num_periods));

    for (size_t ixix = 1; ixix < N; ++ixix) {
        const auto ix = trading_days_indices[ixix];
        const auto prev_ix = trading_days_indices[ixix - 1];
        CHECK(ix < trading_days.size() && prev_ix < trading_days.size());
        const auto& tdix = trading_days[ix];
        const auto& tdix_prev = trading_days[prev_ix];
        asset_return_factor.push_back(tdix.total / tdix_prev.total);
        cash_proxy_return_factor.push_back(tdix.cash_proxy_level / tdix_prev.cash_proxy_level);
    }
    const auto periods_per_year =
      ifcast<double>(num_periods)
      / years_between_days(
        trading_days[trading_days_indices.front()].date, trading_days[trading_days_indices.back()].date
      );
    return sharpe(
      asset_return_factor, cash_proxy_return_factor, periods_per_year, SharpeInputType::return_factor, aggregation
    );
}

optional<double> PortfolioHistory::worst_12_month_return() const
{
    const auto N = trading_days.size();
    optional<double> worst_return;
    for (size_t i = 0, j = 0; i < N; ++i) {
        const auto& tdi = trading_days[i];
        const auto di = tdi.date;
        while (j < N && !is_on_or_later_than_next_year_same_date(di, trading_days[j].date)) {
            ++j;
        }
        if (j >= N) {
            break;
        }
        const auto& tdj = trading_days[j];
        const auto return_ = pow(tdj.total / tdi.total, 1.0 / years_between_days(di, tdj.date)) - 1;
        worst_return = worst_return ? std::min(*worst_return, return_) : return_;
    }
    return worst_return;
}
