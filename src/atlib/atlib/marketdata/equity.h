#pragma once

#include <meadow/cppext.h>

// Provider-neutral daily history for an exchange-listed instrument.
//
// "Equity" is the instrument class, not the underlying: anything that trades on
// an exchange with a daily candle and occasional dividends and splits. Bond ETFs
// qualify. Open-end mutual funds do not -- they price once a day at NAV, so they
// have no candle and no volume.
//
// Raw prices only. Vendors publish an adjusted series alongside (Tiingo's
// adjClose and friends), but adjusted values are restated whenever a corporate
// action lands, which makes them useless for point-in-time work: a payload
// downloaded today reports a 2010 adjClose that nobody could have seen in 2010.
// Raw trade prices never change. Total returns get computed downstream from the
// raw bars plus the distributions and splits below.
//
// Dates are chr::local_days -- a day count, with no timezone attached, which is
// what a trading date is. Two reasons over chr::year_month_day, which is the
// same 4 bytes and prints the same:
//
//   A local_days cannot be a date that does not exist. year_month_day will hold
//   2024-02-30 and only admits it if someone calls ok(); convert that silently
//   and you get 2024-03-01, a wrong date that looks entirely plausible for the
//   rest of the backtest. Making the field a day count forces the parser to
//   reject bad input at the boundary, which is the only place it can be
//   recognised as bad.
//
//   Nothing here can be mistaken for an instant. A local_time does not compare
//   against a sys_time, so a trading date cannot drift into being treated as a
//   moment in UTC -- which is the mistake that a timezone would then have to be
//   invented to correct.
//
// The cost is calendar arithmetic -- "twelve months back", "last day of the
// month" -- which needs a chr::year_month_day{d} round-trip. That belongs to
// whoever is choosing rebalance dates, and happens once per decision rather
// than once per bar.

struct DailyBar {
    chr::local_days date;
    double open;
    double high;
    double low;
    double close;
    double volume; // double, not integer: adjusted-share arithmetic downstream
                   // produces fractions, and at ETF volumes the integer part is
                   // the only digit anyone trusts anyway.
};

// A cash distribution, keyed by ex-date. No breakdown into ordinary income vs.
// capital gains: vendors report a lump sum and only the issuer files carry the
// categories. See src/marketdata/schema.sql for what a reconciliation against
// those would need.
struct Distribution {
    chr::local_days ex_date;
    double amount;
};

// 2.0 for a 2-for-1, 0.1 for a 1-for-10 reverse split.
//
// Stored as one double rather than the num/den pair the Python model uses.
// Tiingo's denominator is always 1, so nothing is lost today; a vendor that
// reports 3-for-2 as an exact ratio would need the pair back.
struct Split {
    chr::local_days ex_date;
    double factor;
};

// One instrument's history, ascending by date, truncated to what the caller was
// entitled to see -- see equity_history().
struct EquityHistory {
    string symbol;
    vector<DailyBar> bars;
    vector<Distribution> distributions;
    vector<Split> splits;

    // Adjust the daily bars (apply distributions and splits) in place, normalized in a way that the last bar's prices
    // don't change.
    // Clear the distributions and splits vectors.
    //
    // The bars become a total-return series: consecutive closes then grow by what a holder earned, on the convention
    // total_return.h documents -- distributions reinvested at the close of their ex-date, at the declared amount.
    // Volumes are restated in current shares, so earlier ones are fractional after a split.
    //
    // An event dated after the last bar is dropped without effect: normalizing on that bar is what makes the series
    // comparable across a truncated history, and there is nothing after it to carry the event.
    void adjust();
};

// Which days have data is not something to ask a calendar: it is the set of
// dates present in `bars`. Weekends, exchange holidays, half days and the
// instrument's own listing date all fall out of that set for free, correctly,
// per instrument, without a holiday table to maintain or get wrong.
//
// The set says nothing about the future. Whether a bar is missing because the
// exchange was shut or because the vendor has not published it yet is not
// answerable from here, and nothing in this library tries -- see covers() in
// MarketData.h for who carries that burden instead.
