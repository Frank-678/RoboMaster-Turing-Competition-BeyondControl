#!/bin/bash
# 批量练习赛归档拷贝（含 session 自洽校验）。用法：sh /home/frank/copy_batch_artifacts_20261030.sh <session...>
# 归档检查协议（2026-10-29 勘误后执行）：
#   1) 文件内所有 session 与目录一致；2) tick 覆盖完整（bot 日志允许 pkill 截尾，须标注）；
#   3) 客户端/网关计数一致；4) 构建SHA与config哈希对应；5) bot 日志必须当局结束后立即拷贝，禁止从共享路径统一拷贝。
set -u
DEST=/mnt/d/code/The-Logical-RMT-C-log/batch-2026-10-30-seed-20261030
mkdir -p "$DEST"
for s in "$@"; do
  src="/home/frank/.rmtc-practice/$s"
  if [ ! -d "$src" ]; then echo "SKIP $s: no source dir"; continue; fi
  dst="$DEST/$s"
  mkdir -p "$dst"
  ok=1
  for f in score.json ai.jsonl config.json referee.log frames.jsonl bot_B.log bot_C.log bot_D.log communication.jsonl scoring.log; do
    if [ -f "$src/$f" ]; then cp "$src/$f" "$dst/$f"; else echo "MISSING $s/$f"; ok=0; fi
  done
  # 校验：score/ai/frames 里的 session 必须全部等于目录名。
  for f in score.json ai.jsonl frames.jsonl; do
    if [ -f "$dst/$f" ]; then
      bad=$(grep -o 'session[=: ]*practice-[a-f0-9]*' "$dst/$f" | grep -v "session[=: ]*$s" | sort -u | head -3)
      if [ -n "$bad" ]; then echo "SESSION-MISMATCH $s/$f: $bad"; ok=0; fi
    fi
  done
  # bot_A.log 若存在（须当局结束后立即从该局路径拷贝来的），同样校验 session 并标注 tick 覆盖。
  if [ -f "$src/bot_A.log" ]; then
    cp "$src/bot_A.log" "$dst/bot_A.log"
    bad=$(grep -o 'session=practice-[a-f0-9]*' "$dst/bot_A.log" | grep -v "session=$s" | sort -u | head -3)
    if [ -n "$bad" ]; then echo "SESSION-MISMATCH $s/bot_A.log: $bad"; ok=0; fi
    last=$(grep -o 'tick=[0-9]*' "$dst/bot_A.log" | sed 's/tick=//' | sort -n | tail -1)
    if [ "$last" != "499" ]; then echo "TRUNCATED $s/bot_A.log last_tick=$last (expected 499)"; fi
  fi
  [ "$ok" = 1 ] && echo "OK $s"
done
