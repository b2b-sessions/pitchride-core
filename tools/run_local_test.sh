#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run a leader and a follower on this computer, then analyse how well they synced.
#
#   tools/run_local_test.sh            # "bad home internet" profile
#   tools/run_local_test.sh clean      # perfect network
#   tools/run_local_test.sh awful      # very bad network
#   DURATION=60 tools/run_local_test.sh
#
# The follower also pretends to be a different machine: its clock is shifted
# by -237.5 ms and runs 60 ppm fast, so the clock sync has real work to do.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/b2b_demo"
OUT="${OUT:-$ROOT/test-output}"
DURATION="${DURATION:-30}"
PORT="${PORT:-9000}"
PROFILE="${1:-bad}"

case "$PROFILE" in
  clean) SIM=() ;;
  bad)   SIM=(--sim-delay-ms 20 --sim-jitter-ms 6 --sim-loss 3) ;;
  awful) SIM=(--sim-delay-ms 45 --sim-jitter-ms 20 --sim-loss 10) ;;
  *) echo "unknown profile: $PROFILE (use clean, bad or awful)"; exit 2 ;;
esac

if [[ ! -x "$BIN" ]]; then
  echo "Build first: cmake -S . -B build && cmake --build build"
  exit 1
fi

mkdir -p "$OUT"
echo "Profile: $PROFILE   Duration: ${DURATION}s   Output: $OUT"
echo

"$BIN" leader --port "$PORT" --duration "$((DURATION + 1))" \
  --log "$OUT/leader.csv" --wav "$OUT/leader.wav" "${SIM[@]}" &
LEADER=$!
sleep 0.3
"$BIN" follower --connect "127.0.0.1:$PORT" --duration "$DURATION" \
  --fake-offset-ms -237.5 --fake-drift-ppm 60 \
  --log "$OUT/follower.csv" --wav "$OUT/follower.wav" "${SIM[@]}" --seed 99 &
FOLLOWER=$!

wait "$FOLLOWER"
wait "$LEADER"
echo
python3 "$ROOT/tools/analyse.py" "$OUT/leader.csv" "$OUT/follower.csv"
