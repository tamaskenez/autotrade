"""Schema and write-path tests, against a real SQLite file in a temp directory."""

import pytest

import db
from model import Bar, Distribution, Payload, Rate, Split

FETCHED_AT = "2026-08-06T09:00:00Z"


@pytest.fixture
def conn(tmp_path):
    c = db.connect(tmp_path / "test.sqlite")
    db.apply_schema(c)
    db.seed_registry(
        c,
        {
            "provider": [{"name": "tiingo", "kind": "vendor"}],
            "instrument": [
                {"symbol": "SPY", "kind": "equity", "description": "test"},
                {
                    "symbol": "DTB3",
                    "kind": "rate",
                    "providers": {"tiingo": "SERIES-DTB3"},
                },
            ],
        },
    )
    yield c
    c.close()


@pytest.fixture
def ids(conn):
    return db.provider_id(conn, "tiingo"), db.instrument_id(conn, "SPY")


def test_seed_registry_is_idempotent(conn):
    registry = {
        "provider": [{"name": "tiingo", "kind": "vendor"}],
        "instrument": [{"symbol": "SPY", "kind": "equity"}],
    }
    db.seed_registry(conn, registry)
    db.seed_registry(conn, registry)
    assert conn.execute("SELECT COUNT(*) FROM instrument").fetchone()[0] == 2


def test_unknown_instrument_is_rejected(conn):
    """Typos must not silently become new instruments."""
    with pytest.raises(db.UnknownInstrument, match="SPYY"):
        db.instrument_id(conn, "SPYY")


def test_vendor_symbol_defaults_to_canonical(conn):
    pid = db.provider_id(conn, "tiingo")
    iid = db.instrument_id(conn, "SPY")
    assert db.vendor_symbol(conn, pid, iid, "SPY") == "SPY"


def test_vendor_symbol_override_is_used(conn):
    pid = db.provider_id(conn, "tiingo")
    iid = db.instrument_id(conn, "DTB3")
    assert db.vendor_symbol(conn, pid, iid, "DTB3") == "SERIES-DTB3"


def test_write_payload_round_trip(conn, ids):
    pid, iid = ids
    payload = Payload(
        bars=[
            Bar(d="2024-03-14", series_kind="raw", open=512.0, high=514.2, low=511.0, close=513.5, volume=7e7),
            Bar(d="2024-03-14", series_kind="vendor_adj", close=255.95),
        ],
        distributions=[Distribution(ex_date="2024-03-15", kind="unknown", amount=1.5951)],
        splits=[Split(ex_date="2024-03-18", num=2.0, den=1.0)],
    )
    db.write_payload(conn, pid, iid, payload, FETCHED_AT)

    assert conn.execute("SELECT COUNT(*) FROM bar").fetchone()[0] == 2
    assert conn.execute("SELECT COUNT(*) FROM distribution").fetchone()[0] == 1
    assert conn.execute("SELECT COUNT(*) FROM split").fetchone()[0] == 1

    row = conn.execute(
        "SELECT * FROM bar WHERE series_kind = 'vendor_adj'"
    ).fetchone()
    assert row["close"] == 255.95
    # Sources that give only a close must not fabricate the rest.
    assert row["open"] is None and row["volume"] is None


def test_ingest_records_data_vintage(conn, ids):
    pid, iid = ids
    db.write_payload(conn, pid, iid, Payload(), FETCHED_AT)
    assert conn.execute("SELECT fetched_at FROM ingest").fetchone()[0] == FETCHED_AT


def test_reingest_overwrites_rather_than_duplicates(conn, ids):
    """Full history is refetched every time; a rerun must not double the rows."""
    pid, iid = ids
    first = Payload(bars=[Bar(d="2024-03-14", series_kind="raw", close=513.5)])
    second = Payload(bars=[Bar(d="2024-03-14", series_kind="raw", close=999.0)])

    db.write_payload(conn, pid, iid, first, FETCHED_AT)
    db.write_payload(conn, pid, iid, second, "2026-08-07T09:00:00Z")

    rows = conn.execute("SELECT close FROM bar").fetchall()
    assert [r["close"] for r in rows] == [999.0]
    assert conn.execute("SELECT fetched_at FROM ingest").fetchone()[0] == "2026-08-07T09:00:00Z"


def test_ordinary_and_capital_gain_on_same_ex_date_coexist(conn, ids):
    """iShares funds do this most Decembers; neither row may overwrite the other."""
    pid, iid = ids
    db.write_payload(
        conn,
        pid,
        iid,
        Payload(
            distributions=[
                Distribution(ex_date="2024-12-20", kind="ordinary", amount=0.42),
                Distribution(ex_date="2024-12-20", kind="lt_cap_gain", amount=0.11),
            ]
        ),
        FETCHED_AT,
    )
    total = conn.execute("SELECT SUM(amount) FROM distribution").fetchone()[0]
    assert total == pytest.approx(0.53)


def test_duplicate_distribution_in_one_payload_is_rejected(conn, ids):
    """A provider contradicting itself inside one response is always a bug."""
    pid, iid = ids
    payload = Payload(
        distributions=[
            Distribution(ex_date="2024-03-15", kind="unknown", amount=1.5951),
            Distribution(ex_date="2024-03-15", kind="unknown", amount=1.60),
        ]
    )
    with pytest.raises(db.DuplicateInBatch):
        db.write_payload(conn, pid, iid, payload, FETCHED_AT)


def test_duplicate_bar_in_one_payload_is_rejected(conn, ids):
    pid, iid = ids
    payload = Payload(
        bars=[
            Bar(d="2024-03-14", series_kind="raw", close=513.5),
            Bar(d="2024-03-14", series_kind="raw", close=514.0),
        ]
    )
    with pytest.raises(db.DuplicateInBatch):
        db.write_payload(conn, pid, iid, payload, FETCHED_AT)


def test_failed_write_leaves_nothing_behind(conn, ids):
    """A half-written series is worse than none, so the write is one transaction."""
    pid, iid = ids
    good = Bar(d="2024-03-14", series_kind="raw", close=513.5)
    payload = Payload(
        bars=[good],
        # NULL amount violates NOT NULL, failing after the bars are inserted.
        distributions=[Distribution(ex_date="2024-03-15", kind="unknown", amount=None)],
    )
    with pytest.raises(Exception):
        db.write_payload(conn, pid, iid, payload, FETCHED_AT)
    assert conn.execute("SELECT COUNT(*) FROM bar").fetchone()[0] == 0


def test_rate_requires_a_convention(conn):
    """The quoting basis must never be implicit -- see schema.sql."""
    pid = db.provider_id(conn, "tiingo")
    iid = db.instrument_id(conn, "DTB3")
    db.write_payload(
        conn,
        pid,
        iid,
        Payload(rates=[Rate(d="2024-03-14", value=5.24, convention="discount_360")]),
        FETCHED_AT,
    )
    assert conn.execute("SELECT convention FROM rate").fetchone()[0] == "discount_360"
