-- Market data store.
--
-- Applied by init_db.py. Written by the fetch_* scripts, read by the C++ backtest.
--
-- Conventions:
--   * dates      TEXT 'YYYY-MM-DD'
--   * timestamps TEXT ISO-8601 UTC ('YYYY-MM-DDTHH:MM:SSZ')
--   * prices     REAL; a double carries far more precision than any quoted price
--   * what the provider said is the source of truth. Adjusted series, spliced
--     series and total returns are derived downstream and never stored here.
--
-- Nothing in this schema knows about any particular strategy or universe.
-- Instruments are just instruments; a backtest picks its universe by symbol.

PRAGMA journal_mode = WAL;
PRAGMA user_version = 1;

-- Data sources. Issuers (ssga, ishares) and official sources (fred) are
-- providers too, so reconciliation is a uniform provider-vs-provider query
-- with no special case for ground truth.
CREATE TABLE IF NOT EXISTS provider (
  provider_id INTEGER PRIMARY KEY,
  name        TEXT NOT NULL UNIQUE,
  kind        TEXT NOT NULL,          -- vendor | issuer | official
  notes       TEXT
) STRICT;

CREATE TABLE IF NOT EXISTS instrument (
  instrument_id INTEGER PRIMARY KEY,
  symbol        TEXT NOT NULL UNIQUE, -- our canonical name
  kind          TEXT NOT NULL,        -- equity | fund | rate
  currency      TEXT NOT NULL DEFAULT 'USD',
  description   TEXT
) STRICT;

-- Providers disagree on identifiers: FRED uses series ids, IBKR wants
-- conId + exchange. `extra` is free-form JSON for whatever a provider needs
-- beyond a symbol string.
CREATE TABLE IF NOT EXISTS provider_symbol (
  provider_id   INTEGER NOT NULL REFERENCES provider,
  instrument_id INTEGER NOT NULL REFERENCES instrument,
  symbol        TEXT NOT NULL,
  extra         TEXT,
  PRIMARY KEY (provider_id, instrument_id)
) STRICT, WITHOUT ROWID;

-- Data vintage, one row per instrument per provider. Set once at the start of
-- an ingest operation, so every table filled from that operation shares a
-- timestamp even when the provider needed several requests.
CREATE TABLE IF NOT EXISTS ingest (
  provider_id   INTEGER NOT NULL REFERENCES provider,
  instrument_id INTEGER NOT NULL REFERENCES instrument,
  fetched_at    TEXT NOT NULL,
  PRIMARY KEY (provider_id, instrument_id)
) STRICT, WITHOUT ROWID;

-- Daily candles.
--
-- series_kind:
--   raw        unadjusted, as quoted. The input to everything.
--   vendor_adj the provider's own adjusted series. Cross-check only, never a
--              backtest input -- an adjusted price is a property of
--              (instrument, date, as-of), so these rows mean "as of the
--              matching ingest.fetched_at" and nothing more.
--
-- open/high/low/volume are nullable: some sources (e.g. IBKR ADJUSTED_LAST)
-- return close only.
CREATE TABLE IF NOT EXISTS bar (
  provider_id   INTEGER NOT NULL REFERENCES provider,
  instrument_id INTEGER NOT NULL REFERENCES instrument,
  series_kind   TEXT NOT NULL,        -- raw | vendor_adj
  d             TEXT NOT NULL,
  open          REAL,
  high          REAL,
  low           REAL,
  close         REAL NOT NULL,
  volume        REAL,
  PRIMARY KEY (provider_id, instrument_id, series_kind, d)
) STRICT, WITHOUT ROWID;

-- Cash distributions. Adjust on ex_date; reinvest at the ex-date close, which
-- is what index providers do.
--
-- kind is in the primary key because a fund can pay ordinary income and a
-- capital gain on the same ex-date (iShares does this most Decembers).
-- Vendors usually report a lump sum and land in 'unknown'; issuer files carry
-- the real breakdown. That asymmetry is what makes the reconciliation exact:
-- vendor lump sum vs. sum of issuer categories on the same ex-date.
--
-- 'qualified' is deliberately absent -- it is a portion of the ordinary
-- dividend rather than a separate payment, so a row for it would double count.
CREATE TABLE IF NOT EXISTS distribution (
  provider_id   INTEGER NOT NULL REFERENCES provider,
  instrument_id INTEGER NOT NULL REFERENCES instrument,
  ex_date       TEXT NOT NULL,
  kind          TEXT NOT NULL,        -- ordinary | st_cap_gain | lt_cap_gain
                                      -- | return_of_capital | unknown
  amount        REAL NOT NULL,
  record_date   TEXT,
  pay_date      TEXT,
  PRIMARY KEY (provider_id, instrument_id, ex_date, kind)
) STRICT, WITHOUT ROWID;

-- Shares multiply by num/den on the ex-date.
--
-- REAL rather than INTEGER because providers report a derived factor, not the
-- declared ratio: Tiingo gives EFA's 3-for-1 as 3.000003 and BIL's 1-for-2
-- reverse as 0.5, neither of which an INTEGER column would accept under STRICT.
-- So the pair does not recover exactness -- den is 1.0 for every Tiingo split,
-- which makes it carry no information today. Kept as a pair because EODHD
-- appears to report a ratio string; collapse to a single factor if it does not.
--
-- A missed split is a silent factor-of-N error in the price series.
CREATE TABLE IF NOT EXISTS split (
  provider_id   INTEGER NOT NULL REFERENCES provider,
  instrument_id INTEGER NOT NULL REFERENCES instrument,
  ex_date       TEXT NOT NULL,
  num           REAL NOT NULL,
  den           REAL NOT NULL,
  PRIMARY KEY (provider_id, instrument_id, ex_date)
) STRICT, WITHOUT ROWID;

-- Interest rates. Separate from `bar` because a yield is not a price: you
-- integrate it, you do not take ratios of it. Keeping them in one table invites
-- computing a trailing return by differencing a yield series, which produces a
-- plausible-looking number that is entirely wrong.
--
-- `convention` is mandatory so the quoting basis cannot be forgotten. FRED DTB3
-- is quoted on a bank discount basis (actual/360, against face value), which
-- understates the realised return by ~14bp at a 5% level -- a systematic bias in
-- exactly the quantity absolute momentum compares against.
CREATE TABLE IF NOT EXISTS rate (
  provider_id   INTEGER NOT NULL REFERENCES provider,
  instrument_id INTEGER NOT NULL REFERENCES instrument,
  d             TEXT NOT NULL,
  value         REAL NOT NULL,
  convention    TEXT NOT NULL,        -- discount_360 | bey_365 | simple_365 | cc
  PRIMARY KEY (provider_id, instrument_id, d)
) STRICT, WITHOUT ROWID;

-- ---------------------------------------------------------------------------
-- Backtest results. Not written by the marketdata scripts; the C++ side owns
-- these. They live here so the cross-provider comparison -- the actual test --
-- is a join rather than a script.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS run (
  run_id      INTEGER PRIMARY KEY,
  run_at      TEXT NOT NULL,
  git_commit  TEXT NOT NULL,
  provider_id INTEGER NOT NULL REFERENCES provider,  -- which feed fed this run
  config      TEXT NOT NULL,   -- JSON: universe, lookback, rebalance rule, costs
  data_asof   TEXT NOT NULL    -- max ingest.fetched_at across the inputs
) STRICT;

-- The decision sequence. Diffing this across providers is more sensitive than
-- comparing metrics, and it localises any disagreement to exact dates.
CREATE TABLE IF NOT EXISTS run_holding (
  run_id        INTEGER NOT NULL REFERENCES run,
  d             TEXT NOT NULL,        -- decision date
  instrument_id INTEGER NOT NULL REFERENCES instrument,
  PRIMARY KEY (run_id, d)
) STRICT, WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS run_metric (
  run_id INTEGER NOT NULL REFERENCES run,
  name   TEXT NOT NULL,
  value  REAL NOT NULL,
  PRIMARY KEY (run_id, name)
) STRICT, WITHOUT ROWID;
