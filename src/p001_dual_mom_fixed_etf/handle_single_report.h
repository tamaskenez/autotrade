#pragma once

#include "BacktestReport.h"
#include "DualMomFixedEtfAlgorithm.h"

#include <meadow/cppext.h>

int handle_single_report(
  string_view universe,
  string_view lookback,
  dual_mom_fixed_etf_algorithm::RebalanceDay timing,
  const expected<BacktestReport, string>& result
);
