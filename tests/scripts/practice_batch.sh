#!/usr/bin/env bash
# 原版裁判批量实测；场景参数传给Python，网络配置从唯一头文件的已编译启动器取得。
set -eo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
if [[ ! -x install/rmtc_bot/lib/rmtc_bot/practice_launcher || ! -f build/rmtc_bot/match_config.sha256 ]]; then
  echo '请先 bash build.sh；网络设置只填 match_config.hpp。' >&2
  exit 2
fi
if ! sha256sum --check --status build/rmtc_bot/match_config.sha256; then
  echo '配置已改变，请先 bash build.sh。' >&2
  exit 2
fi
exec "$PWD/install/rmtc_bot/lib/rmtc_bot/practice_launcher" --batch "$@"
