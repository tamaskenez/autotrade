# Backtest outline — p001 dual momentum, fixed ETF basket

What the backtest has to do, step by step. `README.md` holds the strategy spec;
this is the execution plan for testing it. Where the two disagree, the reason is
written down here.

## Standing decisions

**Sample splits are deferred.** The spec's windows (in-sample 1990–2009,
validation 2010–2017, out-of-sample 2018–today) assume the mutual-fund splice
that `LOG.md` closed on 2026-08-06. Under the all-ETF universe the history is
~2002-09 → today, so the splits have to be re-drawn — but not yet. The first
goal is a backtest that runs end to end over the whole available history. The
periods stay parameters so the split can be imposed later without touching the
engine.

Consequence, accepted deliberately: until the splits exist there is no
out-of-sample set to protect, and equally no protection. Nothing in the first
runs may be treated as an out-of-sample result afterwards.

**History actually available** (Tiingo, per `LOG.md`): SPY from 1993-01-29, EFA
from 2001-08-17, IEF from 2002-07-26, BIL from 2007-05-30, DTB3 from 1954. The
binding constraint is EFA's lookback, not IEF's inception: a 12-month window
ending 2002-08-30 opens 2001-08-29, which exists. So the first signal is
~2002-08-30 and the first execution the next session — earlier than the ~2003-07
estimate in `LOG.md`, which assumed the window had to open after IEF listed.

## 0. Configuration

1. Every backtest parameter is explicit and none is defaulted inside the engine:
   universe (`SPY`, `EFA` risky; `IEF` defensive; `DTB3` hurdle), lookback
   (12 months), rebalance rule (last trading day of the month), cost (0.1% per
   side), provider (`Provider::tiingo`), start date, end date, `as_of`.
2. **One start date shared by every robustness-grid cell** — the same first
   signal and first execution date everywhere, set by whichever cell needs the
   most prior history (the 12-month lookback). The shorter-lookback cells
   therefore ignore history they could have used.

   Not the other reading, where each cell starts as early as its own lookback
   allows. CAGR, max drawdown and worst rolling 12-month are all
   period-dependent, so a cell that ran over a longer window would differ from
   its neighbours for a reason that has nothing to do with the parameter being
   varied. The grid asks whether the parameter matters, not what each variant's
   best-estimate performance is.

   The cost is one month, not six: IEF binds before the lookback does, because
   the defensive leg has to be tradeable at the first execution and IEF's first
   bar is 2002-07-26. A 6-month cell could start at signal 2002-07-31; the
   12-month cell starts at signal 2002-08-30.

## 1. Load

3. Load each equity history **once** at `as_of` via `equity_history()` and slice
   locally. Per `MarketData.h`, every call re-reads and re-parses the payload;
   stepping an as-of forward day by day re-parses megabytes per step.
4. Load `DTB3` via `rate_history()`. Optionally also `DGS3MO` — the same bill
   quoted bond-equivalent instead of discount — as the check on step 6.
5. Assert each history covers the full test window, and that the first bar is
   early enough for the first lookback to open.

## 2. Derived series

6. **Cash total-return index from DTB3.** Convert the discount quote to a daily
   accrual, forward-fill across FRED's holidays (`rate.h`: the gaps carry no
   information and do not line up with the exchange calendar), and compound over
   *calendar* days so weekends accrue. This conversion does not exist yet —
   `rate.h` says so explicitly. Two checks on it: against `DGS3MO`, and against
   BIL's own total return over 2007→today, where BIL should lag by roughly its
   expense ratio and by more during the ZIRP years `LOG.md` flagged (2010,
   2012–15, 2021).
7. **Daily total-return chain per instrument.** `total_return_factor_close_to_close()`
   over each consecutive pair of bars, composed multiplicatively. This is the
   mark-to-market series for the equity curve; adjacent windows compose by
   construction because of the half-open boundary at `from`.
8. **Open-to-open factor for execution.** The held period runs open(exec) →
   open(next exec), which nothing in `atlib` computes today. The clean fix is a
   `total_return_factor_open_to_open()` sibling in `total_return.h`: same
   distribution and split walk, same half-open rule, different endpoints.
   Decomposing it out of the close-to-close function (intraday + close-to-close +
   overnight) also works but puts the dividend-boundary rule in two places, which
   is where the two will eventually disagree.

## 3. Rebalance schedule

9. **Signal dates** = the last date present in SPY's `bars` in each calendar
   month. The bar set *is* the trading calendar (`equity.h`), so no holiday table
   is involved. For the grid's "day 10 / day 15" variants: the last trading day
   on or before that calendar day.
10. **Execution date** = the next bar after the signal date. Assert
    `exec_date > signal_date` in the loop. That assertion is the lookahead
    control; it belongs in the code, not in a comment.

## 4. Decision, per rebalance

11. **Lookback window opens** at the last bar on or before (signal date − 12
    months), by calendar arithmetic — the convention already used in
    `playground/main.cpp`. Back rather than forward, so the window covers the
    whole period rather than falling short of it.
12. Compute the 12-month total-return factor for SPY, EFA and the cash index over
    that same window. Same endpoints for all three; equities step over bars, cash
    over calendar days.
13. **Relative momentum:** winner = max(SPY, EFA). **Absolute momentum:** if the
    winner's factor exceeds the cash factor, hold the winner; otherwise hold IEF.
14. Two tie-breaks, written down now and not revisited: SPY and EFA exactly equal
    → SPY. Winner exactly equal to the hurdle → IEF, because "exceeds" is strict.
15. **Record the whole decision row**, not just the holding: signal date, all
    three factors, winner, hurdle outcome, target, prior holding, execution date,
    execution price. This is what makes the cross-provider holdings diff
    (`LOG.md`, 2026-08-04) localizable to specific months for free.

## 5. Accounting

16. If the target equals the current holding, **no trade**: no turnover, no cost,
    and it does not count toward the trade count.
17. On a switch, apply 0.1% on the sell and 0.1% on the buy, multiplicatively, at
    the execution open — 0.2% per round trip.
18. Advance equity daily using the step-7 chain. The curve is therefore marked at
    closes while trades happen at opens; state that asymmetry, because the
    drawdown series is close-based.
19. Emit three artifacts: the **daily equity curve**, the **monthly holdings
    vector**, and the **switch sequence** (date, from, to).
20. `atlib/papertrading` already has a `Portfolio` and
    `buy_or_sell_equities_by_money()`. A 100%-one-asset strategy does not need
    them, but reusing them keeps the backtest's fill path and any later live path
    the same shape.

## 6. Benchmarks

21. **Buy-and-hold SPY** — same start, same daily marking, one entry cost.
22. **60/40 SPY/IEF.** The spec does not say how it rebalances and it matters:
    monthly, at the same execution timing, with the same 0.1% per side on the
    traded amount. A convention, chosen here rather than left implicit.

## 7. Metrics

23. One table, strategy and both benchmarks: CAGR, max drawdown, Sharpe (risk-free
    = the step-6 cash index, not zero), worst rolling 12-month return, number of
    trades, total cost paid.
24. **Named-episode check**, per the README's stated known behaviors: did it hold
    IEF through 2008, and what did it do in Aug 2015, Feb–Apr 2020 (whipsaw
    expected) and 2022. This is the calibration test. If 2020 comes out clean,
    suspect a bug before suspecting genius.

## 8. Robustness grid

25. Axes: lookback {6, 9, 12} × rebalance day {month-end, 10, 15} × asset dropout
    {none, −EFA, −IEF} × provider {tiingo, + EODHD when it lands}. Same start date
    in every cell (step 2).
26. Report the full grid, never the best cell. The pass condition is graceful
    degradation across cells; a single good cell is noise.
27. Run it on in-sample data only — which, until the splits exist, means it does
    not tell you what the spec intends it to tell you. Re-run it once the windows
    are drawn.

## 9. Correctness checks

These are tests, not a one-off script.

28. Lookahead assertions in the loop: every date read for a decision ≤ the signal
    date; every execution date > it.
29. Reconstruct buy-and-hold SPY from the daily chain and compare against a single
    full-history `total_return_factor_close_to_close()` call. They must agree to
    floating-point noise. Catches chaining and boundary errors in one line.
30. One hand-computed month: all three factors, the decision, and the post-cost
    equity checked against numbers worked out by hand.
31. The EFA 3-for-1 (2005-06-09) and BIL 1-for-2 reverse (2017-11-30) must be
    transparent to the open-to-open factor. `LOG.md` verified them against
    `vendor_adj`, but step 8 is new code that re-derives them.
32. Cost identity: total cost paid == number of trades × 0.2% of equity at each
    execution.

## Known costs of this plan

- **Step 8 is the only real new library work**, and it is not optional. Executing
  at the signal-day close instead would be lookahead and would defeat the rule
  the spec calls out by name.
- **The pre-2002 window is gone** under the current data. The history covers 2008,
  2020 and 2022 but not 2000 or 1987, so the regime diversity the README argued
  for is thinner than it assumes. That is an input to the deferred split decision,
  not a reason to delay the first working run.
