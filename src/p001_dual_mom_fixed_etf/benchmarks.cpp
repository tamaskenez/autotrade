#include "benchmarks.h"

vector<pair<string, double>> get_benchmark_portfolio(BenchmarkType benchmark_type)
{
    switch (benchmark_type) {
    case BenchmarkType::spy:
        return {pair("SPY", 1.0)};
    case BenchmarkType::spy_60_ief_40:
        return {pair("SPY", 60.0), pair("IEF", 40.0)};
    }
    std::unreachable();
}

vector<string> get_benchmark_assets(BenchmarkType benchmark_type)
{
    const auto bp = get_benchmark_portfolio(benchmark_type);
    vector<string> result;
    result.reserve(bp.size());
    for (const auto& [k, _] : bp) {
        result.push_back(k);
    }
    return result;
}
