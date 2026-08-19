#include "Trader.h"

namespace
{
constexpr auto k_broker_cost_scheme = BrokerCostScheme::flat_10bp;
}

Trader::Trader(string symbol_arg, double start_capital, chr::local_days start_date)
    : symbol(MOVE(symbol_arg))
{
    portfolio.cash = start_capital;
    ph.make_snapshot(start_date, 1.0, portfolio, {});
}

expected<void, string>
Trader::advance(chr::local_days date, double sharpe, const std::flat_map<string, double>& asset_prices)
{
    const bool buy_signal = sharpe > 0.1;
    const bool sell_signal = sharpe < -2;
    optional<vector<string>> desired_portfolio;
    const auto q = portfolio.get_equity_amount(symbol);
    if (q == 0.0 && buy_signal) {
        desired_portfolio = vector<string>({symbol});
    } else if (q > 0 && sell_signal) {
        desired_portfolio = vector<string>();
    }
    if (desired_portfolio) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto market_orders,
          market_orders_from_portfolio_change(k_broker_cost_scheme, asset_prices, portfolio, *desired_portfolio)
        );
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          auto response, apply_market_orders(k_broker_cost_scheme, asset_prices, portfolio, market_orders)
        );
        portfolio = MOVE(response.portfolio);
        ph.num_market_orders += iicast<int>(market_orders.size());
    }
    ph.make_snapshot(date, 1.0, portfolio, asset_prices);
    return {};
}
