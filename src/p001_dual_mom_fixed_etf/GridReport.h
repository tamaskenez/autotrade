#pragma once

#include <meadow/cppext.h>

#include "BacktestReport.h"
#include "DualMomFixedEtfAlgorithm.h"

enum class Universe {
    full,
    drop_efa,
    drop_spy,
    drop_ief
};

struct GridKey {
    dual_mom_fixed_etf_algorithm::RebalanceDay timing;
    chr::months lookback;

    bool operator==(const GridKey&) const = default;
};

struct UniverseResult {
    Universe universe;
    vector<pair<GridKey, BacktestReport>> cells;
};

void print_grid_report(const vector<UniverseResult>& urs);
