#pragma once

#include <meadow/cppext.h>

enum class BenchmarkType {
    spy,
    spy_60_ief_40
};

// Return weighted portfolio.
NODIS vector<pair<string, double>> get_benchmark_portfolio(BenchmarkType benchmark_type);

vector<string> get_all_assets(BenchmarkType benchmark_type);
