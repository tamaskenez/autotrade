"""Workspace layout and credential lookup.

Everything the tools generate -- database, raw payload cache, virtualenv,
bytecode and test caches -- lives under a single workspace directory so it never
mixes with source. Nothing generated belongs in the source tree.

Credentials are looked up in the environment first, then in a TOML file outside
the repository:

    ~/.config/autotrade/credentials.toml

        [tiingo]
        api_key = "..."

That file lives outside the working tree so secrets cannot be committed by
accident -- true by construction rather than by remembering to gitignore
something. The environment variable override keeps scripted use easy.
"""

import os
import tomllib
from pathlib import Path

# src/marketdata/config.py -> repository root. Anchoring to the file rather than
# the working directory means the tools behave the same from anywhere.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# The one definition of where generated state goes. md.sh derives the venv and
# bytecode cache locations from the same directory; pyproject.toml points
# pytest's cache here too.
WORKSPACE = REPO_ROOT / "_md"

DB_PATH = WORKSPACE / "marketdata.sqlite"
RAW_DIR = WORKSPACE / "raw"

CREDENTIALS_PATH = Path.home() / ".config" / "autotrade" / "credentials.toml"


class MissingCredential(Exception):
    pass


def api_key(provider: str) -> str:
    """Return the API key for `provider`, or raise MissingCredential."""
    env_var = f"{provider.upper()}_API_KEY"
    if key := os.environ.get(env_var):
        return key

    if CREDENTIALS_PATH.exists():
        with CREDENTIALS_PATH.open("rb") as f:
            creds = tomllib.load(f)
        if key := creds.get(provider, {}).get("api_key"):
            return key

    raise MissingCredential(
        f"no API key for {provider!r}.\n"
        f"Set ${env_var}, or add to {CREDENTIALS_PATH}:\n\n"
        f"    [{provider}]\n"
        f'    api_key = "..."\n'
    )
