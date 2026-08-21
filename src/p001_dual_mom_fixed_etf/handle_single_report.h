#pragma once

#include "BacktestReport.h"

#include <meadow/cppext.h>

int handle_single_report(const expected<BacktestReport, string>& result);
