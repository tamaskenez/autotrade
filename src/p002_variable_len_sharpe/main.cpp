#include "Trader.h"

#include "atlib/chrono.h"
#include "atlib/marketdata/MarketData.h"
#include "atlib/matlab_export.h"
#include "atlib/papertrading/PortfolioHistory.h"

#include <meadow/file.h>
#include <meadow/finance.h>
#include <meadow/math.h>

using namespace std::chrono_literals;

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
    return {MOVE(local_days), MOVE(prices)};
}

constexpr double k_initial_capital = 100000;

namespace
{
expected<void, string> run()
{
    const auto provider = Provider::tiingo;
    const auto mdcfg = MarketDataConfig{.workspace_dir = WORKSPACE_DIR};
    const auto as_of = chr::local_days(2026y / chr::January / 1d);
    const string k_symbol = "SPY";
    const auto start_date = chr::local_days(2007y / chr::January / 1d);
    const auto end_date = chr::local_days(2021y / chr::January / 1d);

    // Get adjusted price history.
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(auto eh, equity_history(mdcfg, provider, k_symbol, as_of));
    eh.adjust();
    auto [local_days, prices] = extract_local_days_and_prices(eh);

    // Write out the history to MATLAB file.
    string s;
    s += matlab_export::assign_date_axis("days", local_days);
    s += matlab_export::assign_row_vector("close", prices);
    s += "subplot(2, 1, 1);\n";
    s += "plot(days, log(close)/log(2^(1/12))), grid;\n";
    s += matlab_export::set_ticks_labels("days");

    const auto N = local_days.size();
    CHECK(prices.size() == N);

    CHECK(!local_days.empty());

    // Create daily return factors of the equity.
    vector<double> return_factors; // Of the equity `k_symbol`.
    return_factors.reserve(N - 1);
    for (size_t i = 1; i < N; i++) {
        return_factors.push_back(prices[i] / prices[i - 1]);
    }
    constexpr int duration_step = 40;
    constexpr int min_duration = 40;
    constexpr int max_duration = 320;
    const auto durations = regspace(min_duration, duration_step, max_duration);

    // Actual trading starts on start_date + chr::days(1) to allow a cash-only snapshot to be made on start_date.
    const auto jt_begin = ra::lower_bound(local_days, start_date + chr::days(1));
    CHECK(jt_begin < local_days.end());
    const auto jt_end = ra::lower_bound(local_days, end_date);
    CHECK(jt_end <= local_days.end());

    // Create traders, one for each duration.
    auto traders = vector<Trader>(durations.size(), Trader(k_symbol, k_initial_capital, start_date));

    // For all traders (durations) go through start_date..end_date and make a buy/sell decision based on current
    // Sharpe ratio.
    //
    // `sharpes`[d][ix] will be the duration[d] sharpe ratio at trading day `ix`
    auto sharpes = vector<vector<double>>(durations.size());
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
            // We're looking at the period of trading days local_days[ix] to local_days[jx] (inclusive).
            // The return factor local_days[ix] to local_days[ix + 1] is stored in return_factors[ix].
            // The return factor local_days[jx - 1] to local_days[jx] is stored in return_factors[jx - 1].
            // Thus we need the range of return factors return_factors[ix] .. return_factors[jx - 1] (inclusive)
            assert(cmp_less_equal(jx, return_factors.size()));
            const auto sr = sharpe(
              span(return_factors.data() + ix, return_factors.data() + jx),
              1.0,
              duration / ybd,
              SharpeInputType::return_factor,
              SharpeAggregation::geometric
            );
            v.push_back(sr);
            asset_prices[k_symbol] = prices[sucast(jx)];
            TRY_OR_RETURN_UNEXPECTED(traders[duration_ix].advance(*jt, sr, asset_prices));
        }
    }
    const auto sharpe_days = vector(jt_begin, jt_end);
    s += matlab_export::assign_date_axis("sharpe_days", sharpe_days);
    s += matlab_export::assign_matrix("sharpes", sharpes);
    s += "subplot(2, 1, 2);\n";
    s += "plot(sharpe_days', sharpes'), grid;\n";
    s += matlab_export::set_ticks_labels("sharpe_days");

    CHECK(write_string_to_file(s, "/tmp/p2.m"));

    for (size_t i = 0; i < durations.size(); ++i) {
        auto& t = traders[i];
        println("trader dur = {}, return: {:.2f}%, {} orders", durations[i], 100 * t.ph.cagr(), t.ph.num_market_orders);
    }
    return {};
}
} // namespace

int main()
{
    auto r = run();
    LOG_IF(FATAL, !r) << r.error();
    return EXIT_SUCCESS;
}
