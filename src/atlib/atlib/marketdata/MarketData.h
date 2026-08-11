#pragma once

#include <atlib/marketdata/equity.h>
#include <atlib/marketdata/rate.h>

#include <meadow/cppext.h>

// Which vendor's version of the equity legs to use.
//
// A parameter rather than a choice made once, because the whole backtest gets run
// per provider and the holdings vectors compared -- data source is an axis of the
// robustness grid, not a decision. It covers equities only. Rates have exactly one
// source and it is named where it is used; see rate_history().
enum class Provider {
    tiingo,
};

struct MarketDataConfig {
    // Root of everything generated: the raw payload cache lives at
    // <workspace_dir>/raw/<provider>/<symbol>.<ext>, which is where the Python
    // tools already put it. The two share the cache; neither owns it.
    fs::path workspace_dir;
};

// The steps of equity_history() and rate_history() below, each callable on its
// own. Both run the same steps; where one is overloaded, the two histories differ
// only in which rows it looks at.
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

// The same rule for a rate series: its last observation is dated `as_of` or later.
//
// The costs land in the same place and are, if anything, easier to trip over. A
// rate series has its own holidays, which are not the exchange's, so a date that
// is an ordinary trading day can still have no observation -- and a caller
// stepping through *equity* dates will hit those. See RateHistory: the gaps are
// not a calendar, and covering `as_of` is about publication, not about whether
// the rate existed that day.
bool covers(const RateHistory& history, chr::local_days as_of);

// Writes the payload to `path`, creating parent directories.
//
// Via a temporary and a rename, so an interrupted write cannot leave a truncated
// file that a later run reads as a valid cache entry.
expected<void, string> write_cache(const fs::path& path, string_view payload);

// Drops every row dated after `as_of`. A plain date comparison.
void truncate_to(EquityHistory& history, chr::local_days as_of);
void truncate_to(RateHistory& history, chr::local_days as_of);

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

// Daily rate history for `symbol`, from FRED, as it stood at `as_of`.
//
// The same function as equity_history() over a different payload: same cache
// path, same one-download retry, same freshness rule, same truncation, same
// error when the data does not reach `as_of`. Everything written above applies
// here, including the parts about what a call costs and why nothing is
// memoised -- read it there rather than trusting a summary.
//
// No provider parameter, and this is not an oversight to be corrected later.
// Above, the provider is a parameter because vendors disagree about what SPY did
// and running the backtest on each of them is the test. FRED does not disagree
// with anyone: DTB3 is the H.15 release itself, so every other source of it --
// the Treasury's own API, any commercial feed -- is republishing these numbers
// rather than measuring them. There is no second opinion to be had and so nothing
// to diff. If a rate ever arrives that is genuinely someone's estimate, this grows
// a parameter and that will be the reason.
//
// `symbol` is still a parameter, and earns it: DGS3MO is the same bill quoted
// bond-equivalent instead of discount, which is how the conversion below gets
// checked.
//
// What comes back is quotes and a label saying what basis they are quoted on.
// Turning that into money is not done here and is not free: DTB3 is a bank
// discount rate, so it is not a return, and it is published on the Treasury's
// calendar rather than the exchange's. See RateConvention and RateHistory for
// both traps before using the numbers.
expected<RateHistory, string> rate_history(const MarketDataConfig& config, string_view symbol, chr::local_days as_of);
