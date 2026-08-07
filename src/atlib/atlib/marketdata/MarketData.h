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

    // Seam for tests. Returns the raw payload for one symbol, and defaults to
    // the provider's real download. A test substitutes a canned response and
    // asserts it is never reached on a cache hit -- without which the cache rule
    // below is only testable against the network.
    function<expected<string, string>(Provider, string_view symbol)> fetch;
};

class MarketData
{
public:
    explicit MarketData(MarketDataConfig config);

    // Daily history for `symbol` as it stood at `as_of`.
    //
    // `as_of` is a wall-clock instant, not a date, and it decides two separate
    // things:
    //
    //   Which rows come back. Every row published after `as_of` is dropped --
    //   see published_at(). This is the whole point of the parameter: a cached
    //   payload almost always holds more history than the caller is entitled
    //   to, and returning it would quietly feed a backtest tomorrow's prices.
    //
    //   Whether the cache is usable. A payload's mtime is when it was
    //   downloaded, so it holds everything the provider had published by then.
    //   It can serve any request with `as_of <= mtime`; anything later needs a
    //   download. Note this is a claim about the download time, not about the
    //   last bar in the file -- asking whether the file "reaches" a given date
    //   cannot work, because on a weekend or holiday it never will and the
    //   answer would be to re-download forever.
    //
    // An `as_of` in the future is an error rather than a fetch: no download
    // could satisfy it, so the alternative is to lie about the result or spin.
    //
    // Cached payloads are refetched in place, so a run today and the same run
    // next month can disagree -- vendors backfill and correct raw history. Only
    // raw prices are kept, which is the smallest such exposure available, but it
    // is not zero.
    expected<EquityHistory, string> equity_history(Provider provider, string_view symbol, chr::sys_seconds as_of);

private:
    MarketDataConfig config_;
};
