#!/bin/bash -e
#
# Run a marketdata tool with everything it generates confined to the workspace.
#
#   ./md.sh init_db.py
#   ./md.sh fetch_equity_history.py --provider tiingo --symbol SPY EFA IEF BIL
#   ./md.sh test
#
# Left to themselves, uv puts the virtualenv next to the sources and CPython
# scatters __pycache__ through them. The two exports below redirect both into
# _md/, which is where the database and the raw payload cache already live.
# pytest's own cache is pointed there by pyproject.toml.
#
# The scripts anchor their default paths to the repository root rather than the
# working directory, so this can be run from anywhere.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project="$root/src/marketdata"

export UV_PROJECT_ENVIRONMENT="$root/_md/venv"
export PYTHONPYCACHEPREFIX="$root/_md/pycache"

usage() {
	echo "usage: $(basename "$0") <script.py> [args...]"
	echo "       $(basename "$0") test [pytest args...]"
	echo
	echo "available scripts:"
	(cd "$project" && ls -1 *.py | grep -v -E '^(config|db|model)\.py$' | sed 's/^/  /')
	exit 1
}

case "${1:-}" in
	test)
		shift
		# --directory so pytest's rootdir is the project and it picks up
		# the settings in pyproject.toml.
		exec uv run --directory "$project" python -m pytest "$@"
		;;
	"" | -h | --help)
		usage
		;;
	*)
		script="$1"
		shift
		[[ -f "$project/$script" ]] || usage
		exec uv run --project "$project" "$project/$script" "$@"
		;;
esac
