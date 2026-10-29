#!/usr/bin/env bash
# 冻结历史源码、生成地图清单、按同一批局面多版本对打；不启动真实模型。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
python3 tests/version_league.py "$@"
