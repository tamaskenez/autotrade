"""Tiingo parser tests.

The fixture matches Tiingo's documented response shape exactly, but the values
are constructed rather than copied from a real response: it lets the same short
window carry a dividend and a split, and it sidesteps the vendor's
internal-use-only licence. A smoke test against a real payload belongs in the
gitignored raw cache, not here.
"""

import json
from pathlib import Path

import pytest

from model import SERIES_RAW, SERIES_VENDOR_ADJ
from providers import tiingo

FIXTURE = Path(__file__).parent / "fixtures" / "tiingo_prices.json"


@pytest.fixture
def payload():
    return tiingo.parse(FIXTURE.read_bytes())


def test_two_series_per_row(payload):
    """Each daily row yields a raw bar and a vendor-adjusted bar."""
    rows = json.loads(FIXTURE.read_text())
    assert len(payload.bars) == 2 * len(rows)

    raw = [b for b in payload.bars if b.series_kind == SERIES_RAW]
    adj = [b for b in payload.bars if b.series_kind == SERIES_VENDOR_ADJ]
    assert len(raw) == len(adj) == len(rows)


def test_dates_are_truncated_to_days(payload):
    assert all(len(b.d) == 10 for b in payload.bars)
    assert payload.bars[0].d == "2024-03-14"


def test_raw_bar_fields(payload):
    bar = next(b for b in payload.bars if b.series_kind == SERIES_RAW and b.d == "2024-03-14")
    assert (bar.open, bar.high, bar.low, bar.close) == (512.0, 514.2, 511.0, 513.5)
    assert bar.volume == 70000000


def test_adjusted_bar_reads_the_adj_fields(payload):
    """The adjusted series must come from adjOpen/adjClose, not be re-derived."""
    bar = next(
        b for b in payload.bars if b.series_kind == SERIES_VENDOR_ADJ and b.d == "2024-03-14"
    )
    assert (bar.open, bar.high, bar.low, bar.close) == (255.2, 256.3, 254.71, 255.95)
    assert bar.volume == 140000000


def test_distribution_extracted_on_ex_date(payload):
    assert len(payload.distributions) == 1
    dist = payload.distributions[0]
    assert dist.ex_date == "2024-03-15"
    assert dist.amount == 1.5951
    # Tiingo reports a lump sum with no breakdown into income vs. capital gains.
    assert dist.kind == "unknown"
    assert dist.record_date is None and dist.pay_date is None


def test_zero_dividends_produce_no_rows(payload):
    """Most rows carry divCash 0.0; those must not become distributions."""
    assert {d.ex_date for d in payload.distributions} == {"2024-03-15"}


def test_split_extracted_as_exact_ratio(payload):
    assert len(payload.splits) == 1
    split = payload.splits[0]
    assert split.ex_date == "2024-03-18"
    # 2-for-1 kept as num/den rather than collapsed to a float ratio.
    assert (split.num, split.den) == (2.0, 1.0)


def test_unsplit_rows_produce_no_split(payload):
    assert {s.ex_date for s in payload.splits} == {"2024-03-18"}


def test_no_rates(payload):
    """Tiingo has nothing to say about interest rates."""
    assert payload.rates == []


def test_rejects_non_array_payload():
    """Tiingo returns a JSON object for errors; that must not parse as data."""
    with pytest.raises(ValueError, match="expected a JSON array"):
        tiingo.parse(b'{"detail": "Not found."}')


def test_empty_history_is_not_an_error():
    assert tiingo.parse(b"[]").bars == []
