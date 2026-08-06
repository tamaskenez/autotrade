"""SQLite access for the market data store.

Writes are idempotent upserts: we always fetch full history, so re-running an
ingest overwrites a series with the provider's current view of it. Old values
are not kept -- the cache and the store are conveniences, not an archive.
"""

import sqlite3
import tomllib
from collections.abc import Iterable
from pathlib import Path

from model import Payload

MIN_SQLITE = (3, 37)  # STRICT tables

HERE = Path(__file__).parent
SCHEMA_PATH = HERE / "schema.sql"
REGISTRY_PATH = HERE / "instruments.toml"


class UnknownInstrument(Exception):
    pass


class DuplicateInBatch(Exception):
    """A provider contradicted itself inside a single response.

    Always a bug -- in our parser or theirs -- and never something to paper over
    by letting one row silently overwrite the other.
    """


def connect(path: Path) -> sqlite3.Connection:
    if sqlite3.sqlite_version_info < MIN_SQLITE:
        raise RuntimeError(
            f"SQLite {'.'.join(map(str, MIN_SQLITE))}+ required for STRICT tables, "
            f"have {sqlite3.sqlite_version}"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(path)
    conn.execute("PRAGMA foreign_keys = ON")  # per-connection, not persisted
    conn.row_factory = sqlite3.Row
    return conn


def apply_schema(conn: sqlite3.Connection) -> None:
    conn.executescript(SCHEMA_PATH.read_text())
    conn.commit()


# --------------------------------------------------------------------------
# Registry
# --------------------------------------------------------------------------


def load_registry(path: Path = REGISTRY_PATH) -> dict:
    with path.open("rb") as f:
        return tomllib.load(f)


def seed_registry(conn: sqlite3.Connection, registry: dict) -> None:
    """Apply instruments.toml to the database. Idempotent."""
    for p in registry.get("provider", []):
        conn.execute(
            """INSERT INTO provider (name, kind, notes) VALUES (?, ?, ?)
               ON CONFLICT (name) DO UPDATE SET
                 kind = excluded.kind, notes = excluded.notes""",
            (p["name"], p["kind"], p.get("notes")),
        )

    for i in registry.get("instrument", []):
        conn.execute(
            """INSERT INTO instrument (symbol, kind, currency, description)
               VALUES (?, ?, ?, ?)
               ON CONFLICT (symbol) DO UPDATE SET
                 kind = excluded.kind, currency = excluded.currency,
                 description = excluded.description""",
            (
                i["symbol"],
                i["kind"],
                i.get("currency", "USD"),
                i.get("description"),
            ),
        )
        # Only genuine overrides are stored; lookup falls back to the canonical
        # symbol, so there is no row for every (provider, instrument) pair.
        for provider_name, vendor_symbol in i.get("providers", {}).items():
            conn.execute(
                """INSERT INTO provider_symbol
                     (provider_id, instrument_id, symbol)
                   VALUES (
                     (SELECT provider_id FROM provider WHERE name = ?),
                     (SELECT instrument_id FROM instrument WHERE symbol = ?),
                     ?)
                   ON CONFLICT (provider_id, instrument_id) DO UPDATE SET
                     symbol = excluded.symbol""",
                (provider_name, i["symbol"], vendor_symbol),
            )
    conn.commit()


def provider_id(conn: sqlite3.Connection, name: str) -> int:
    row = conn.execute(
        "SELECT provider_id FROM provider WHERE name = ?", (name,)
    ).fetchone()
    if row is None:
        raise UnknownInstrument(
            f"unknown provider {name!r}. Add it to {REGISTRY_PATH.name} "
            f"and re-run init_db.py."
        )
    return row["provider_id"]


def instrument_id(conn: sqlite3.Connection, symbol: str) -> int:
    row = conn.execute(
        "SELECT instrument_id FROM instrument WHERE symbol = ?", (symbol,)
    ).fetchone()
    if row is None:
        raise UnknownInstrument(
            f"unknown instrument {symbol!r}.\n"
            f"Instruments are registered rather than created on the fly, so a "
            f"typo cannot silently become data. Add to {REGISTRY_PATH.name}:\n\n"
            f"    [[instrument]]\n"
            f'    symbol = "{symbol}"\n'
            f'    kind = "equity"\n'
            f'    description = "..."\n\n'
            f"then re-run init_db.py."
        )
    return row["instrument_id"]


def vendor_symbol(conn: sqlite3.Connection, pid: int, iid: int, canonical: str) -> str:
    """The provider's identifier for an instrument, defaulting to ours."""
    row = conn.execute(
        "SELECT symbol FROM provider_symbol WHERE provider_id = ? AND instrument_id = ?",
        (pid, iid),
    ).fetchone()
    return row["symbol"] if row else canonical


# --------------------------------------------------------------------------
# Writing
# --------------------------------------------------------------------------


def _check_unique(rows: Iterable, key, what: str) -> None:
    seen = set()
    for row in rows:
        k = key(row)
        if k in seen:
            raise DuplicateInBatch(f"provider returned two {what} rows for {k!r}")
        seen.add(k)


def write_payload(
    conn: sqlite3.Connection,
    pid: int,
    iid: int,
    payload: Payload,
    fetched_at: str,
) -> None:
    """Write a parsed payload and stamp the instrument's data vintage.

    All of it in one transaction: a half-written series is worse than none.
    """
    _check_unique(payload.bars, lambda b: (b.series_kind, b.d), "bar")
    _check_unique(payload.distributions, lambda x: (x.ex_date, x.kind), "distribution")
    _check_unique(payload.splits, lambda s: s.ex_date, "split")
    _check_unique(payload.rates, lambda r: r.d, "rate")

    with conn:  # commits on success, rolls back on exception
        conn.executemany(
            """INSERT INTO bar (provider_id, instrument_id, series_kind, d,
                                open, high, low, close, volume)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
               ON CONFLICT (provider_id, instrument_id, series_kind, d)
               DO UPDATE SET open = excluded.open, high = excluded.high,
                             low = excluded.low, close = excluded.close,
                             volume = excluded.volume""",
            [
                (pid, iid, b.series_kind, b.d, b.open, b.high, b.low, b.close, b.volume)
                for b in payload.bars
            ],
        )
        conn.executemany(
            """INSERT INTO distribution (provider_id, instrument_id, ex_date, kind,
                                         amount, record_date, pay_date)
               VALUES (?, ?, ?, ?, ?, ?, ?)
               ON CONFLICT (provider_id, instrument_id, ex_date, kind)
               DO UPDATE SET amount = excluded.amount,
                             record_date = excluded.record_date,
                             pay_date = excluded.pay_date""",
            [
                (pid, iid, x.ex_date, x.kind, x.amount, x.record_date, x.pay_date)
                for x in payload.distributions
            ],
        )
        conn.executemany(
            """INSERT INTO split (provider_id, instrument_id, ex_date, num, den)
               VALUES (?, ?, ?, ?, ?)
               ON CONFLICT (provider_id, instrument_id, ex_date)
               DO UPDATE SET num = excluded.num, den = excluded.den""",
            [(pid, iid, s.ex_date, s.num, s.den) for s in payload.splits],
        )
        conn.executemany(
            """INSERT INTO rate (provider_id, instrument_id, d, value, convention)
               VALUES (?, ?, ?, ?, ?)
               ON CONFLICT (provider_id, instrument_id, d)
               DO UPDATE SET value = excluded.value,
                             convention = excluded.convention""",
            [(pid, iid, r.d, r.value, r.convention) for r in payload.rates],
        )
        conn.execute(
            """INSERT INTO ingest (provider_id, instrument_id, fetched_at)
               VALUES (?, ?, ?)
               ON CONFLICT (provider_id, instrument_id)
               DO UPDATE SET fetched_at = excluded.fetched_at""",
            (pid, iid, fetched_at),
        )
