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
void print_grid_report_as_csv(const vector<UniverseResult>& urs);
void print_csv_report_line(
  string_view universe,
  string_view lookback,
  dual_mom_fixed_etf_algorithm::RebalanceDay timing,
  const BacktestReport& br
);
