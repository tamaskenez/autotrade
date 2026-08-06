# Work log — p001 dual momentum, fixed ETF basket

Entries newest first. Each session gets a dated heading.

---

## 2026-08-06

### Scope reductions

**Mutual fund NAVs dropped.** Open-end funds price once daily at NAV, so they
have no candle and no volume — a "daily OHLC" for VFITX from a vendor is either
the same number four times, or a *monthly* aggregate of daily NAVs (which is what
Alpha Vantage's `TIME_SERIES_MONTHLY` returns, and why it looked like real OHLC).

More importantly they are only needed if we extend history before the ETFs exist.
The all-ETF universe binds at IEF (~Jul 2002), so with a 12-month lookback the
first tradeable signal is ~Jul 2003 — about 23 years to today, containing 2008,
2020 and 2022. Splicing funds would buy ~1992–2002 at the cost of an
actively-managed international leg. Given the stated preference for consistent
quality over reach, the fund path stays closed and `fetch_fund_navs.py` is not
being built.

**BIL is not a signal input.** Re-reading the rules: cash is never *held* — the
strategy holds the equity winner or IEF, and the cash proxy appears only as the
hurdle in the absolute-momentum comparison. So DTB3 covers the hurdle for the
whole history, and BIL is worth fetching only as the overlap check that validates
the DTB3 discount-basis conversion.

Leaves two fetch scripts: `fetch_equity_history.py` and `fetch_rates.py`.

**No restatement detection.** The cache and the store are conveniences, not a
data warehouse. Cost: if a vendor silently revises history, a backtest result
changes with no record of why. Accepted, because the multi-provider diff is the
real safety net.

Consequence: since we always pull full history and upsert, `fetch_id` would be
constant across a `(provider, instrument)` series — functionally dependent on the
key, so storing it per row was pure denormalisation. Replaced by an `ingest`
table holding one `fetched_at` per instrument per provider, taken at the start of
a multi-request operation so every table from that operation shares a timestamp.

### Decisions

- `src/marketdata/`, not under p001 — nothing in the data layer knows what a
  strategy is. **Instruments are parameters throughout**; the universe is a
  backtest config concern, and `instruments.toml` is a registry of what we know
  how to fetch, not a universe.
- uv + Python 3.14, standalone, no CMake coupling.
- `schema.sql` is the shared contract, applied by `init_db.py`, read-only from C++.
- Credentials: `$TIINGO_API_KEY` first, then `~/.config/autotrade/credentials.toml`.
  Outside the repo so secrets cannot be committed by construction rather than by
  gitignore discipline. Key goes in the `Authorization` header, never a query
  parameter — otherwise it lands in archived payloads and logs.
- Raw cache `_md/raw/{provider}/{symbol}.{ext}`, newest payload only, cache-first,
  `--refetch` to force. Written via a temporary file so an interrupted download
  cannot leave a truncated payload that a later run treats as a cache hit.
- Full history always; no `--from`/`--to`.
- Unknown symbols are rejected rather than auto-registered, so a typo cannot
  silently become data.
- Within-batch duplicate check retained: a provider contradicting itself inside
  one response is always a bug.

### Built

`src/marketdata/` — `schema.sql`, `instruments.toml`, `init_db.py`,
`fetch_equity_history.py`, `db.py`, `model.py`, `config.py`,
`providers/tiingo.py`, 23 tests. Verified end-to-end by seeding the cache with
the test fixture.

### First real pull — Tiingo, SPY/EFA/IEF/BIL

Full history, no truncation on the free tier. Raw and vendor_adj both present:

| symbol | first | last | bars |
|---|---|---|---|
| SPY | 1993-01-29 | 2026-08-05 | 8436 |
| EFA | 2001-08-17 | 2026-08-05 | 6277 |
| IEF | 2002-07-26 | 2026-08-05 | 6045 |
| BIL | 2007-05-30 | 2026-08-05 | 4827 |

Validation results:

- **Structure clean** — no null or non-positive closes, no inverted high/low, no
  close outside its own high/low range, no bad volumes.
- **Calendar gaps are all real closures**: 9/11 (Sep 11–14 2001), Hurricane Sandy
  (Oct 29–30 2012), and Jan 2 2007 (national day of mourning for Gerald Ford).
  Nothing unexplained.
- **Splits: 2 events, both directions verified.** EFA 3-for-1 on 2005-06-09 (raw
  157.00 → 52.54, adjusted volume exactly 3× pre-split) and BIL 1-for-2 *reverse*
  on 2017-11-30 (raw 45.74 → 91.48). `vendor_adj` is continuous across both.
  SPY correctly has none — it has never split, and still trades at ~1/10 of the
  index level as it did in 1993.
- **Distributions** match expected frequencies: SPY quarterly (135 since 1993),
  EFA semi-annual (46), IEF monthly (288). BIL's missing years — 2010, 2012–2015,
  2021 — are ZIRP: T-bill yields sat below the fund's expenses so there was no
  net income to distribute. Real behaviour, not missing data. Note for the
  BIL/DTB3 overlap check: during those years BIL should *underperform* a
  DTB3-derived index by more than just its expense ratio.
- **Corporate actions are complete.** Reconstructing the adjusted series from raw
  prices plus our stored distributions and splits tracks Tiingo's own `adjClose`
  to within 1e-3 over 33 years, and the vendor's adj/raw ratio drifts by only
  4e-12 on non-event days — i.e. it steps *only* on ex-dates we already have. A
  missing dividend would show up as an unexplained step; none do.

**Residual convention difference, deliberately not chased.** Per-event
adjustment factors differ from ours by ~2e-5 (median), largest on the most
volatile days. Tested prev-close, ex-date close and ex-date open as the
reference price; none explains it, so it is something finer in Tiingo's
methodology. Impact on a 12-month total return is ~1e-4, i.e. two to three
orders of magnitude below any momentum spread that could flip a holding
decision. Below the threshold, so it gets no forensics — the cross-provider diff
will characterise it better than speculation would.

This is the payoff for storing raw + distributions rather than consuming
`adjClose`: whatever nonstandard convention is baked into the vendor's adjusted
series, we don't inherit it.

**Also noted:** Tiingo reports EFA's split factor as `3.000003`, not `3`. It
appears to derive the factor from prices rather than reporting the declared
ratio, so the num/den storage cannot recover exactness from a lossy input. Worth
diffing against EODHD, which may report the declared ratio.

### Next

EODHD as the second provider, then the cross-provider diff.

---

## 2026-08-04

### Status

`README.md` holds the strategy spec, produced in an earlier LLM session. It is a
starting point, not binding — anything in it may be overridden. No code exists yet;
`src/common/` and `src/p001_dual_mom_fixed_etf/` are empty except for stub
`CMakeLists.txt` files.

Agreed: **build the data layer first**, and stop for review before any strategy code.
A lookahead or dividend bug at the data layer invalidates everything downstream, and
the protocol allows only one look at the out-of-sample set.

### Decisions made this session

**Provider choice is deferred — deliberately.** Rather than picking one vendor and
reconciling its discrepancies, run the *entire backtest independently on each
provider's data*. Data source becomes a fourth axis of the robustness grid alongside
lookback, rebalance day, and asset dropout. If the conclusion is insensitive to which
vendor supplied the data, the vendor choice was never decision-relevant.

**Diff the decision sequence, not the metrics.** Compare the monthly holdings vector
(which asset held each month) across provider runs — not just CAGR/drawdown. Rationale:
the strategy is a discrete switch driven by comparing two nearly-equal numbers, so when
SPY's 12-month return is within basis points of EFA's or of the cash hurdle, a trivial
data difference flips the position for a whole month. Small input differences produce
occasional *large* output differences, not small ones. Metric-level comparison is
insensitive in one direction (runs can agree on CAGR by luck while making different
decisions) and misleading in the other (one flipped month can move CAGR without
indicating any data problem).

Holdings-level diffing also localizes any investigation for free: you get the exact
months where providers disagreed and look only at those.

**Pre-committed threshold: any holdings disagreement gets investigated.** Set before
seeing results so it can't become a knob to turn. Expected to be a handful of months
out of ~240; each is a few minutes to check once localized.

**A null result here is itself a finding.** If all providers produce identical holdings
across the full history, that establishes the signal is not knife-edge sensitive —
information no amount of data reconciliation would have produced.

**Keep one cheap structural check anyway.** Outcome comparison is blind to *systematic*
errors shared across providers (cheap vendors often resolve to the same upstream feeds,
so agreement can only fail to invalidate — it cannot validate). The floor against this
is not forensics but a checksum: total distributions per ticker per year vs. the
issuer's published file. A few dozen numbers, ~5 minutes. Plus basic well-formedness:
no missing bars, no stale repeated prices, no ex-dates off by more than a couple of days.

**In-sample start date: still open.** User prioritizes continuously consistent,
high-quality data over reaching as far back as possible. Decision deferred until we have
hands-on experience with the actual data, since the point where data stops being
trustworthy is the real input to this choice.

### Data providers evaluated

| Provider | Cost (2026, verify) | Verdict |
|---|---|---|
| **Tiingo** | free tier 250 calls/day; ~$22/mo annual | **Primary candidate.** `adjOpen` is a first-class field (needed for next-open execution — many vendors publish adjusted *close* only). Covers ~33k US mutual fund NAVs back to the 1970s, needed for any pre-2002 splice. Cleaning process explicitly combines multiple feeds to catch missing splits/distributions. |
| **EODHD** | free tier 20 calls/day; EOD All-World ~$20/mo | **Cross-check.** Chicago Booth adjustment algorithm at 4 decimal places; exposes unadjusted dividend values and raw/split-only/fully-adjusted variants, so the adjustment arithmetic can be checked rather than trusted. Gotcha: negative `adj_close` on some high-dividend tickers breaks ratio-derived OHLC — assert against it. |
| **IBKR** | free (account already held, no subscriptions) | **Third vote.** Genuinely independent feed (own tape, not a reseller of the same upstream as the cheap vendors), which is its main virtue. Limitations: no historical corporate-actions endpoint, so its `ADJUSTED_LAST` series cannot be audited; no usable mutual fund NAV history (abstains on any pre-2002 window); bars are filtered for trade conditions so they are not the official tape; socket API against a running TWS/IB Gateway, ~60 historical requests per 10 min; unclear which historical requests work without market data subscriptions — test empirically. |
| **FRED** | free | Authoritative for the T-bill leg (`DTB3`) — it *is* the source (Treasury H.15). Never pay for this. |
| **Sharadar** (SEP/SFP) | pricing no longer public | Fallback if the multi-provider approach proves unwieldy. Institutional-grade curation; history believed to start ~1998 (verify). |
| **Norgate** | ~$630–790/yr (Platinum/Diamond) | **Rejected.** Windows-only (proprietary local DB + plugins) — would need a VM purely to export CSVs from a macOS/C++ project. Its value-add (delisted securities, point-in-time index constituents, survivorship bias) is worth a lot for a *screening* universe and nothing for a fixed 4-ETF basket. |
| **Alpha Vantage** | — | **Rejected**, prior experience: dividend errors. Structurally a thin aggregation layer that *computes* adjusted series with no reconciliation step. |
| Polygon, Intrinio | — | Built for intraday/breadth; EOD history doesn't reach far enough back. |

**Ground truth for distributions:** SSGA (SPY) and iShares (EFA, IEF) publish complete
distribution histories and official NAV total-return series, free. With a fixed
4-ticker universe, 100% of corporate actions can be audited against issuer records —
a luxury most backtests don't have.

**Licensing:** Tiingo and EODHD personal tiers are internal-use, non-redistributable.
Raw vendor files must be gitignored, not committed. Commit ingestion code and checksums;
reproducibility comes from the manifest, not from vendoring the data.

### Problems found in the spec (not yet resolved)

**The ETFs don't exist for most of the spec's 1990–2009 in-sample window.**
Approximate inceptions: SPY ~Jan 1993, EFA ~Aug 2001, IEF ~Jul 2002, BIL ~May 2007.
The spec names a proxy only for the US leg (VFINX). Available substitutes:

- US equity — VFINX (~1976). Clean index fund, no problem.
- Cash — FRED `DTB3`, daily, back to 1954. Clean.
- Intermediate treasuries — VFITX (~Oct 1991), or synthesize from FRED constant-maturity
  yields.
- International equity — **the weak link.** No passive EAFE tracker exists before the
  mid-90s. All candidates (Vanguard International Growth, T. Rowe Price International
  Stock, Fidelity Diversified International ~Dec 1991) are *actively managed*, so
  pre-2001 "EFA" is really some manager's international book. Relative-momentum
  decisions in that era would partly reflect manager skill and style drift, not the
  asset class.

**"Execute at next day's open" is not representable in the pre-ETF era.** Mutual funds
have a single daily NAV, no open/close. Execution there has to be next-day NAV. Still
lookahead-free, but the rule must say so explicitly rather than silently changing
meaning mid-backtest.

**Splice on returns, not prices** — chain daily returns at the handover date. Verify via
correlation/tracking over the fund-vs-ETF overlap window (e.g. VFITX vs. IEF, 2002–2009).
That overlap check is a cheap, strong bug detector and belongs in the test suite, not in
a one-off script.

**FRED `DTB3` is quoted on a secondary-market discount basis**, not bond-equivalent
yield. Converting it to a daily total return for the cash leg needs care — easy place to
introduce a small persistent bias in exactly the quantity the absolute-momentum filter
compares against.

### Next steps

1. Design the storage schema for multi-provider data (in progress).
2. Build the loader with a **source abstraction** — "which feed" should be a parameter,
   not baked in. Wanted anyway for eventual live IBKR signal computation.
3. Pull SPY, EFA, IEF, BIL daily bars from Tiingo (free tier), EODHD (free tier), and
   IBKR (`ADJUSTED_LAST` plus unadjusted `TRADES`). This also settles empirically whether
   unsubscribed IBKR historical requests work for these ETFs at all — cheaper to test
   with a short script than to reason about from the docs.
4. Ingest issuer distribution files (SSGA, iShares) as ground truth, and FRED `DTB3`.
5. Run the annual distribution checksum and well-formedness checks per provider.
6. Then, and only then, the strategy code — run per provider, diff holdings vectors.

### Longer-term note

If this goes live, IBKR is the broker. Reconciling the live signal path against the same
source used to trade removes a class of backtest-vs-live discrepancy. The source
abstraction in step 2 is what keeps that option open.