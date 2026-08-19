#pragma once

#include "atlib/papertrading/PortfolioHistory.h"
#include "atlib/papertrading/papertrading.h"

#include <meadow/cppext.h>

// Experimental single-asset trading algorithm trying to get buy-sell signals from Sharpe ratio of the past period of
// the asset to skip dips.
// For simplicity, decides and trades on the same daily price quote.
class Trader
{
public:
    Trader(string symbol_arg, double start_capital, chr::local_days start_date);

    // - date must be increasing (trading days)
    // - sharpe is annualized sharpe ratio of the recent prices of the single asset
    // - asset_prices must hold the price of the single asset
    NODIS expected<void, string>
    advance(chr::local_days date, double sharpe, const std::flat_map<string, double>& asset_prices);

    string symbol;
    Portfolio portfolio;
    PortfolioHistory ph;
};
