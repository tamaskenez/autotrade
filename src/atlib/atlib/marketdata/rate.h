#pragma once

#include <meadow/cppext.h>

// Provider-neutral daily history for an interest rate.
//
// Separate from EquityHistory, and deliberately not shaped like it, because a
// yield is not a price: you integrate it, you do not take ratios of it. Giving a
// rate a DailyBar -- the same number in all four price fields -- would put it one
// call away from total_return_factor_close_to_close(), which would return the
// ratio of two yields: a small, plausible, entirely meaningless number that
// nothing downstream could recognise as wrong. The types differ so that mistake
// does not compile. Same reasoning as the separate `rate` table in
// src/marketdata/schema.sql.

// How a quoted rate turns into money. There is no default and no "unspecified":
// the basis is not recoverable from the numbers, and every guess is right for
// some series and wrong for others.
//
// The conversion itself does not live here yet -- this library only carries the
// quotes and the label that says what they mean.
enum class RateConvention {
    // Bank discount, actual/360, quoted against face value: the price of a bill
    // is face * (1 - value/100 * days/360). Not a return. Reading it as one
    // understates what the bill actually earns by ~14bp at a 5% level, which is a
    // systematic, one-directional error in whatever it is compared against.
    //
    // This is what FRED DTB3 is.
    discount_360,
};

struct RateObservation {
    chr::local_days date;

    // Percent per annum, exactly as the source quotes it: 3.74 is 3.74%, not
    // 0.0374. Not rescaled on the way in, so a number here and the same number in
    // the cached payload are the same number, and a human comparing the two is
    // not silently comparing across a factor of 100.
    //
    // What it *means* is RateConvention, below, and not this field.
    double value;
};

// One rate series, ascending by date -- see rate_history() in MarketData.h.
//
// Note what the dates are not. For an equity, the set of dates in `bars` is the
// trading calendar, and a date with no bar is a date on which nothing could have
// been done. Here a date with no observation is just a bank holiday: the rate
// still existed, money still accrued, and the last quote is still the current
// one. So the gaps carry no information and must not be treated as they are for
// bars -- the series has to be read forward-filled, and it does not line up with
// any exchange calendar (the bond market shuts for Columbus Day and Veterans Day;
// the NYSE does not).
struct RateHistory {
    string symbol;
    RateConvention convention;
    vector<RateObservation> observations;
};
