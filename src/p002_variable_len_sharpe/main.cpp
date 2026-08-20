#include "Trader.h"

#include "atlib/chrono.h"
#include "atlib/marketdata/MarketData.h"
#include "atlib/matlab_export.h"
#include "atlib/papertrading/PortfolioHistory.h"

#include <meadow/file.h>
#include <meadow/finance.h>
#include <meadow/math.h>

using namespace std::chrono_literals;

struct OHLCVVectors {
    vector<double> open, high, low, close, volume;
    void reserve(size_t n)
    {
        open.reserve(n);
        high.reserve(n);
        low.reserve(n);
        close.reserve(n);
        volume.reserve(n);
    }
    void push_back(const DailyBar& b)
    {
        open.push_back(b.open);
        high.push_back(b.high);
        low.push_back(b.low);
        close.push_back(b.close);
        volume.push_back(b.volume);
    }
};

static pair<vector<chr::local_days>, OHLCVVectors> extract_local_days_and_prices(const EquityHistory& eh)
{
    const auto N = eh.bars.size();
    vector<chr::local_days> local_days;
    OHLCVVectors prices;
    local_days.reserve(N);
    prices.reserve(N);
    for (auto& b : eh.bars) {
        local_days.push_back(b.date);
        prices.push_back(b);
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
    auto [local_days, ohlcvvs] = extract_local_days_and_prices(eh);

    const auto& prices = ohlcvvs.close;
    // Write out the history to MATLAB file.
    string s;
    s += matlab_export::assign_date_axis("days", local_days);
    s += matlab_export::assign_column_vector("db_o", ohlcvvs.open);
    s += matlab_export::assign_column_vector("db_h", ohlcvvs.high);
    s += matlab_export::assign_column_vector("db_l", ohlcvvs.low);
    s += matlab_export::assign_column_vector("db_c", ohlcvvs.close);
    s += matlab_export::assign_column_vector("db_v", ohlcvvs.volume);
    s += "subplot(2, 1, 1);\n";
    s += "l12 = log(2^(1/12));\n";
    // o, c, o, c, ...
    s += "db_oc = [db_o db_c]';\n";
    s += "db_oc = db_oc(:);\n";
    // o, h, c, o, h, c, ...
    s += "db_ohc = [db_o db_h db_c]';\n";
    s += "db_ohc = db_ohc(:);\n";
    // o, l, c, o, l, c, ...
    s += "db_olc = [db_o db_l db_c]';\n";
    s += "db_olc = db_olc(:);\n";

    s += "dd = [days days + 0.7]';\n";
    s += "dd = dd(:);\n";
    s += "ddd = [days days + 0.35 days + 0.7]';\n";
    s += "ddd = ddd(:);\n";

    s += "plot(dd, log(db_oc)/l12, ddd, log(db_ohc)/l12, ddd, log(db_olc)/l12), grid;\n";
    s += matlab_export::set_ticks_labels("days");

    const auto N = local_days.size();
    CHECK(prices.size() == N);

    CHECK(!local_days.empty());

    // Create daily return factors of the equity.
    vector<double> asset_return_factors; // Of the equity `k_symbol`.
    asset_return_factors.reserve(N - 1);
    for (size_t i = 1; i < N; i++) {
        asset_return_factors.push_back(prices[i] / prices[i - 1]);
    }
    constexpr int duration_step = 40;
    constexpr int min_duration = 40;
    constexpr int max_duration = 320;
    const auto durations = regspace(min_duration, duration_step, max_duration);

    // Actual trading starts on start_date + chr::days(1) to allow a cash-only snapshot to be made on start_date.
    const auto cash_day_before_jt_begin = ra::lower_bound(local_days, start_date);
    CHECK(cash_day_before_jt_begin < local_days.end());
    const auto jt_begin = std::next(cash_day_before_jt_begin);
    CHECK(cash_day_before_jt_begin < local_days.end());
    const auto jt_end = ra::lower_bound(local_days, end_date);
    CHECK(jt_end <= local_days.end());

    // Create traders, one for each duration.
    auto traders = vector<Trader>(durations.size(), Trader(k_symbol, k_initial_capital, *cash_day_before_jt_begin));

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
            assert(cmp_less_equal(jx, asset_return_factors.size()));
            const auto sr = sharpe(
              span(asset_return_factors.data() + ix, asset_return_factors.data() + jx),
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

    const auto j0 = sucast(cash_day_before_jt_begin - local_days.begin());
    const auto j1 = sucast(jt_end - local_days.begin());
    const auto d0 = local_days[j0];
    const auto period_asset_return_factors =
      span(asset_return_factors.begin() + uscast(j0), asset_return_factors.begin() + signed_subtract(j1, 1));
    for (size_t i = 0; i < durations.size(); ++i) {
        auto& t = traders[i];
        CHECK(d0 == t.ph.trading_days.front().date);
        const auto return_factors = t.ph.return_factors_for_trading_days();
        const auto periods_per_year = ifcast<double>(return_factors.size())
                                    / years_between_days(t.ph.trading_days.front().date, t.ph.trading_days.back().date);
        const auto raw_sharpe =
          sharpe(return_factors, 1.0, periods_per_year, SharpeInputType::return_factor, SharpeAggregation::geometric);
        const auto sharpe_to_asset = sharpe(
          return_factors,
          period_asset_return_factors,
          periods_per_year,
          SharpeInputType::return_factor,
          SharpeAggregation::geometric
        );
        println(
          "trader dur = {}, return: {:.2f}%, sharpe: {:.2f}, to asset: {:.2f} from {} orders",
          durations[i],
          100 * t.ph.cagr(),
          raw_sharpe,
          sharpe_to_asset,
          t.ph.num_market_orders
        );
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
