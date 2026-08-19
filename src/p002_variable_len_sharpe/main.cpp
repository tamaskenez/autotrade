#include <meadow/matlab.h>

#include "../p001_dual_mom_fixed_etf/common.h"
#include "atlib/marketdata/MarketData.h"
#include "atlib/matlab_export.h"
#include "atlib/papertrading/papertrading.h"
#include "meadow/file.h"
#include "meadow/finance.h"
#include "meadow/math.h"

using namespace std::chrono_literals;
using namespace std::string_view_literals;

static pair<vector<chr::local_days>, vector<double>> extract_local_days_and_prices(const EquityHistory& eh)
{
    const auto N = eh.bars.size();
    vector<chr::local_days> local_days;
    vector<double> prices;
    local_days.reserve(N);
    prices.reserve(N);
    for (auto& b : eh.bars) {
        local_days.push_back(b.date);
        prices.push_back(b.close);
    }
    return {local_days, prices};
}

constexpr auto k_broker_cost_scheme = BrokerCostScheme::flat_10bp;
constexpr double k_initial_capital = 100000;

class Trader
{
public:
    Trader()
    {
        portfolio.cash = k_initial_capital;
    }
    NODIS expected<void, string> advance(double sharpe, const std::flat_map<string, double>& asset_prices)
    {
        const bool buy_signal = sharpe > 1;
        const bool sell_signal = sharpe < -1;
        optional<vector<string>> desired_portfolio;
        if (portfolio.equities.empty() && buy_signal) {
            desired_portfolio = vector<string>({"SPY"});
        } else if (!portfolio.equities.empty() && sell_signal) {
            desired_portfolio = vector<string>();
        }
        if (desired_portfolio) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(
              const auto market_orders,
              market_orders_from_portfolio_change(k_broker_cost_scheme, asset_prices, portfolio, *desired_portfolio)
            );
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(
              const auto& response, apply_market_orders(k_broker_cost_scheme, asset_prices, portfolio, market_orders)
            );
            portfolio = response.portfolio;
            portfolio.clear_below(1e-6);
        }
        return {};
    }

    Portfolio portfolio;
};

static expected<void, string> run()
{
    const auto provider = Provider::tiingo;
    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    auto market_data = MarketData(mdcfg, provider);
    const auto as_of = chr::local_days(2026y / chr::January / 1d);
    const string k_symbol = "SPY";

    TRY_ASSIGN_OR_RETURN_UNEXPECTED(auto eh, equity_history(mdcfg, provider, k_symbol, as_of));
    eh.adjust();

    auto [local_days, prices] = extract_local_days_and_prices(eh);

    string s;
    s += matlab_export::assign_date_axis("days", local_days);
    s += matlab_export::assign_row_vector("close", prices);
    s += "subplot(2, 1, 1);\n";
    s += format("plot(days, log(close)/log(1.1)), grid;\n");
    s += matlab_export::set_ticks_labels("days");

    const auto N = local_days.size();
    CHECK(prices.size() == N);

    CHECK(!local_days.empty());

    vector<double> return_factors;
    return_factors.reserve(N - 1);
    for (size_t i = 1; i < N; i++) {
        return_factors.push_back(prices[i] / prices[i - 1]);
    }
    constexpr int duration_step = 40;
    constexpr int min_duration = 40;
    constexpr int max_duration = 320;
    const auto durations = regspace(min_duration, duration_step, max_duration);

    const auto jt_begin = ra::lower_bound(local_days, chr::local_days(2007y / chr::January / 1d));
    CHECK(jt_begin < local_days.end());
    const auto jt_end = ra::lower_bound(local_days, chr::local_days(2021y / chr::January / 1d));
    CHECK(jt_end <= local_days.end());

    vector<Trader> traders(durations.size());

    vector<vector<double>> sharpes(durations.size());
    std::flat_map<string, double> asset_prices;
    for (size_t duration_ix = 0; duration_ix < durations.size(); ++duration_ix) {
        const auto duration = durations[duration_ix];
        auto& v = sharpes[duration_ix];
        v.reserve(sucast(jt_end - jt_begin));
        for (auto jt = jt_begin; jt != jt_end; ++jt) {
            const auto jx = jt - local_days.begin();
            // local_days[jx], for example, if jx = 5, duration = 5
            // we need the days[0, 1, 2, 3, 4, 5] which went to
            // return_factors[0, 1, 2, 3, 4]
            assert(jx >= duration);
            const auto ix = jx - duration;
            // Go back from day to duration and calculate sharpe
            const auto ybd = years_between_days(*(jt - duration), *jt);
            assert(cmp_less(jx, return_factors.size()));
            const auto sr = sharpe(
              span(return_factors.data() + ix, return_factors.data() + jx),
              1.0,
              duration / ybd,
              SharpeInputType::return_factor,
              SharpeAggregation::geometric
            );
            v.push_back(sr);
            asset_prices[k_symbol] = prices[sucast(jx)];
            TRY_OR_RETURN_UNEXPECTED(traders[duration_ix].advance(sr, asset_prices));
        }
    }
    const auto sharpe_days = vector(jt_begin, jt_end);
    s += matlab_export::assign_date_axis("sharpe_days", sharpe_days);
    s += matlab_export::assign_matrix("sharpes", sharpes);
    s += "subplot(2, 1, 2);\n";
    s += format("plot(sharpe_days', sharpes'), grid;\n");
    s += matlab_export::set_ticks_labels("sharpe_days");

    CHECK(write_string_to_file(s, "/tmp/p2.m"));

    asset_prices[k_symbol] = prices[sucast(jt_end - local_days.begin())];
    for (size_t i = 0; i < durations.size(); ++i) {
        auto& t = traders[i];
        println(
          "trader dur = {}, return: {:.2f}%",
          durations[i],
          pow(t.portfolio.total(asset_prices) / k_initial_capital, 1.0 / years_between_days(*jt_begin, *jt_end)) - 1
        );
    }
    return {};
}

int main()
{
    auto r = run();
    LOG_IF(FATAL, !r) << r.error();
    return EXIT_SUCCESS;
}
