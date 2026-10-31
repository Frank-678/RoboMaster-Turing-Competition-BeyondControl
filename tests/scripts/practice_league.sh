#!/usr/bin/env bash
# 原版practice/ROS/真实模型版本联赛；离线快速联赛仍是league.sh。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
exec python3 tests/practice_version_league.py "$@"
