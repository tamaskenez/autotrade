#include "benchmarks.h"

vector<pair<string, double>> get_benchmark_portfolio(BenchmarkType benchmark_type)
{
    switch (benchmark_type) {
    case BenchmarkType::spy:
        return {pair("SPY", 1.0)};
    case BenchmarkType::spy_60_ief_40:
        return {pair("SPY", 60.0), pair("IEF", 40.0)};
    }
}

vector<string> get_all_assets(BenchmarkType benchmark_type)
{
    vector<string> result;
    for (auto& [k, v] : get_benchmark_portfolio(benchmark_type)) {
        result.push_back(k);
    }
    return result;
}
