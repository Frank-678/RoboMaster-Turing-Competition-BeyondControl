#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${RMTC_FORMAL_CONFIG:-$ROOT/practice2/config.json}"
python3 "$ROOT/tools/formal_practice_check.py" --stage 2 --config "$CONFIG"
exec "$ROOT/practice/start_referee.sh" --stage 2 --tick-ms 500 --max-ticks 500 \
  --port "${RMTC_FORMAL_PORT:-8080}" --data-dir "${RMTC_FORMAL_DATA_DIR:-$HOME/.rmtc-practice2}" \
  --ai-mode mock --bots B,C,D --config "$CONFIG" "$@"
