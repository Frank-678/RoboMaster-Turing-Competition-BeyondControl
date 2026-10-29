#!/usr/bin/env bash
# 席位变体构建（docs/38 联赛）：临时把 team/ai_token 改成目标席位 → colcon 独立底座构建 → 还原。
# 用法：bash tools/build_seat_variants.sh <source_dir> <version_name> <seats> [--manifest tools/versions.json]
# 例：bash tools/build_seat_variants.sh /home/frank/cst champion B
#     bash tools/build_seat_variants.sh /home/frank/cst-league-old old_champion A,B
# 不触碰 <source_dir>/build 与 install（冠军底座逐位不动）；每席位记录二进制 sha256 与配置 sha256。
set -eo pipefail

SOURCE_DIR="${1:?source_dir required}"
VERSION="${2:?version_name required}"
SEATS="${3:?seats (A/B/C/D, comma-separated) required}"
MANIFEST=""
if [[ "$4" == "--manifest" ]]; then MANIFEST="${5:?manifest path}"; fi

HDR="$SOURCE_DIR/src/rmtc_bot/include/rmtc_bot/match_config.hpp"
[[ -f "$HDR" ]] || { echo "找不到 $HDR" >&2; exit 1; }
BAK="$(mktemp /tmp/match_config.XXXXXX)"
cp "$HDR" "$BAK"
ORIG_SHA=$(sha256sum "$HDR" | cut -d' ' -f1)

source /opt/ros/humble/setup.bash

build_one() {
  local seat="$1"
  echo "=== [$VERSION-$seat] patch team/token -> $seat ==="
  sed -i "s/team = \"A\"/team = \"$seat\"/; s/practice-A/practice-$seat/" "$HDR"
  grep -q "team = \"$seat\"" "$HDR" || { echo "patch 失败：team = \"$seat\" 未出现" >&2; exit 1; }
  colcon build --packages-select rmtc_bot \
    --build-base "$SOURCE_DIR/build-v-$VERSION-$seat" \
    --install-base "$SOURCE_DIR/install-v-$VERSION-$seat"
  # 立即还原，绝不把变体配置留在源树里。
  cp "$BAK" "$HDR"
  local after
  after=$(sha256sum "$HDR" | cut -d' ' -f1)
  [[ "$after" == "$ORIG_SHA" ]] || { echo "还原校验失败：$after != $ORIG_SHA" >&2; exit 1; }
  local bin="$SOURCE_DIR/install-v-$VERSION-$seat/rmtc_bot/lib/rmtc_bot/bot"
  local bin_sha cfg_sha
  bin_sha=$(sha256sum "$bin" | cut -d' ' -f1)
  cfg_sha=$(sha256sum "$BAK" | cut -d' ' -f1)
  echo "seat=$seat binary_sha256=$bin_sha params_sha256=$cfg_sha"
  if [[ -n "$MANIFEST" ]]; then
    python3 - "$MANIFEST" "$VERSION" "$seat" "$bin_sha" "$cfg_sha" "$SOURCE_DIR" <<'EOF'
import json, sys
path, version, seat, bin_sha, cfg_sha, source_dir = sys.argv[1:7]
data = json.load(open(path)) if __import__("os").path.exists(path) else {}
data.setdefault("seats", {})
data["seats"].setdefault(version, {})[seat] = {
    "install_base": f"{source_dir}/install-v-{version}-{seat}",
    "binary_sha256": bin_sha, "params_sha256": cfg_sha}
json.dump(data, open(path, "w"), ensure_ascii=False, indent=2)
EOF
  fi
}

for s in $(echo "$SEATS" | tr ',' ' '); do
  case "$s" in A|B|C|D) build_one "$s" ;; *) echo "非法席位 $s" >&2; exit 2 ;; esac
done
rm -f "$BAK"
echo "=== [$VERSION] seats done: $SEATS ==="
