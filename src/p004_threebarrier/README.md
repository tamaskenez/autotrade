# AI chat excerpt

 I'd like to ask about another idea for a stock trading algorithm: Say, we construct an algorithm that takes a week's price data (from monday open to monday open) and
  predicts, that on the next week the price will be at least temporarily go 1% above that monday's open. At each monday open we run this prediction on a huge number of stocks
  and pick the most promising ones and open a position with take-profit.
  More precisely, we'd like to predict the probability that the price will go above x before going below y so we can open a position with a take profit at x and stop loss at
  y.
  Is this a viable direction?

  Read 1 file, listed 1 directory (ctrl+o to expand)

⏺ Read the p003 excerpt for context. Short answer: the labeling scheme is good and worth adopting; as a source of edge it's a mirage. The framing hides the fact that the entire
  family has zero expected value by construction, and that what your model will actually learn to predict is volatility.

  The structural problem

  For a driftless random walk with an up-barrier at $x$ and a down-barrier at $y$, gambler's ruin gives

  $$P(\text{hit } x \text{ first}) = \frac{y}{x+y}$$

  and therefore

  $$EV = \frac{y}{x+y}\cdot x - \frac{x}{x+y}\cdot y = 0$$

  Exactly zero. Not approximately. And adding the time limit (the vertical barrier at next Monday's open) doesn't change it — optional stopping holds for any bounded stopping
  time. Every choice of $x$, $y$, and horizon has EV zero. The barrier geometry is a pure risk/frequency knob, not a return knob.

  This means the probability you're proposing to predict is not the thing that makes money. Only your deviation from $y/(x+y)$ is. And here's the trap: a model can have
  excellent AUC and excellent calibration on this target while having a deviation of exactly zero — because nearly all the discriminative power in "will it hit ±1% this week"
  is volatility, which is highly predictable (IC ~0.7 for realized vol, trivially) and completely EV-neutral.

  You'd build something that looks like it works, validate it honestly, and it would be a volatility forecaster wearing a costume.

  The single-barrier version is worse

  "At least temporarily go 1% above Monday's open" without a stop is the seductive one. Typical stock weekly vol is ~4%. By the reflection principle, for driftless BM:

  $$P(\max > 1%) = 2\Phi(-0.25) \approx 0.80$$

  An 80% hit rate with no model at all. You will find configurations with 85–90% "accuracy," and the 10–15% of weeks that miss will be down 5–10%. That's a classic payoff-shape
  illusion — it's writing an option and calling it a signal.

  Tight barriers destroy the cost budget

  This one is quantitative and I think decisive for the 1% version specifically. With $x = y = 1%$ and round-trip cost $c$:

  $$EV = (2p - 1)\cdot 0.01 - c$$

  At $c = 8$ bps (liquid large caps, spread + commission + a little slippage), you need $p > 0.54$. A 54% hit rate on a weekly coin flip is a far larger edge than anything
  documented in the equity literature. On the high-vol small caps that look most attractive here, spreads are 20–50 bps and the required $p$ goes past 0.6 — hopeless.

  Widen to $x = y = 5%$ and you only need $p > 0.508$. The cost-to-edge ratio scales as $c/(2x)$, so tight barriers are the expensive choice. If you pursue this, barriers
  should be ~1–2 weekly sigma, and — importantly — scaled per asset, not fixed at 1%. A fixed 1% is a 3-sigma move for a utility and a 0.2-sigma move for a biotech; pooling
  those is pooling two unrelated experiments.

  Two implementation problems that bite hard

  Intrabar ordering. With daily OHLC you cannot tell whether the high or the low came first. On a day where low < y and high > x, the label is genuinely undetermined. At 1%
  barriers this is not an edge case — it's a large fraction of your labels. The standard convention is to assume the adverse order (stop hit first), which is honest but
  pessimistic; assuming favorable order manufactures a large fake edge. Either way the choice moves the backtest materially, which tells you the result is dominated by an
  artifact. Tight barriers on daily bars is the wrong combination; you need intraday data or wider barriers.

  Gaps break the payoff symmetry. The take-profit is a limit order and fills at $x$. The stop is a stop order and fills at $y$ or worse. Overnight gaps past 1% are common. So
  your realized distribution is $+x$ / $-(y + \text{gap})$ — a structural negative drift stacked on top of costs, and it's largest exactly in the high-vol names the model will
  select. Backtests that fill stops at the stop price systematically overstate this strategy class.

  What I'd actually do

  Change the target from the outcome to the payoff. Instead of predicting $P(\text{hit } x \text{ first})$, regress directly on the realized barrier payoff: $+x$, $-y$, or the
  residual return if the week times out. Now the null hypothesis is a prediction of zero, the metric is expected value, and there's no base rate for a volatility forecast to
  hide inside. This is a small change that converts an unfalsifiable classification problem into a measurable one.

  Run the null test first — it's a day's work and it's decisive. Over your universe and history, with vol-scaled barriers, compute the unconditional realized EV of the barrier
  trade, bucketed by trailing vol. Include the adverse-ordering convention and a gap model. If the hit rates track $y/(x+y)$ once you condition on vol (they will, to within
  noise), you've established that the whole family is EV-zero before costs, and any model you build afterwards has a clearly defined bar to clear. If they don't track it,
  you've found something genuinely interesting — a real deviation from the martingale — and that's worth chasing hard.

  Keep the triple barrier, but as an exit rule. This is López de Prado's construction from Advances in Financial Machine Learning, and the reason he advocates it is real: it
  removes the arbitrary fixed holding period from your labels and caps tail losses. That's a legitimate improvement to p003's harness. But it belongs downstream of a return
  predictor, not as the predictor.

  Verdict

  Not viable as stated — the 1% weekly barrier version fails on cost arithmetic alone, before any of the modeling questions. But it's not a wasted idea: the triple-barrier
  labeling is a genuine upgrade to the p003 plan's fixed-$k$-day target, and the null test above is cheap and settles the question either way.

  The uncomfortable through-line with p003 is that both ideas reduce to the same problem — predicting drift — and this one adds path dependence, intrabar ambiguity, and gap
  risk on top. I'd finish the p003 measurement project first (ridge, cross-sectional target, purged walk-forward, OOS IC with error bars), then layer barrier-based exits onto
  whatever signal survives.

✻ Crunched for 1m 35s

❯ This is not related to p003 in any way. I'm collecting ideas. What's the triple barrier?

⏺ Fair — I dropped the term without defining it. It's a labeling technique from Marcos López de Prado's Advances in Financial Machine Learning (2018), chapter 3. Your setup is
  essentially it, arrived at independently.

  The construction

  You're trying to answer: "what label do I attach to this moment in time, for supervised learning?" The triple barrier says: from the entry point, draw three lines and see
  which one price touches first.

  price
    ^
    │ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─  upper barrier  → label +1
    │                    ╱╲      ╱
    │        ╱╲    ╱╲  ╱   ╲   ╱
    │  ●───╱───╲──╱──╲╱     ╲─╱
    │ entry     ╲╱                          │
    │                                       │  vertical barrier → label 0
    │ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┴  lower barrier  → label −1
    │
    └──────────────────────────────────────────> time

  1. Upper horizontal — profit target
  2. Lower horizontal — stop loss
  3. Vertical — a maximum holding period

  The label is whichever gets touched first. Usually {+1, −1, 0}, though for the timeout case you can use the sign of the return at the vertical barrier instead of 0, or just
  use the realized return as a continuous label.

  What it fixes

  The default alternative is fixed-horizon labeling: label = sign(return over next k bars). Three things go wrong with that:

  - k is arbitrary. Nothing about the market says 5 days.
  - It ignores the path. A trade that goes −15% and crawls back to +0.5% gets labeled a winner. In reality you'd have been stopped out, or margin-called, or you'd have closed
    it out of sheer nerve. You are training the model on an outcome you would never have experienced.
  - It doesn't match how positions actually work. Real positions have exits. The label should describe what the position does, not what the price series does.

  The triple barrier makes the label path-dependent in the same way a real position is.

  The detail that matters most

  The horizontal barriers are set as multiples of trailing volatility estimated at entry time, not as fixed percentages. So it's "1.5 sigma up, 1 sigma down," where sigma is,
  say, an EWMA of daily returns at that moment.

  This is what makes the labels comparable across assets and across regimes. A fixed 1% barrier is a different event for a utility than for a biotech, and a different event in
  2017 than in March 2020 — pooling them pools unrelated experiments. Vol-scaling normalizes that away, and it's cheap to do.

  The up/down multiples don't have to be equal, and setting them asymmetrically is how you encode a directional view in the label geometry.

  What it comes packaged with

  The book presents it alongside several pieces that address problems the barrier method itself creates:

  - Meta-labeling (§3.6) — two stages. A primary model picks the side (long/short); a secondary model, trained on "did the primary model's bet win," decides whether to take the
    bet at all and at what size. The insight is that filtering and sizing is an easier learning problem than direction, and you can bolt it onto an existing signal you already
  The book presents it alongside several pieces that address problems the barrier method itself creates:

  - Meta-labeling (§3.6) — two stages. A primary model picks the side (long/short); a secondary model, trained on "did the primary model's bet win," decides whether to take the bet at all and at what size. The insight is
    that filtering and sizing is an easier learning problem than direction, and you can bolt it onto an existing signal you already trust.
  - Sample uniqueness / concurrency (ch. 4) — barrier windows overlap, so labels aren't IID and effective sample size is far below row count. He weights samples by how much their windows overlap others'.
  - Purged K-fold CV with embargo (ch. 7) — because each label spans a forward window, a naive train/test split leaks the test period into training. Purge training samples whose windows overlap the test block; embargo a few
    bars after it.
  - Event-based sampling (§2.5, CUSUM filter) — don't label every bar; label only when something notable happened, which reduces the redundancy problem at the source.
  - Fractional differentiation (ch. 5) — making price series stationary while retaining memory, rather than throwing all of it away by taking returns.

  The book is worth reading directly for chapters 2–7; the later ML-specific material is more variable in quality. The core labeling idea is genuinely standard now and shows up well outside his framework.

