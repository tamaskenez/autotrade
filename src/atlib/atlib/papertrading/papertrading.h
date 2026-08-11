#pragma once

#include <meadow/cppext.h>

#include <flat_map>

struct Portfolio {
    double cash = 0;
    std::flat_map<string, double> equities;
};

// buy if money > 0
Portfolio buy_or_sell_equities_by_money(const Portfolio& portfolio, const string& symbol, double money);
