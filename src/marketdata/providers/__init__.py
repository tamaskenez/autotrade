"""Provider modules.

Each exposes the same two functions, so the ingest driver never branches on
which vendor it is talking to:

    NAME  : str                     provider name, matching instruments.toml
    EXT   : str                     file extension for the raw archive
    fetch(symbol: str) -> bytes     download full available history
    parse(payload: bytes) -> Payload

A provider fills in whichever parts of the Payload it can and leaves the rest
empty: Tiingo returns bars, distributions and splits; FRED will return only
rates; IBKR would return only vendor-adjusted bars.
"""

from types import ModuleType

from providers import tiingo

_MODULES: dict[str, ModuleType] = {
    tiingo.NAME: tiingo,
}


def get(name: str) -> ModuleType:
    try:
        return _MODULES[name]
    except KeyError:
        known = ", ".join(sorted(_MODULES))
        raise SystemExit(f"unknown provider {name!r}; have: {known}") from None


def names() -> list[str]:
    return sorted(_MODULES)
