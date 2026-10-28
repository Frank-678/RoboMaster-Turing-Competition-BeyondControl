#!/usr/bin/env bash
set -euo pipefail
rmtc_test_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
rmtc_test_tmp="$(mktemp -d)"
trap 'rm -rf -- "$rmtc_test_tmp"' EXIT
rmtc_test_flags=(-std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread
  -I "$rmtc_test_root/src/rmtc_bot/include")
if [[ -n "${RMTC_JSON_INCLUDE:-}" ]]; then
  rmtc_test_flags+=(-I "$RMTC_JSON_INCLUDE")
fi
if [[ "${RMTC_SANITIZE:-0}" == 1 ]]; then
  rmtc_test_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
fi
g++ "${rmtc_test_flags[@]}" \
  "$rmtc_test_root"/src/rmtc_bot/src/{strategy,tactics,events,ai_worker,session}.cpp \
  "$rmtc_test_root"/tests/{strategy_test,full_strategy_test}.cpp \
  -lcurl -o "$rmtc_test_tmp/strategy_test"
python3 "$rmtc_test_root/tests/mock_gateway.py" "$rmtc_test_tmp/strategy_test"
