#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${RMTC_FORMAL_CONFIG:-$ROOT/practice1/config.json}"
python3 "$ROOT/tools/formal_practice_check.py" --stage 1 --config "$CONFIG"
exec "$ROOT/practice/start_referee.sh" --stage 1 --tick-ms 100 --max-ticks 100 \
  --port "${RMTC_FORMAL_PORT:-8080}" --data-dir "${RMTC_FORMAL_DATA_DIR:-$HOME/.rmtc-practice1}" \
  --config "$CONFIG" "$@"
