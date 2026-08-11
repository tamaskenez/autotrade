#pragma once

#include <atlib/marketdata/rate.h>

#include <meadow/cppext.h>

// FRED -- Federal Reserve Bank of St. Louis.
//
// Not a vendor. DTB3 is the Treasury's own H.15 series, so this is the source
// rather than somebody's cleaned copy of it, and there is nothing to
// cross-check it against that would be more authoritative.
//
// The CSV download rather than the documented JSON API, because it needs no
// registration and no key: one URL, full history, nothing to distribute or
// rotate, and the file is ~250 KB. The API returns the same numbers behind a free
// key. The cost of that choice is that this endpoint exists to serve the site's
// chart-download button rather than under a compatibility promise -- it has
// already renamed its date column once (DATE -> observation_date) -- so parse()
// below leans on the format as little as it can get away with. If it does break,
// it breaks loudly at the parse, and the switch is a URL and a reader.
//
// Format: https://fred.stlouisfed.org/graph/fredgraph.csv?id=DTB3
//
//     observation_date,DTB3
//     1954-01-04,1.33
//     ...
//     2026-08-06,3.74

namespace fred
{

constexpr string_view k_name = "fred";
constexpr string_view k_file_extension = "csv";

// Downloads the full available history for `series_id`, as bytes, unparsed.
//
// Full history every time. The URL takes no date range, the whole file is smaller
// than one of Tiingo's per-symbol payloads, and stitching ranges in the cache
// would cost more bookkeeping than it saves bytes.
//
// Needs no credential, which is the point of using this endpoint.
expected<string, string> fetch(string_view series_id);

// Parses a payload into observations, ascending by date.
//
// Days with no observation are dropped, not carried forward: a bank holiday is
// absent from the source and stays absent here. Forward-filling is a convention
// the caller has to choose deliberately -- see RateHistory -- and doing it at the
// parse would hide which days FRED actually published.
expected<RateHistory, string> parse(string_view series_id, string_view payload);

} // namespace fred
