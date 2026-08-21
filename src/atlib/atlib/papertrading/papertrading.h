#pragma once

#include <meadow/cppext.h>

#include <flat_map>

// Used for 2 checks:
// - trade orders less than this are rejected or ignored
// - portfolio rebalancer (market_orders_from_portfolio_change) doesn't try to optimize remaining cash below this limit
constexpr double k_min_cash_amount_to_trade = 1;

struct Portfolio {
    double cash = 0;
    std::flat_map<string, double> equities; // Number of shares.
    NODIS double total(const std::flat_map<string, double>& asset_prices) const;
    NODIS double get_equity_amount(const string& symbol) const;
};

// What a trade costs, as a single fraction of its trade value.
//
// Per side, and on the trade value rather than on portfolio equity: switching
// out of one asset and into another pays it twice. One number stands for
// commission, half-spread and slippage together, because none of the three is
// separately knowable at the moment an order is priced -- the commission is, the
// other two are not -- and a figure comfortably above all three is a margin
// rather than an estimate.
//
// What a fraction cannot express is a per-order minimum, which is an absolute
// amount and so bites hardest where it is least visible: on the smallest orders,
// where a real broker charges a percentage cap instead. Orders far above
// k_min_cash_amount_to_trade are the case this models honestly.
enum class BrokerCostScheme {
    flat_10bp,
    flat_20bp,
    flat_100bp,
};

// buy if money > 0
Portfolio buy_or_sell_equities_by_money(const Portfolio& portfolio, string_view symbol, double money);

// Sell num_shares @ price_quote, how much do I get after deducting cost?
double
estimate_net_income_from_selling_equity_by_shares(BrokerCostScheme scheme, double num_shares, double price_quote);

// What amount of shares, in trade value, do I need to sell in order to get specified net_income?
double estimate_trade_value_when_selling_equity_and_getting_net_income(BrokerCostScheme scheme, double net_income);

// What is the trade value we can buy if we have the specified cash (which includes costs).
double estimate_trade_value_when_buying_equity_for_cash_order(BrokerCostScheme scheme, double cash);

// What do I need to pay, including costs, when buying given number of shares of equity.
double estimate_total_cash_needed_when_buying_equity_for_shares(
  BrokerCostScheme scheme, double num_shares, double price_quote
);

struct MarketOrder {
    string symbol;

    enum class Unit {
        shares,
        cash
    } unit;

    // Negative means sell. In case of cash, this is total trade value, excluding commission/fees.
    double quantity;
};

// The portfolio changes from `current_portfolio` to `desired_portfolio`. Return the market orders that bring
// the former into the latter.
// The market orders can't be precise because we don't have the actual market prices, we're estimating with
// the close prices of `past_trading_day`.
// Since the `desired_portfolio` is a list only, no weighting, the function assumes that we intend to hold those
// equities in equal proportions.
// Note leftover cash at or above k_min_cash_amount_to_trade triggers a full renormalization of the kept assets
// not just a purchase
class MarketData;

expected<vector<MarketOrder>, string> market_orders_from_portfolio_change(
  BrokerCostScheme broker_scheme,
  MarketData& market_data,
  const Portfolio& current_portfolio,
  const vector<string>& desired_portfolio,
  chr::local_days past_trading_day
);

// Different API: instead of MarketData it expects a map with the asset prices.
// It's an error if an asset price is missing (or invalid, e.g. negative).
expected<vector<MarketOrder>, string> market_orders_from_portfolio_change(
  BrokerCostScheme broker_scheme,
  const std::flat_map<string, double>& asset_prices,
  const Portfolio& current_portfolio,
  const vector<string>& desired_portfolio
);

struct ApplyMarketOrdersResult {
    Portfolio portfolio;

    // The prices the orders were filled at, for the symbols the orders name. Not a
    // valuation of the portfolio: a holding no order touched has no entry here.
    std::flat_map<string, double> asset_prices;
};

enum class MarketOrderPriceType {
    open,
    close
};

// Fetch the daily bar, pick the request prices and call the second signature.
expected<ApplyMarketOrdersResult, string> apply_market_orders(
  BrokerCostScheme broker_scheme,
  MarketData& market_data,
  chr::local_days trading_day,
  MarketOrderPriceType price_type,
  Portfolio portfolio,
  const vector<MarketOrder>& market_orders
);

// Fill the specified market orders on the prices, using the estimate* functions above to calculate total prices which
// include commissions/fees.
//
// An order whose trade value is below k_min_cash_amount_to_trade is skipped rather than filled, which is the same
// answer market_orders_from_portfolio_change gives to the sells it declines to emit: a round trip through the two
// agrees about which orders are too small to be worth placing.
//
// Selling more shares than the portfolio holds is an error, because it is a short position arrived at by accident and
// nothing downstream models one. Overdrawing cash is not: the resulting Portfolio may carry a negative balance, and
// noticing that is the caller's job.
//
// Orders are applied in the order given, so several touching one symbol compound. Nothing is filled when the call
// fails -- the portfolio comes in by value and a rejected order discards the whole copy.
expected<ApplyMarketOrdersResult, string> apply_market_orders(
  BrokerCostScheme broker_scheme,
  std::flat_map<string, double> asset_prices,
  Portfolio portfolio,
  const vector<MarketOrder>& market_orders
);
