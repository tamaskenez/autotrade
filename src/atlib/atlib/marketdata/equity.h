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
//   The conversion to an instant becomes visible. See published_at() below.
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
// entitled to see -- see MarketData::equity_history.
struct EquityHistory {
    string symbol;
    vector<DailyBar> bars;
    vector<Distribution> distributions;
    vector<Split> splits;
};

// A daily row dated D is not knowable until after that day's close. US equity
// sessions close at 20:00 UTC under EDT and 21:00 under EST; taking the later of
// the two never claims a row was available before it was, at the cost of up to
// an hour of conservatism in summer, and needs no timezone database.
//
// Distributions and splits go through the same test. An ex-date is announced
// weeks in advance in reality, but the vendor payload only reveals it alongside
// the bar, so treating it as same-day news is both simpler and conservative.
constexpr chr::hours k_daily_row_published_after{21};

// The time_since_epoch() round-trip is the whole timezone assumption of this
// library, in one expression: it reinterprets a calendar day in New York as the
// same-numbered day in UTC. Nothing else may do this. It is deliberately ugly
// so that the day someone needs real exchange-local time, grep finds the one
// place to fix.
inline chr::sys_seconds published_at(chr::local_days date)
{
    return chr::sys_days{date.time_since_epoch()} + k_daily_row_published_after;
}
