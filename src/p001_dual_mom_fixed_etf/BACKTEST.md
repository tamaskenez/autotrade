# Backtest outline — p001 dual momentum, fixed ETF basket

What the backtest has to do, step by step.

`README.md` is not a specification. It is an excerpt from the conversation that
started this project, kept because it is a good starting point and because it
states the economic hypothesis before anyone looked at data. It binds nothing.
The goal it names is the one that matters: a strategy with a mix of return,
variance and drawdown that can be trusted with part of the author's family's
savings. Everything below is a decision made in service of that, not deference
to the excerpt, and every one of them can be revisited if it stops serving it.

Two things from the excerpt are worth keeping and are used below, neither as a
target:

- The **calibrated expectations** — roughly 7–10% CAGR, 15–25% max drawdown,
  whipsaw in fast V-shaped crashes, multi-year lag in relentless bull runs.
  These are a bug detector. A result far better than this is evidence of a
  defect or of selection, not of an edge.
- The **0.1% per side** cost assumption, which is pessimistic for ETFs this
  liquid and so errs in the safe direction.

## Standing decisions

**History actually available** (Tiingo, per `LOG.md`): SPY from 1993-01-29, EFA
from 2001-08-17, IEF from 2002-07-26, BIL from 2007-05-30, DTB3 from 1954. The
binding constraint is EFA's lookback, not IEF's inception: a 12-month window
ending 2002-08-30 opens 2001-08-29, which exists. So the first signal is
~2002-08-30 and the first execution the next session — earlier than the ~2003-07
estimate in `LOG.md`, which assumed the window had to open after IEF listed.

## Sample splits — PROPOSED, not yet locked

The excerpt's windows (1990–2009 / 2010–2017 / 2018–today) assumed the
mutual-fund splice that `LOG.md` closed on 2026-08-06. Under the all-ETF
universe the history runs 2002-09 → 2026-08, so they have to be re-drawn.

Drawn **now**, before any results exist, because a holdout is only worth
anything if it was chosen before anyone had seen a number. Deferring this was
acceptable while the grid was diagnostic; it stops being acceptable once the
objective is to maximise return over a 216-cell grid, because then the grid is a
search and the holdout is the only thing standing between a search and a fitted
result.

| window | period | signals | what is in it |
|---|---|---|---|
| **In-sample** — look freely | 2002-09 → 2014-12 | ~148 | 2003 recovery, 2007–09 bear, 2011 euro crisis |
| **Validation** — bounded looks | 2015-01 → 2019-12 | 60 | 2015 whipsaw, 2018 Q4 selloff, the 2010s bull run |
| **Out-of-sample** — one look, ever | 2020-01 → 2026-08 | ~80 | COVID crash and V-recovery, 2022 |

Why this cut and not another:

- **2008 has to be in-sample.** It is the episode the trend filter exists for,
  and developing blind to it means developing blind to the mechanism.
- **The out-of-sample window is deliberately hostile.** It holds both documented
  failure modes: the 2020 V-shaped crash, and 2022 — when IEF fell about 15% and
  the defensive leg failed to defend. A holdout that only contains easy years
  tells you nothing. This one can genuinely fail the strategy.
- **It is also the regime the money would actually be deployed into**, which is
  the question the whole project is asking.
- The cost is real: 6.6 years of the most recent and most relevant data cannot
  inform development at all. That is what a holdout costs, and paying it is the
  point.

**Operationally, what "one look" means.** The out-of-sample run is a single
execution of a single fully-specified configuration, with that configuration
committed to git *before* the run. If the result disappoints there is no
re-tuning: the strategy is rejected, or a new hypothesis is started and it needs
its own data. Anything else silently converts the holdout into a third
development set.

**Validation looks are bounded and recorded.** At most three, each logged in
`LOG.md` with the date, the configuration, and what was decided as a result.
Not because three is principled, but because an unrecorded "occasional" check is
indistinguishable from fitting, and a count that has to be written down is one
that gets noticed.

**The deployed configuration is pre-committed, not selected.** This is the part
that makes the in-sample grid safe. The configuration to be carried forward is
fixed now:

> 12-month lookback, last trading day of month, full universe
> (SPY/EFA/IEF/DTB3), `month_end` anchor, Tiingo, 0.1% per side.

The grid's job is to show that performance degrades *gracefully* around that
point — it is a stability check, not a search for the best cell. If the
pre-committed cell is poor and some other cell is excellent, that is a finding
about fragility and an argument against the strategy, not a licence to move the
configuration. Choosing the deployed cell by in-sample rank would contaminate
validation before it was ever used, and the arithmetic in the next paragraph is
why that matters.

**Why the discipline, quantitatively.** The standard error on an annualised
Sharpe from ~24 years of monthly returns is roughly 0.2. Selecting the best of N
noisy cells inflates the winner by about `SE × sqrt(2 ln N)`: ~0.7 Sharpe at
N = 216 if the cells were independent, ~0.5 with the correlation they actually
have. That is the same order as the entire edge being claimed — and it is
invisible to every check in section 9, because nothing is broken. The
calibrated-expectations trigger inherited from the excerpt would fire on the
best cell, and the cause would be the grid, not a bug.

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

3. **The lookback anchor convention is a parameter, not a decision.** Where the
   lookback window opens can be defined two ways, and the backtest implements
   both:

   - **`month_end`** — take the *month* N months back (`chr::year_month` minus
     `chr::months`, which is exact and total, so no day-of-month is carried and
     nothing needs clamping) and use that month's last trading day. Defined only
     for the month-based rebalance rules.
   - **`fixed_days`** — go back `round(N * 365.2425 / 12)` calendar days and snap
     to the last trading day on or before. Defined for every rebalance rule,
     weekly included, which `month_end` is not.

   Measured on the real data over 289 month-ends (2001-08 → 2026-08, SPY/EFA with
   a DTB3 hurdle), the two disagree about the *anchor date* constantly but about
   the *holding* rarely:

   | lookback | anchors differ | holding differs |
   |---|---|---|
   | 3 months | 57% | 10 / 298 (3.4%) |
   | 6 months | 76% | 13 / 295 (4.4%) |
   | 9 months | 72% | 6 / 292 (2.1%) |
   | 12 months | 38% | 4 / 289 (1.4%) |

   The disagreements are all near-ties, and they land where near-ties land: at
   12 months one of the four is 2020-04-30, which decides whether May 2020 is
   held in IEF or SPY. So the convention is worth carrying as an axis rather than
   settling by argument — it perturbs holdings at the same order of magnitude as
   a provider disagreement, and for the same reason.

   Anchor resolution happens once, on dates every asset has a bar for — not per
   asset. Snapping per symbol would let SPY and EFA open their windows on
   different dates whenever one has a holiday the other does not, which makes
   relative momentum compare unequal windows.

## 1. Load

4. Load each equity history **once** at `as_of` via `equity_history()` and slice
   locally. Per `MarketData.h`, every call re-reads and re-parses the payload;
   stepping an as-of forward day by day re-parses megabytes per step.
5. Load `DTB3` via `rate_history()`. Optionally also `DGS3MO` — the same bill
   quoted bond-equivalent instead of discount — as the check on step 7.
6. Assert each history covers the full test window, and that the first bar is
   early enough for the first lookback to open.

## 2. Derived series

7. **Cash total-return index from DTB3.** Built, as `cash_return_factor()` in
   `atlib/marketdata/total_return.h`. The discount quote is converted to what the
   bill actually earns (`price = 1 − d·91/360`, `yield = d/price`) and accrued at
   `yield/360` per *calendar* day, forward-filled across FRED's holidays because
   the gaps are days nobody published, not days money stopped earning. Skipping
   the basis conversion would understate the hurdle by ~7bp a year at a 5% level,
   one-directionally, biasing the strategy toward equities.

   **The dominant modelling choice is the roll, not the basis.** The index rolls
   daily at the spot three-month quote rather than buying a bill and holding it to
   maturity, which is what published T-bill indices measure. Against those: 2021
   agrees to a basis point, 2023 runs ~34bp rich, 2007 ~24bp cheap, 2022 ~58bp
   rich as rates went 0.05% → 4.4%. The error changes sign with the direction of
   rates and is an order of magnitude larger than the basis correction.

   Kept because it is the only construction that is *windowable* — a real bill
   roll depends on when its cycle started, so a factor between two arbitrary dates
   would not be well defined and adjacent windows would not chain, which is what
   lets the cash leg be compared against the equity legs over identical windows at
   all. The residual is a hurdle bias of up to ~50bp, in the number the
   absolute-momentum filter subtracts, where near-ties decide the holding.

   **This is a candidate axis for the robustness grid**, on the same argument as
   the anchor convention in step 3: swap the DTB3 roll for BIL's own total return
   over 2007→today and diff the holdings. If the conclusion is insensitive, the
   choice was never decision-relevant.

   Checks run: BIL lags the index by 14.3bp in 2021 (ZIRP) against a 13.6bp
   expense ratio, matching what `LOG.md` predicted; 6.4bp in 2019 and 33.8bp in
   2023, the spread between them being the roll effect rather than anything about
   BIL. `DGS3MO` remains available as an independent check on the basis
   conversion, and is not yet wired up.
8. **Daily total-return chain per instrument.** `total_return_factor_close_to_close()`
   over each consecutive pair of bars, composed multiplicatively. This is the
   mark-to-market series for the equity curve; adjacent windows compose by
   construction because of the half-open boundary at `from`.
9. **Open-to-open factor for execution.** The held period runs open(exec) →
   open(next exec), which nothing in `atlib` computes today. The clean fix is a
   `total_return_factor_open_to_open()` sibling in `total_return.h`: same
   distribution and split walk, same half-open rule, different endpoints.
   Decomposing it out of the close-to-close function (intraday + close-to-close +
   overnight) also works but puts the dividend-boundary rule in two places, which
   is where the two will eventually disagree.

## 3. Rebalance schedule

10. **Signal dates** = the last date present in SPY's `bars` in each calendar
   month. The bar set *is* the trading calendar (`equity.h`), so no holiday table
   is involved. For the grid's "day 10 / day 15" variants: the last trading day
   on or before that calendar day.
11. **Execution date** = the next bar after the signal date. Assert
    `exec_date > signal_date` in the loop. That assertion is the lookahead
    control; it belongs in the code, not in a comment.

## 4. Decision, per rebalance

12. **Lookback window opens** at the last bar on or before (signal date − 12
    months), by calendar arithmetic — the convention already used in
    `playground/main.cpp`. Back rather than forward, so the window covers the
    whole period rather than falling short of it.
13. Compute the 12-month total-return factor for SPY, EFA and the cash index over
    that same window. Same endpoints for all three; equities step over bars, cash
    over calendar days.
14. **Relative momentum:** winner = max(SPY, EFA). **Absolute momentum:** if the
    winner's factor exceeds the cash factor, hold the winner; otherwise hold IEF.
15. Two tie-breaks, written down now and not revisited: SPY and EFA exactly equal
    → SPY. Winner exactly equal to the hurdle → IEF, because "exceeds" is strict.
16. **Record the whole decision row**, not just the holding: signal date, all
    three factors, winner, hurdle outcome, target, prior holding, execution date,
    execution price. This is what makes the cross-provider holdings diff
    (`LOG.md`, 2026-08-04) localizable to specific months for free.

## 5. Accounting

17. If the target equals the current holding, **no trade**: no turnover, no cost,
    and it does not count toward the trade count.
18. On a switch, apply 0.1% on the sell and 0.1% on the buy, multiplicatively, at
    the execution open — 0.2% per round trip.
19. Advance equity daily using the step-8 chain. The curve is therefore marked at
    closes while trades happen at opens; state that asymmetry, because the
    drawdown series is close-based.
20. Emit three artifacts: the **daily equity curve**, the **monthly holdings
    vector**, and the **switch sequence** (date, from, to).
21. `atlib/papertrading` already has a `Portfolio` and
    `buy_or_sell_equities_by_money()`. A 100%-one-asset strategy does not need
    them, but reusing them keeps the backtest's fill path and any later live path
    the same shape.

## 6. Benchmarks

22. **Buy-and-hold SPY** — same start, same daily marking, one entry cost.
23. **60/40 SPY/IEF**, rebalanced monthly at the same execution timing, with the
    same 0.1% per side on the traded amount. The rebalancing rule matters to the
    result and no source fixes it, so it is chosen here and stated rather than
    left implicit.

## 7. Metrics

24. One table, strategy and both benchmarks: CAGR, max drawdown, Sharpe (risk-free
    = the step-7 cash index, not zero), worst rolling 12-month return, number of
    trades, total cost paid.
25. **Named-episode check.** Did it hold IEF through 2008, and what did it do in
    Aug 2015, Feb–Apr 2020 (whipsaw expected) and 2022? These are the episodes
    this class of strategy is known to handle well and badly, so they are the
    cheapest available test that the implementation behaves like the thing it
    claims to be. If 2020 comes out clean, suspect a defect before suspecting an
    edge. Note the last two fall in the out-of-sample window, so this check runs
    in full only once — see Sample splits.

## 8. Robustness grid

26. Axes: lookback {6, 9, 12} × rebalance day {month-end, 10, 15} × asset dropout
    {none, −EFA, −IEF} × anchor convention {month_end, fixed_days} (step 3)
    × provider {tiingo, + EODHD when it lands}. Same start date in every cell
    (step 2).
27. Report the full grid, never the best cell. The pass condition is graceful
    degradation across cells; a single good cell is noise.
28. Run it on in-sample data only (2002-09 → 2014-12). It is a stability check
    around the pre-committed configuration, not a search — see Sample splits for
    why that distinction is the one doing the work.

## 9. Correctness checks

These are tests, not a one-off script.

29. Lookahead assertions in the loop: every date read for a decision ≤ the signal
    date; every execution date > it.
30. Reconstruct buy-and-hold SPY from the daily chain and compare against a single
    full-history `total_return_factor_close_to_close()` call. They must agree to
    floating-point noise. Catches chaining and boundary errors in one line.
31. One hand-computed month: all three factors, the decision, and the post-cost
    equity checked against numbers worked out by hand.
32. The EFA 3-for-1 (2005-06-09) and BIL 1-for-2 reverse (2017-11-30) must be
    transparent to the open-to-open factor. `LOG.md` verified them against
    `vendor_adj`, but step 9 is new code that re-derives them.
33. Cost identity: total cost paid == number of trades × 0.2% of equity at each
    execution.

## Known costs of this plan

- **Step 9 is the only real new library work**, and it is not optional. Executing
  at the signal-day close instead would be lookahead and would defeat the rule
  the next-open execution rule exists to prevent.
- **The pre-2002 window is gone** under the current data. The history covers 2008,
  2020 and 2022 but not 2000 or 1987, so regime diversity is thinner than the
  excerpt assumed when it was counting on a mutual-fund splice. That is an input to the deferred split decision,
  not a reason to delay the first working run.
