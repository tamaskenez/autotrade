"""Tiingo end-of-day prices.

One request returns bars, dividends and splits together: `divCash` and
`splitFactor` ride along on each daily row rather than coming from separate
corporate-action endpoints. That is why the ingest driver is organised per
provider rather than per table.

Tiingo publishes an adjusted series (`adjOpen`/`adjClose`/...) which we store as
`vendor_adj` for cross-checking only. Total returns get computed downstream from
the raw bars plus distributions -- see schema.sql.

API: https://www.tiingo.com/documentation/end-of-day
"""

import json

import requests

import config
from model import (
    SERIES_RAW,
    SERIES_VENDOR_ADJ,
    Bar,
    Distribution,
    Payload,
    Split,
)

NAME = "tiingo"
EXT = "json"

_URL = "https://api.tiingo.com/tiingo/daily/{symbol}/prices"

# Tiingo returns only the most recent row unless a start date is given.
_EPOCH = "1900-01-01"

_TIMEOUT = 60


def fetch(symbol: str) -> bytes:
    """Download the full available history for `symbol`."""
    response = requests.get(
        _URL.format(symbol=symbol),
        params={"startDate": _EPOCH, "format": "json"},
        # Header rather than a `token=` query parameter: the key would otherwise
        # end up in the archived payload path, logs and any committed fixture.
        headers={"Authorization": f"Token {config.api_key(NAME)}"},
        timeout=_TIMEOUT,
    )
    if not response.ok:
        raise RuntimeError(
            f"tiingo {symbol}: HTTP {response.status_code} {response.reason}\n"
            f"{response.text[:500]}"
        )
    return response.content


def parse(payload: bytes) -> Payload:
    rows = json.loads(payload)
    if not isinstance(rows, list):
        raise ValueError(f"expected a JSON array, got {type(rows).__name__}")

    out = Payload()
    for row in rows:
        # '1993-01-29T00:00:00.000Z' -> '1993-01-29'
        d = row["date"][:10]

        out.bars.append(
            Bar(
                d=d,
                series_kind=SERIES_RAW,
                open=row["open"],
                high=row["high"],
                low=row["low"],
                close=row["close"],
                volume=row["volume"],
            )
        )
        out.bars.append(
            Bar(
                d=d,
                series_kind=SERIES_VENDOR_ADJ,
                open=row["adjOpen"],
                high=row["adjHigh"],
                low=row["adjLow"],
                close=row["adjClose"],
                volume=row["adjVolume"],
            )
        )

        # Tiingo reports a single cash amount with no breakdown into ordinary
        # income vs. capital gains, so everything lands in 'unknown'. The issuer
        # files are what carry the categories; reconciliation compares this lump
        # sum against their sum for the same ex-date.
        if (amount := row["divCash"]) != 0.0:
            out.distributions.append(
                Distribution(ex_date=d, kind="unknown", amount=amount)
            )

        # 2.0 for a 2-for-1. Stored as num/den to keep the ratio exact.
        if (factor := row["splitFactor"]) != 1.0:
            out.splits.append(Split(ex_date=d, num=factor, den=1.0))

    return out
