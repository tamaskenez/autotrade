#pragma once

#include <atlib/marketdata/equity.h>
#include <atlib/marketdata/rate.h>

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

// Growth of money rolled in short bills from the close of `from` to the close of
// `to`, on the same scale and the same window as the function above: 1.0 is flat,
// half-open at `from`, so the two chain and compare over identical windows. That
// matters more than it looks -- the absolute-momentum filter subtracts one from
// the other, and a window that differed by a day at one end would put the
// difference into the comparison.
//
// This is the conversion rate.h defers, and it is not a formatting step. DTB3 is a
// bank *discount* rate: it prices the bill against face value, so the quote is not
// what the money earns. A 5% quote is a 5.134% bond-equivalent yield -- the ~13bp
// rate.h warns about -- which comes out here as 5.268% of realised growth over a
// year against 5.200% for the naive reading, so about 7bp a year, in one
// direction, in exactly the number the equity legs are measured against.
// Understating the hurdle biases the strategy toward holding equities.
//
// The conversion, per observation:
//
//   price = 1 - d * n/360             a bill maturing in n days, per unit face
//   yield = d / price                 what the buyer actually earns, act/360
//
// with n = 91, the 13-week bill DTB3 quotes. Growth is then compounded daily at
// yield/360 over *calendar* days, weekends and holidays included, because money
// does not stop accruing when a desk is shut.
//
// Two convention choices. The second is not small, and is the more important of
// the two by an order of magnitude:
//
//   Daily compounding rather than a strict 13-week roll. A real bill compounds
//   about four times a year, not 365, which makes this run ~3bp/year rich at a 5%
//   level.
//
//   The rate in force is the *spot* three-month quote every day, so this is money
//   rolled daily at whatever three-month bills yield that morning -- not a bill
//   bought and held to maturity, which is what a published T-bill index measures.
//   The two diverge whenever rates move fast, because a held bill is locked into
//   an older yield and this is not. Measured against published 3-month T-bill
//   total returns: 2021 (ZIRP, flat) agrees to a basis point, 2023 runs ~34bp
//   rich, 2007 ~24bp cheap as rates fell, and 2022 ~58bp rich as they went from
//   0.05% to 4.4%. That is far larger than the ~7bp the discount basis is worth,
//   and it changes sign with the direction of rates.
//
//   Kept anyway, because it is the only construction that is *windowable*. A real
//   bill roll depends on when the roll cycle started, so a factor between two
//   arbitrary dates would not be well defined and adjacent windows would not
//   chain -- and chaining is what lets this be compared against the equity legs
//   over identical windows at all. The cost is a bias of up to ~50bp, with a sign
//   that follows the rate cycle, in the number the absolute-momentum filter
//   subtracts. Near-ties are where that filter flips, so it can move a holding.
//   Use BIL as the cash proxy over 2007+ if that bias ever needs pinning down;
//   see the overlap check in BACKTEST.md.
//
//   Gaps are forward-filled, per RateHistory: a missing day is a bank holiday, not
//   a day the rate did not exist. The series has its own calendar and does not
//   line up with the exchange's, so the caller's dates are not required to appear
//   in it -- unlike the equity function, where an endpoint with no bar is an error.
//
// `from` must not precede the first observation: there is no rate in force to
// carry forward, and assuming one would invent the hurdle rather than measure it.
expected<double, string> cash_return_factor(const RateHistory& history, chr::local_days from, chr::local_days to);
