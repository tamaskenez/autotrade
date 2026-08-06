#!/usr/bin/env python3
"""Download daily history for exchange-listed instruments and load it into the
market data database.

"Equity" here means the instrument class, not the underlying: anything that
trades on an exchange with a daily candle and occasional dividends and splits.
Bond ETFs qualify -- IEF is an exchange-listed share whose contents happen to be
Treasuries. Open-end mutual funds do not: they price once a day at NAV, so they
have no candle and no volume, and belong in a separate script.

Symbols are parameters. Nothing here knows or cares which of them some strategy
intends to trade.

    uv run src/marketdata/fetch_equity_history.py --provider tiingo --symbol SPY

Payloads are cached under the raw directory and reused by default; --refetch
forces a download. The cache is a convenience -- it keeps parser iterations off
the vendor's rate limits -- not an archive, so only the newest payload per
symbol is kept.
"""

import argparse
import os
import sys
from datetime import UTC, datetime
from pathlib import Path

import config
import db
import providers
from model import Payload

def _iso_utc(dt: datetime) -> str:
    return dt.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


def _load_payload(
    provider, symbol: str, cache: Path, refetch: bool
) -> tuple[bytes, str, bool]:
    """Return (payload, fetched_at, from_cache).

    A cached payload's vintage is when it was downloaded, not now -- otherwise
    re-running against the cache would silently claim the data is fresh.
    """
    if cache.exists() and not refetch:
        mtime = datetime.fromtimestamp(cache.stat().st_mtime, tz=UTC)
        return cache.read_bytes(), _iso_utc(mtime), True

    now = datetime.now(tz=UTC)
    payload = provider.fetch(symbol)

    # Write via a temporary so an interrupted download cannot leave a truncated
    # file that a later run would happily treat as a valid cache hit.
    cache.parent.mkdir(parents=True, exist_ok=True)
    tmp = cache.with_suffix(cache.suffix + ".part")
    tmp.write_bytes(payload)
    tmp.replace(cache)

    # Stamp the file with the request time rather than leaving the write time,
    # so a later cache hit reports the same vintage this run does. Otherwise the
    # same payload gets a different fetched_at depending on how it was loaded --
    # off by however long the download took.
    os.utime(cache, (now.timestamp(), now.timestamp()))

    return payload, _iso_utc(now), False


def ingest_symbol(
    conn, provider, symbol: str, raw_dir: Path, refetch: bool, dry_run: bool
) -> Payload:
    pid = db.provider_id(conn, provider.NAME)
    iid = db.instrument_id(conn, symbol)
    remote_symbol = db.vendor_symbol(conn, pid, iid, symbol)

    cache = raw_dir / provider.NAME / f"{symbol}.{provider.EXT}"
    raw, fetched_at, from_cache = _load_payload(provider, remote_symbol, cache, refetch)

    payload = provider.parse(raw)
    source = "cached" if from_cache else "downloaded"
    print(f"{provider.NAME}:{symbol}  {source} {fetched_at}  {payload.counts()}")

    if dry_run:
        print(f"{provider.NAME}:{symbol}  dry run, nothing written")
    else:
        db.write_payload(conn, pid, iid, payload, fetched_at)

    return payload


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--db", type=Path, default=config.DB_PATH, help="database path")
    ap.add_argument(
        "--raw-dir", type=Path, default=config.RAW_DIR, help="raw payload cache"
    )
    ap.add_argument(
        "--provider", required=True, choices=providers.names(), help="data source"
    )
    ap.add_argument(
        "--symbol", required=True, nargs="+", help="one or more instrument symbols"
    )
    ap.add_argument(
        "--refetch", action="store_true", help="ignore the cache and download"
    )
    ap.add_argument(
        "--dry-run", action="store_true", help="fetch and parse, but do not write"
    )
    args = ap.parse_args()

    if not args.db.exists():
        sys.exit(f"{args.db} does not exist -- run init_db.py first")

    provider = providers.get(args.provider)
    conn = db.connect(args.db)
    try:
        for symbol in args.symbol:
            ingest_symbol(
                conn, provider, symbol, args.raw_dir, args.refetch, args.dry_run
            )
    # Configuration mistakes, not bugs -- a traceback would only bury the fix.
    except (db.UnknownInstrument, config.MissingCredential) as e:
        sys.exit(f"\n{e}")
    finally:
        conn.close()


if __name__ == "__main__":
    main()
