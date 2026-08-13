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

// Where a date sits relative to the daily bars we have for an instrument.
//
// Four values rather than a bool because the three ways of not having a bar are
// three different situations for a caller, and collapsing them puts the work of
// telling them apart back on whoever asked. An algorithm comparing a basket needs
// exactly this distinction: every asset answering `not_traded` on the same date is
// a market holiday and unremarkable, one asset answering `before_first_bar` has
// simply not listed yet, and a single `not_traded` among `available` ones is a
// hole in somebody's data and should stop the run.
//
// The two ends are not equally firm, which is the other reason to separate them.
// `before_first_bar` is permanent -- no future download puts a bar before an
// instrument's inception. `after_last_bar` is provisional and says only that we
// have run out of history, whether because the session has not happened, or has
// not been published, or because an `as_of` guard is standing in the way.
enum class DailyBarAvailability {
    // There is a bar for this date.
    available,

    // Inside the range we have, and there is no bar: the exchange was shut. Per
    // equity.h, the set of dates in `bars` *is* the trading calendar, so this is
    // what "not a trading day" means here -- there is no holiday table to consult
    // and a vendor dropping a row would be indistinguishable from a closure.
    not_traded,

    // Earlier than the first bar we have -- before the instrument listed.
    before_first_bar,

    // Later than the last bar we have. Not a statement about the exchange.
    after_last_bar,
};

// The MarketData provides
// - queries about time series
// - an "as_of" guard for simulating that data after a certain date is not available yet, for backtesting
// - cacheing the parsed data, preprocessing
//
// Cacheing behavior: for each query, after checking the as_of guard, it tries to use the cache. If the
// requested time series doesn't exist in the cache or it's too old, downloads it again.
//
// Three rules the queries share, all of them consequences of the two above:
//
//   A `day` past `as_of` terminates. It is not an error, because it is not a
//   runtime condition: a backtest steps over dates this class handed it, so
//   asking past the guard is a bug in the caller, and routing it through the
//   `expected` would let a caller that forwards errors upward report the most
//   serious defect in the system as a data problem.
//
//   The cache holds the *whole* parsed history, not the part before `as_of`, so
//   set_as_of() costs nothing and a backtest can step it forward without
//   re-parsing megabytes per step. What keeps that from becoming a lookahead
//   hazard is that no query reads the rows directly: they go through visible(),
//   which ends the range at `as_of`, so a row past it is unreachable rather than
//   merely unread.
//
//   Bars never leave this class. A query answers a question -- did it trade, what
//   did it close at -- and hands out no container, so there is no borrowed range
//   that outlives an as_of change or a refetch.
class MarketData
{
public:
    MarketData(const MarketDataConfig& config_arg, Provider provider_arg)
        : config(config_arg)
        , provider(provider_arg)
    {
    }

    void set_as_of(chr::local_days as_of_arg)
    {
        as_of = as_of_arg;
    }

    void clear_as_of()
    {
        as_of.reset();
    }

    // Where `day` sits relative to the daily bars we have for `symbol`.
    //
    // Answered entirely from the visible range, so a bar dated after `as_of` does
    // not exist for this call -- it is not merely hidden from the answer, it moves
    // where "the last bar" is. A guard sitting on a Sunday therefore reports the
    // preceding Saturday as `after_last_bar` and not `not_traded`, even though a
    // later bar in the payload proves it was a weekend.
    //
    // That is the point-in-time answer rather than a defect: standing at that
    // Sunday, a caller has not seen Monday's bar and so cannot know whether the
    // gap is a weekend or a feed that has stopped. Reporting `not_traded` there
    // would be answering from data the caller is not entitled to, which is the
    // whole thing this class exists to prevent.
    //
    // An error means a data problem and only that: an unusable symbol, a cached
    // payload that will not parse, a download that failed, or a symbol with no
    // history at all.
    expected<DailyBarAvailability, string> daily_bar_availability(string_view symbol, chr::local_days day);

    // The bar `symbol` printed on `day`.
    //
    // The same guard and the same visible range as daily_bar_availability(), so a
    // bar dated after `as_of` is not there to be found rather than merely hidden.
    // The two differ only in what a missing bar means: here it is an error, because
    // a caller asking for a bar has already concluded that `day` traded, and the
    // absence contradicts that. The message names the visible range, which is what
    // says which of the three absences it was.
    //
    // A copy comes back rather than a pointer into `bars`, for the reason above:
    // nothing borrowed can outlive the next set_as_of() or refetch.
    expected<DailyBar, string> daily_bar(string_view symbol, chr::local_days day);

    // Growth of a position in `symbol` held from the close of `from` to the close
    // of `to`, dividends reinvested and splits undone. 1.0 is flat, 1.05 is +5%.
    //
    // The rules are total_return_factor_close_to_close()'s and are documented
    // there, including the half-open boundary at `from` that lets consecutive
    // windows chain by multiplication without a dividend being counted twice or
    // lost between them. Read them there; this only supplies the history.
    //
    // Both dates must be days `symbol` actually traded. A date with no bar is an
    // error rather than a match to the nearest one, because a silently shifted
    // endpoint yields a plausible number nothing downstream can recognise as
    // wrong. Resolve the endpoints first -- that is what daily_bar_availability()
    // is for.
    //
    // Unlike the other queries this one does not consult visible(), and does not
    // need to: `as_of` bounds both endpoints, the window is closed at `to`, and a
    // bar dated later cannot change a factor computed between two earlier ones.
    // The guard is enforced on the arguments instead, which is the only place it
    // can bite here.
    expected<double, string>
    total_equity_return_factor_close_to_close(string_view symbol, chr::local_days from, chr::local_days to);

    // Growth of money rolled in the cash proxy over the same window, on the same
    // scale: the hurdle the equity legs are compared against.
    //
    // The arithmetic, the conventions it commits to, and the ~13bp of hurdle that
    // reading the quote naively would lose, are all in cash_return_factor(); read
    // them there before comparing the two numbers.
    //
    // `symbol` is a FRED series id -- DTB3 for this project. It is a parameter for
    // the reason rate_history() gives, and an unrecognised series is rejected at
    // the parse rather than given a default basis, because a wrong basis does not
    // look wrong.
    //
    // Unlike the equity query the dates need not appear in the series: a rate has
    // no trading calendar and its gaps are bank holidays, so the last quote on or
    // before a day is the one in force. `from` before the first observation is
    // still an error -- there is nothing to carry forward.
    expected<double, string> total_cash_return_factor(string_view symbol, chr::local_days from, chr::local_days to);

private:
    // One symbol's parsed history, plus what this run has already tried in order
    // to get it. Templated because the rules below are the same rules for a price
    // series and a rate series, and they are the part worth getting right.
    template<class History>
    struct Cached {
        // nullopt when nothing was cached on disk and no download has succeeded.
        optional<History> history;

        // Whether a download has been attempted this run, and what came of it.
        //
        // The flag exists because a miss is answered with `false` rather than an
        // error: a caller walking forward over dates past the end of the payload
        // would otherwise trigger a download per date. One attempt per symbol per
        // run, and afterwards the payload's own last date decides.
        //
        // The error is kept rather than discarded so that a failed download does
        // not decay into `false` on the next query -- a network failure is not a
        // holiday, and every later query that needs data the cache does not have
        // gets told the same thing.
        bool downloaded = false;
        optional<string> download_error;
    };

    // The cached history for `symbol`, loading it from disk and downloading once
    // if what is there does not reach `day`.
    //
    // May return a history that still does not reach `day`. That is not an error
    // here; see has_daily_bar() for what it means.
    expected<const EquityHistory*, string> equity(string_view symbol, chr::local_days day);

    // The same, for a FRED rate series. No provider parameter, for the reason
    // rate_history() gives: there is no second opinion on DTB3 to diff against.
    expected<const RateHistory*, string> rate(string_view symbol, chr::local_days day);

    // The prefix of `bars` that `as_of` allows, which is all of it when no guard
    // is set. The one place any query is allowed to reach the rows.
    span<const DailyBar> visible(const vector<DailyBar>& bars) const;

    MarketDataConfig config;
    Provider provider;
    optional<chr::local_days> as_of;
    // If set, all queries return error or terminate if it needs information later than as_of.

    // Keyed by symbol, so a run parses each payload once. std::less<> for lookup
    // straight from the string_view the queries take, without a string per call.
    std::map<string, Cached<EquityHistory>, std::less<>> equities;
    std::map<string, Cached<RateHistory>, std::less<>> rates;
};
