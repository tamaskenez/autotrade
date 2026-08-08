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
// is reachable only through the front door, so no injection point has to exist
// in production types to make the rules verifiable. They are also, individually,
// things a caller can reasonably want -- where does the payload for SPY live,
// when was it downloaded.

// <workspace_dir>/raw/<provider>/<symbol>.<ext>.
fs::path cache_path(const MarketDataConfig& config, Provider provider, string_view symbol);

// When the cached payload was downloaded, or nullopt if there is no cache entry.
// Errors are filesystem failures -- a missing file is not one.
expected<optional<chr::sys_seconds>, string> cache_downloaded_at(const fs::path& path);

// Whether a payload downloaded at `downloaded_at` can answer a request for
// `as_of`: it holds everything the provider had published by then, so it covers
// any earlier instant and nothing later.
bool cache_covers(chr::sys_seconds downloaded_at, chr::sys_seconds as_of);

// Writes the payload and stamps it with `downloaded_at`.
//
// Via a temporary and a rename, so an interrupted write cannot leave a truncated
// file that a later run reads as a valid cache entry. The stamp is the instant
// the request was made rather than the one the write finished, so that reusing
// the file later reports the same vintage the downloading run did -- otherwise
// an identical payload dates differently depending on how long the transfer
// took.
expected<void, string> write_cache(const fs::path& path, string_view payload, chr::sys_seconds downloaded_at);

// Drops every row published after `as_of`. See published_at().
void truncate_to(EquityHistory& history, chr::sys_seconds as_of);

// Daily history for `symbol` as it stood at `as_of`. Composes the functions
// above, downloading only when the cache cannot answer the request.
//
// `as_of` is a wall-clock instant, not a date, and it decides two separate
// things:
//
//   Which rows come back. Every row published after `as_of` is dropped -- see
//   published_at(). This is the whole point of the parameter: a cached payload
//   almost always holds more history than the caller is entitled to, and
//   returning it would quietly feed a backtest tomorrow's prices.
//
//   Whether the cache is usable. A payload's mtime is when it was downloaded,
//   so it holds everything the provider had published by then. It can serve any
//   request with `as_of <= mtime`; anything later needs a download. Note this is
//   a claim about the download time, not about the last bar in the file --
//   asking whether the file "reaches" a given date cannot work, because on a
//   weekend or holiday it never will and the answer would be to re-download
//   forever.
//
// An `as_of` in the future is an error rather than a fetch: no download could
// satisfy it, so the alternative is to lie about the result or spin.
//
// Every call reads and parses the payload again -- there is no memoisation, by
// design, so that a call always means a disk read and nobody has to wonder
// whether they are looking at a stale in-process copy. Loading a symbol once per
// run and slicing the result locally is the intended usage; stepping an as-of
// forward day by day re-parses megabytes per step and wants a cache the caller
// owns and can see.
//
// Cached payloads are refetched in place, so a run today and the same run next
// month can disagree -- vendors backfill and correct raw history. Only raw
// prices are kept, which is the smallest such exposure available, but it is not
// zero.
expected<EquityHistory, string>
equity_history(const MarketDataConfig& config, Provider provider, string_view symbol, chr::sys_seconds as_of);
