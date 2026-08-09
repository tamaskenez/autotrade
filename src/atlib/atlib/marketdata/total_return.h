#pragma once

#include <atlib/marketdata/equity.h>

#include <meadow/cppext.h>

// The other half of what equity.h defers.
//
// Raw prices are stored because a vendor's adjusted series is restated whenever a
// corporate action lands, which makes it useless for point-in-time work. The
// adjustment still has to happen somewhere; it happens here, from data that never
// changes, under a convention that is ours and is written down rather than
// inherited.

// Growth of a position held from the close of `from` to the close of `to`.
//
// 1.0 is flat, 1.05 is +5%, 0.9 is -10%. A ratio -- not a percentage, not
// annualized -- which is why this is a "factor" and not a "return": everywhere
// else a return is this value minus one. Returned in the multiplicative form
// because that is what the computation produces, and because adjacent windows
// then compose by multiplication with no bookkeeping.
//
// Close to close, per the name, because that is what a month-end signal is. The
// decision is taken on the close of the last trading day of the month, so both
// ends of the lookback are closes. Ending at an open would throw away the final
// session; ending at the *next* open would need a price the decision cannot see.
// What the strategy then earns, executing at the open, is a different quantity
// and is not this one.
//
// Half-open at `from`: its close is the entry price, and a distribution with
// ex_date == from is excluded. That distribution explains the drop into `from`'s
// close -- the price paid here is already ex, and the buyer does not receive it --
// so it belongs to the window ending at `from`. The rule is what lets consecutive
// windows chain without a dividend being counted twice or lost between them.
//
// Distributions are treated as reinvested at the close of their ex-date, at the
// declared amount rather than the observed price drop. The declared amount is
// exact; the drop is buried in the day's ordinary volatility.
//
// Both dates must be dates the instrument traded on. A date with no bar is an
// error and not a match to the nearest one, because a silently shifted endpoint
// yields a plausible number that nothing downstream can recognise as wrong. It is
// a runtime condition rather than a caller bug -- "the last trading day twelve
// months ago" falls before inception for every instrument at the start of its
// history -- which is why this reports rather than asserts.
expected<double, string>
total_return_factor_close_to_close(const EquityHistory& history, chr::local_days from, chr::local_days to);
