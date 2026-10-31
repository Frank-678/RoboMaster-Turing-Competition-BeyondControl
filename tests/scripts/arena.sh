#!/usr/bin/env bash
set -eo pipefail
# 只运行离线自建对战场；不启动ROS、不读取或改写practice/，也不自动改比赛配置。
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
export PYTHONDONTWRITEBYTECODE=1
exec python3 tests/run_arena.py "$@"
