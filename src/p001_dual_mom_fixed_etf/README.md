## Strategy: Dual Momentum on a fixed ETF basket

**Economic hypothesis (stated before touching data):** Cross-asset momentum persists over 6–12 month horizons due to investor underreaction and slow institutional capital flows, and a trend filter sidesteps the deepest equity drawdowns because bear markets are typically prolonged rather than instantaneous. This is one of the most robust documented effects across decades and asset classes (Moskowitz/Ooi/Pedersen time-series momentum; Antonacci's dual momentum), which matters — we're testing a known hypothesis, not mining for one.

**Universe (fixed, small, deliberately chosen before backtesting):**
- US large-cap equities (SPY for backtest history)
- Developed international equities (EFA)
- Intermediate treasuries (IEF) — the defensive asset
- Cash proxy (BIL, or 3-month T-bill rate for pre-2007 history)

The universe is small and fixed precisely to kill the selection-bias problem from your original methodology. No filtering by past performance, ever.

**Rules:**
1. On the last trading day of each month, compute trailing 12-month total return (dividend-adjusted) for SPY, EFA, and the cash proxy.
2. *Relative momentum:* pick the equity ETF with the higher 12-month return.
3. *Absolute momentum:* if that winner's 12-month return exceeds the cash proxy's 12-month return, hold it for the next month. Otherwise hold IEF.
4. 100% in one asset at a time, no leverage. Signal from month-end close; execute at the **next day's open** in the backtest — this ordering prevents lookahead bias.

Expected turnover is only 1–4 switches per year, so costs and slippage stay small — another deliberate choice.

## Backtest protocol

**Costs:** assume 0.1% per trade (each way) covering commission, half-spread, and slippage. That's pessimistic for liquid ETFs; good.

**Data:** you need dividend-adjusted total-return series. Yahoo adjusted close works to start (verify a few dividend dates by hand — Yahoo has known glitches). For history back to the 1980s, splice in mutual fund proxies (VFINX for S&P 500) so you can test across the 1987 crash, 2000, 2008 — regime diversity matters more than data precision here.

**Splits, locked in now:**
- In-sample (development, look at freely): 1990–2009
- Validation (check occasionally): 2010–2017
- Out-of-sample (locked, touch exactly once at the end): 2018–today

**Robustness grid (run on in-sample only):** vary the lookback across 6, 9, and 12 months; vary the rebalance day (month-end vs. day 10 vs. day 15 — momentum strategies have documented sensitivity to rebalance timing); drop each asset one at a time. You want performance to degrade *gracefully* across the grid. If only one cell looks good, that's noise.

**Metrics to report vs. two benchmarks (buy-and-hold SPY, and a 60/40 portfolio):** CAGR, max drawdown, Sharpe, worst rolling 12-month return, number of trades, and the full equity curve. Also report the *sequence* of switches — I want to see whether it dodged 2008 and got whipsawed in 2015/2020, because those are the known behaviors we're checking against.

## Calibrated expectations

Historically this class of strategy delivers roughly equity-like CAGR (7–10% nominal) with max drawdown in the 15–25% range instead of 50%+. Its known failure mode is whipsaw in fast V-shaped crashes (COVID 2020 was brutal for it) and multi-year underperformance vs. plain buy-and-hold during relentless bull runs like 2010–2019. If your backtest shows something dramatically better than this, suspect a bug (usually lookahead or dividend handling) before suspecting genius.

## Reports to generate

Report back with the in-sample equity curve, the metrics table vs. both benchmarks, and the robustness grid, and we'll decide whether it earns a look at the validation set.
