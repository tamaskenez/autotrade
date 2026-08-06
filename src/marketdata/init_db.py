#!/usr/bin/env python3
"""Create or update the market data database.

Applies schema.sql and instruments.toml. Idempotent -- re-run it after editing
either file.

    uv run src/marketdata/init_db.py
"""

import argparse
from pathlib import Path

import config
import db


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db", type=Path, default=config.DB_PATH, help="database path")
    ap.add_argument(
        "--registry",
        type=Path,
        default=db.REGISTRY_PATH,
        help="instrument registry TOML",
    )
    args = ap.parse_args()

    conn = db.connect(args.db)
    db.apply_schema(conn)
    db.seed_registry(conn, db.load_registry(args.registry))

    providers = conn.execute("SELECT COUNT(*) AS n FROM provider").fetchone()["n"]
    instruments = conn.execute("SELECT COUNT(*) AS n FROM instrument").fetchone()["n"]
    version = conn.execute("PRAGMA user_version").fetchone()[0]
    conn.close()

    print(
        f"{args.db}: schema v{version}, "
        f"{providers} providers, {instruments} instruments"
    )


if __name__ == "__main__":
    main()
