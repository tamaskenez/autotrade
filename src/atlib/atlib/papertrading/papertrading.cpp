#include "papertrading.h"

#include "atlib/marketdata/MarketData.h"

#include <meadow/math.h>

namespace
{
struct BrokerCostConfig {
    double flat_cost_factor;
};

// A switch rather than a table indexed by the enum, for the reason info() gives
// in MarketData.cpp: a table is positional, so a scheme inserted above another
// silently hands both of them the wrong rate, while a case missing from here is
// a diagnostic.
BrokerCostConfig config(BrokerCostScheme scheme)
{
    switch (scheme) {
    case BrokerCostScheme::flat_10bp:
        return {.flat_cost_factor = 0.001};
    case BrokerCostScheme::flat_20bp:
        return {.flat_cost_factor = 0.002};
    case BrokerCostScheme::flat_100bp:
        return {.flat_cost_factor = 0.01};
    }
    std::unreachable();
}
} // namespace

double Portfolio::get_equity_amount(const string& symbol) const
{
    const auto it = equities.find(symbol);
    if (it == equities.end()) {
        return 0.0;
    }
    return it->second;
}

double estimate_net_income_from_selling_equity_by_shares(BrokerCostScheme scheme, double num_shares, double price_quote)
{
    // A sale of a negative quantity is a buy the caller has lost track of, and
    // the arithmetic below would answer it with a plausible negative number.
    CHECK(num_shares >= 0);
    CHECK(price_quote > 0);

    const auto c = config(scheme);
    // num_shares sold at quote, then subtract the flat cost.
    return num_shares * price_quote * (1 - c.flat_cost_factor);
}

double estimate_trade_value_when_selling_equity_and_getting_net_income(BrokerCostScheme scheme, double net_income)
{
    CHECK(net_income >= 0);

    const auto c = config(scheme);
    return net_income / (1 - c.flat_cost_factor);
}

double estimate_trade_value_when_buying_equity_for_cash_order(BrokerCostScheme scheme, double cash)
{
    CHECK(cash >= 0);

    const auto c = config(scheme);
    return cash / (1 + c.flat_cost_factor);
}

double
estimate_total_cash_needed_when_buying_equity_for_shares(BrokerCostScheme scheme, double num_shares, double price_quote)
{
    CHECK(num_shares >= 0);
    CHECK(price_quote > 0);

    const auto c = config(scheme);
    return (1 + c.flat_cost_factor) * num_shares * price_quote;
}

expected<vector<MarketOrder>, string> market_orders_from_portfolio_change(
  BrokerCostScheme broker_scheme,
  MarketData& market_data,
  const Portfolio& current_portfolio,
  const vector<string>& desired_portfolio,
  chr::local_days past_trading_day
)
{
    auto result = std::flat_map<string, double>();
    for (const auto& s : current_portfolio.equities.keys()) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& daily_bar, market_data.daily_bar(s, past_trading_day));
        result[s] = daily_bar.close;
    }
    for (const auto& s : desired_portfolio) {
        if (!result.contains(s)) {
            TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& daily_bar, market_data.daily_bar(s, past_trading_day));
            result[s] = daily_bar.close;
        }
    }
    return market_orders_from_portfolio_change(broker_scheme, result, current_portfolio, desired_portfolio);
}

namespace
{
expected<double, string> asset_price_lookup(const std::flat_map<string, double>& asset_prices, const string& symbol)
{
    const auto it = asset_prices.find(symbol);
    if (it == asset_prices.end()) {
        return unexpected(format("No asset_price for {}", symbol));
    }
    if (!isfinite(it->second) || it->second <= 0) {
        return unexpected(format("Invalid asset_price for {}: {}", symbol, it->second));
    }
    return it->second;
}
} // namespace

expected<vector<MarketOrder>, string> market_orders_from_portfolio_change(
  BrokerCostScheme broker_scheme,
  const std::flat_map<string, double>& asset_prices,
  const Portfolio& current_portfolio,
  const vector<string>& desired_portfolio
)
{
    // Determine the 3 sets of equities, (A - B), (B - A) and (A intersection B).
    // Note that the assets_to_keep will contain equities that might need to be bought or sold
    // to maintain equal proportions.
    vector<string> assets_to_buy, assets_to_sell, assets_to_keep;
    for (auto&& [symbol, q] : current_portfolio.equities) {
        if (q == 0) {
            continue;
        }
        CHECK(q > 0); // Short positions are not handled for now.
        if (ra::find(desired_portfolio, symbol) == desired_portfolio.end()) {
            assets_to_sell.push_back(symbol);
        } else {
            assets_to_keep.push_back(symbol);
        }
    }
    for (const auto& s : desired_portfolio) {
        if (auto it = current_portfolio.equities.find(s); it == current_portfolio.equities.end() || it->second == 0) {
            assets_to_buy.push_back(s);
        }
    }
    if (assets_to_buy.empty() && assets_to_sell.empty()) {
        // Existing assets won't be renormalized in this case.
        return {};
    }

    // First, take care of selling the assets we don't need.
    double cash_after_primary_sells = current_portfolio.cash;
    vector<MarketOrder> market_orders;
    for (auto& s : assets_to_sell) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& daily_bar_close, asset_price_lookup(asset_prices, s));
        const auto num_shares = current_portfolio.equities.at(s);
        const double transaction_value =
          estimate_net_income_from_selling_equity_by_shares(broker_scheme, num_shares, daily_bar_close);
        if (transaction_value < k_min_cash_amount_to_trade) {
            continue;
        }
        cash_after_primary_sells += transaction_value;
        market_orders.push_back(MarketOrder{.symbol = s, .unit = MarketOrder::Unit::shares, .quantity = -num_shares});
    }

    if (assets_to_buy.empty() && assets_to_keep.empty()) {
        // Everything sold, don't try to null remaining cash.
        return market_orders;
    }

    struct AssetChange {
        double current_stock = 0.0;
        double close_price = 0.0;
        double shares_to_buy = 0.0;
    };
    std::flat_map<string, AssetChange> assets_to_buy_and_sell;

    // Find the trade value of the rest of the current assets and add the trade value of cash_after_primary_sells.
    double current_trade_value_of_desired_assets = 0.0;
    for (auto& s : assets_to_keep) {
        auto& a = assets_to_buy_and_sell[s];
        a.current_stock = current_portfolio.equities.at(s);
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(a.close_price, asset_price_lookup(asset_prices, s));
        current_trade_value_of_desired_assets += a.current_stock * a.close_price;
    }

    double final_trade_value_of_desired_assets = current_trade_value_of_desired_assets;
    if (cash_after_primary_sells < 0) {
        // Find out assets of what trade value we need to sell to make up for -cash_after_primary_sells.
        final_trade_value_of_desired_assets -=
          estimate_trade_value_when_selling_equity_and_getting_net_income(broker_scheme, -cash_after_primary_sells);
    } else if (cash_after_primary_sells > 0) {
        // Find out assets of what trade value we need to buy to spend cash_after_primary_sells.
        final_trade_value_of_desired_assets +=
          estimate_trade_value_when_buying_equity_for_cash_order(broker_scheme, cash_after_primary_sells);
    }

    // Enter assets_to_buy into assets_to_buy_and_sell, their current stock will be zero.
    for (auto& s : assets_to_buy) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(assets_to_buy_and_sell[s].close_price, asset_price_lookup(asset_prices, s));
    }

    const double desired_trade_value_per_asset =
      final_trade_value_of_desired_assets / ifcast<double>(assets_to_buy_and_sell.size());

    // Create an initial guess of how many shares we need to buy/sell.
    for (auto&& [_, a] : assets_to_buy_and_sell) {
        const double current_trade_value = a.current_stock * a.close_price;
        a.shares_to_buy = (desired_trade_value_per_asset - current_trade_value) / a.close_price;
    }

    constexpr double k_rebalance_threshold_factor = 0.01; // Don't rebalance assets if the change is minimal

    // We're going to iterate to find the best amount of shares to buy/sell to have zero cash left and the assets
    // are (approximately) equal in trade value.

    // cash_after_transaction_for_scaling calculates this:
    // For each asset we now the current stock and how many shares we need to buy/sell to end up with equal trade
    // value. Because of broker costs, this initial guess is not exact.
    // This function, instead of trying to get to current_stock + shares_to_buy, it applies a scaling to that value,
    // resulting in lower or higher final portfolio value and higher or lower remaining cash.
    const auto cash_after_transaction_for_scaling =
      [cash_after_primary_sells, &assets_to_buy_and_sell, broker_scheme](double scaling) -> double {
        double cash_after_transactions = cash_after_primary_sells;
        for (const auto& a : assets_to_buy_and_sell.values()) {
            const auto scaled_shares_to_buy = scaling * (a.current_stock + a.shares_to_buy) - a.current_stock;
            if (scaled_shares_to_buy < 0) {
                // Sell.
                cash_after_transactions += estimate_net_income_from_selling_equity_by_shares(
                  broker_scheme, -scaled_shares_to_buy, a.close_price
                );
            } else if (scaled_shares_to_buy > 0) {
                // Buy.
                cash_after_transactions -= estimate_total_cash_needed_when_buying_equity_for_shares(
                  broker_scheme, scaled_shares_to_buy, a.close_price
                );
            }
        }
        return cash_after_transactions;
    };

    // Compute the scaling which results in zero remaining chash by fitting a line on 2 points.
    const auto compute_scaling = [&](double x0, double d) {
        const double xplus = x0 + d / 2;
        const double xminus = x0 - d / 2;
        const double yplus = cash_after_transaction_for_scaling(xplus);
        const double yminus = cash_after_transaction_for_scaling(xminus);
        const double a = (yplus - yminus) / d;
        const double b = (yplus + yminus) / 2 - a * x0;
        const double scaling = -b / a;
        return scaling;
    };

    // Return market orders or the smallest order that is invalid.
    const auto create_market_orders_with_scaling =
      [&assets_to_buy_and_sell](double scaling) -> expected<vector<MarketOrder>, string> {
        vector<MarketOrder> mos;
        optional<pair<string, double>> rejected_order;
        for (auto&& [symbol, a] : assets_to_buy_and_sell) {
            const auto scaled_shares_to_buy = scaling * (a.current_stock + a.shares_to_buy) - a.current_stock;
            const auto quantity = scaled_shares_to_buy * a.close_price;
            if (abs(quantity) < k_min_cash_amount_to_trade
                || abs(scaled_shares_to_buy) < a.current_stock * k_rebalance_threshold_factor) {
                if (!rejected_order || abs(quantity) < rejected_order->second) {
                    rejected_order.emplace(symbol, abs(quantity));
                }
            } else {
                mos.push_back(MarketOrder{.symbol = symbol, .unit = MarketOrder::Unit::cash, .quantity = quantity});
            }
        }
        if (!rejected_order) {
            return mos;
        }
        return unexpected(rejected_order->first);
    };

    for (;;) {
        constexpr double x0 = 1.0;
        constexpr double d0 = 0.1;
        constexpr int k_max_iterations = 12;

        double d = d0;
        struct Best {
            double scaling;
            double abs_cash;
        } best{x0, abs(cash_after_transaction_for_scaling(x0))};
        for (int i = 0; best.abs_cash > k_min_cash_amount_to_trade && i < k_max_iterations; ++i, d /= 2) {
            const auto new_scaling = compute_scaling(best.scaling, d);
            const auto new_abs_cash = std::isfinite(new_scaling) && new_scaling > 0
                                      ? abs(cash_after_transaction_for_scaling(new_scaling))
                                      : INFINITY;
            if (new_abs_cash < best.abs_cash) {
                best.scaling = new_scaling;
                best.abs_cash = new_abs_cash;
            }
        }

        const double scaling = best.scaling;

        constexpr double k_min_reasonable_scaling = 0.4;
        constexpr double k_max_reasonable_scaling = 2.1;
        if (!std::isfinite(scaling) || !in_cc_range(scaling, k_min_reasonable_scaling, k_max_reasonable_scaling)) {
            return unexpected(format(
              "Scaling ({}) is not finite or out of reasonable range ({}...{})",
              scaling,
              k_min_reasonable_scaling,
              k_max_reasonable_scaling
            ));
        }
        const auto mos = create_market_orders_with_scaling(scaling);
        if (mos) {
            market_orders.append_range(*mos);
            return market_orders;
        }
        // Remove the smallest invalid order.
        assets_to_buy_and_sell.erase(mos.error());
    }
}

namespace
{
// A switch for the reason config() gives above: a case missing from here is a
// diagnostic rather than a silently wrong price.
double bar_price(const DailyBar& bar, MarketOrderPriceType price_type)
{
    switch (price_type) {
    case MarketOrderPriceType::open:
        return bar.open;
    case MarketOrderPriceType::close:
        return bar.close;
    }
    std::unreachable();
}
} // namespace

expected<ApplyMarketOrdersResult, string> apply_market_orders(
  BrokerCostScheme broker_scheme,
  MarketData& market_data,
  chr::local_days trading_day,
  MarketOrderPriceType price_type,
  Portfolio portfolio,
  const vector<MarketOrder>& market_orders
)
{
    auto asset_prices = std::flat_map<string, double>();
    for (const auto& o : market_orders) {
        if (asset_prices.contains(o.symbol)) {
            continue;
        }
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& bar, market_data.daily_bar(o.symbol, trading_day));
        asset_prices[o.symbol] = bar_price(bar, price_type);
    }
    return apply_market_orders(broker_scheme, std::move(asset_prices), std::move(portfolio), market_orders);
}

namespace
{
// A sell of a whole position arrives as a trade value, and dividing it back by
// the price it was priced with need not land on the share count exactly -- it
// can miss in either direction. This much slack, relative to the position,
// still counts as selling all of it; beyond it the order is asking for shares
// the portfolio does not have.
constexpr double k_full_sale_tolerance_factor = 1e-9;
} // namespace

expected<ApplyMarketOrdersResult, string> apply_market_orders(
  BrokerCostScheme broker_scheme,
  std::flat_map<string, double> asset_prices,
  Portfolio portfolio,
  const vector<MarketOrder>& market_orders
)
{
    for (const auto& o : market_orders) {
        if (!isfinite(o.quantity)) {
            return unexpected(format("Invalid quantity for {}: {}", o.symbol, o.quantity));
        }
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const double price, asset_price_lookup(asset_prices, o.symbol));

        // Both units reduce to a share count: a cash quantity is the trade value
        // before commission, so the price divides straight out of it.
        const double shares = o.unit == MarketOrder::Unit::shares ? o.quantity : o.quantity / price;
        const double trade_value = abs(shares) * price;
        if (trade_value < k_min_cash_amount_to_trade) {
            continue;
        }

        auto& held = portfolio.equities[o.symbol];
        if (shares < 0) {
            const double shares_to_sell = -shares;
            const double slack = abs(held) * k_full_sale_tolerance_factor;
            if (shares_to_sell > held + slack) {
                return unexpected(
                  format("Order to sell {} shares of {}, but only {} held", shares_to_sell, o.symbol, held)
                );
            }
            portfolio.cash += estimate_net_income_from_selling_equity_by_shares(broker_scheme, shares_to_sell, price);
            // Within the slack the position is gone, whichever side the division
            // landed on, and saying so exactly keeps dust of either sign out of the
            // result -- the next rebalance would otherwise read it as a holding.
            const double remaining = held - shares_to_sell;
            held = abs(remaining) <= slack ? 0.0 : remaining;
        } else {
            portfolio.cash -= estimate_total_cash_needed_when_buying_equity_for_shares(broker_scheme, shares, price);
            held += shares;
        }
    }
    return ApplyMarketOrdersResult{.portfolio = std::move(portfolio), .asset_prices = std::move(asset_prices)};
}

double Portfolio::total(const std::flat_map<string, double>& asset_prices) const
{
    double total = cash;
    for (const auto&& [symbol, shares] : equities) {
        if (shares == 0) {
            continue;
        }
        const auto price = asset_price_lookup(asset_prices, symbol);
        LOG_IF(FATAL, !price) << format("No price for {}, reason: {}", symbol, price.error());
        total += shares * *price;
    }
    return total;
}
