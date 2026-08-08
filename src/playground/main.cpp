#include "atlib/marketdata/tiingo.h"

int main()
{
    auto symbol = "SPY";
    auto a = tiingo::fetch(symbol);
    auto b = tiingo::parse(symbol, a.value());
    auto c = b.value();
    return 0;
}
