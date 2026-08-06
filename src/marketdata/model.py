"""Provider-neutral representation of a parsed payload.

A provider module turns bytes into one of these; the driver writes it to the
database. Nothing here is provider- or strategy-specific, which is what keeps
`fetch_*.py` free of per-vendor branching.
"""

from dataclasses import dataclass, field

SERIES_RAW = "raw"
SERIES_VENDOR_ADJ = "vendor_adj"

# See schema.sql. 'qualified' is deliberately not a value: it is a portion of
# the ordinary dividend, not a separate payment.
DIST_KINDS = frozenset(
    {"ordinary", "st_cap_gain", "lt_cap_gain", "return_of_capital", "unknown"}
)


@dataclass(frozen=True, slots=True)
class Bar:
    d: str
    series_kind: str
    close: float
    open: float | None = None
    high: float | None = None
    low: float | None = None
    volume: float | None = None


@dataclass(frozen=True, slots=True)
class Distribution:
    ex_date: str
    kind: str
    amount: float
    record_date: str | None = None
    pay_date: str | None = None


@dataclass(frozen=True, slots=True)
class Split:
    ex_date: str
    num: float
    den: float


@dataclass(frozen=True, slots=True)
class Rate:
    d: str
    value: float
    convention: str


@dataclass(slots=True)
class Payload:
    """What a provider extracted for one instrument.

    A provider fills in whatever it can and leaves the rest empty: Tiingo
    returns bars, distributions and splits from a single response; FRED returns
    only rates; IBKR would return only `vendor_adj` bars.
    """

    bars: list[Bar] = field(default_factory=list)
    distributions: list[Distribution] = field(default_factory=list)
    splits: list[Split] = field(default_factory=list)
    rates: list[Rate] = field(default_factory=list)

    def counts(self) -> str:
        return (
            f"{len(self.bars)} bars, {len(self.distributions)} distributions, "
            f"{len(self.splits)} splits, {len(self.rates)} rates"
        )
