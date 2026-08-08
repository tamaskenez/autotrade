#pragma once

#include <atlib/marketdata/equity.h>

#include <meadow/cppext.h>

enum class Provider {
    tiingo,
};

struct MarketDataConfig {
    // Root of everything generated: the raw payload cache lives at
    // <workspace_dir>/raw/<provider>/<symbol>.<ext>, which is where the Python
    // tools already put it. The two share the cache; neither owns it.
    fs::path workspace_dir;
};

// The steps of equity_history() below, each callable on its own.
//
// They are public because that is how they get tested: no part of this pipeline
// is reachable only through the front door, so no injection point has to exist in
// production types to make the rules verifiable. They are also, individually,
// things a caller can reasonably want -- where does the payload for SPY live,
// does what we have reach far enough.

// <workspace_dir>/raw/<provider>/<symbol>.<ext>.
fs::path cache_path(const MarketDataConfig& config, Provider provider, string_view symbol);

// Whether `history` already reaches `as_of`: its last bar is dated `as_of` or
// later, so nothing a download could add falls within what the caller asked for.
//
// This is the entire freshness rule, and note what it does not consult: not the
// file's mtime, not the current time, not a publication schedule, not a timezone,
// not a holiday calendar. The payload answers the question about itself. That
// makes the decision deterministic and the same on every machine, and it is why
// no part of this library needs to know what time it is.
//
// The cost is pushed onto the caller, deliberately. Asking for a date that has no
// bar -- today before the close, a weekend, a holiday -- cannot be distinguished
// from asking for a date whose bar simply has not been downloaded yet, so it
// costs one wasted download and then fails. A backtest never does this: it steps
// over dates it read out of `bars`, which are trading days by construction. A
// live caller has to know that the session has closed, which is knowledge it has
// and this library does not.
bool covers(const EquityHistory& history, chr::local_days as_of);

// Writes the payload to `path`, creating parent directories.
//
// Via a temporary and a rename, so an interrupted write cannot leave a truncated
// file that a later run reads as a valid cache entry.
expected<void, string> write_cache(const fs::path& path, string_view payload);

// Drops every row dated after `as_of`. A plain date comparison.
void truncate_to(EquityHistory& history, chr::local_days as_of);

// Daily history for `symbol` as it stood at `as_of`.
//
// `as_of` is a date, inclusive: the last day the caller is allowed to see. A
// backtest standing at the close of D asks for D and gets D's bar, and nothing
// after it. There is no time of day and no timezone, because a daily strategy has
// no use for either -- it reasons in trading days.
//
// Reads the cached payload, and if it does not already reach `as_of`, downloads
// once and reads again. If it still does not reach `as_of`, that is an error
// naming the latest date actually available -- which is the useful thing to say,
// because it is simultaneously the answer to "was I too early?", "is today a
// holiday?" and "does this symbol have data at all?".
//
// Rows dated after `as_of` are dropped. That is the whole point of the parameter:
// a cached payload almost always holds more history than the caller is entitled
// to, and returning it would quietly feed a backtest tomorrow's prices.
//
// Every call reads and parses the payload again -- there is no memoisation, by
// design, so that a call always means a disk read and nobody has to wonder
// whether they are looking at a stale in-process copy. Loading a symbol once per
// run and slicing the result locally is the intended usage; stepping an as-of
// forward day by day re-parses megabytes per step and wants a cache the caller
// owns and can see.
//
// A payload that already reaches `as_of` is never refetched, so vendor
// corrections to history it already covers will not arrive. Deleting the file is
// the way to force a refresh; there is deliberately no flag for it.
expected<EquityHistory, string>
equity_history(const MarketDataConfig& config, Provider provider, string_view symbol, chr::local_days as_of);
