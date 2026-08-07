#pragma once

#include <atlib/marketdata/equity.h>

#include <meadow/cppext.h>

// Tiingo end-of-day prices.
//
// One request returns bars, dividends and splits together: divCash and
// splitFactor ride along on each daily row rather than coming from separate
// corporate-action endpoints. That is why the cache is one file per symbol and
// not one per kind of data.
//
// API: https://www.tiingo.com/documentation/end-of-day

namespace tiingo
{

constexpr string_view kName = "tiingo";
constexpr string_view kFileExtension = "json";

// Downloads the full available history for `symbol`, as bytes, unparsed.
//
// Full history every time, from an epoch far enough back to predate any listing.
// Tiingo returns only the most recent row unless a start date is given, and
// incremental range requests would mean stitching segments together in the cache
// -- at 2 MB per symbol the whole file is cheaper than the bookkeeping.
//
// Needs a credential; see credentials.h.
expected<string, string> fetch(string_view symbol);

// Parses a payload into raw bars plus corporate actions, ascending by date.
//
// Zero dividends and unit split factors are dropped: Tiingo reports them on
// every ordinary row, and keeping them would bury the real events. The adjusted
// series is ignored -- see equity.h.
expected<EquityHistory, string> parse(string_view symbol, string_view payload);

} // namespace tiingo
