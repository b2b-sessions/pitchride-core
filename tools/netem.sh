#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Add real (kernel-level) bad-network conditions to a network interface using
# Linux's built-in `tc netem`. Needs sudo.
#
#   sudo tools/netem.sh on lo 25ms 8ms 3%     # delay, jitter, loss on loopback
#   sudo tools/netem.sh off lo
#
# WARNING: this affects ALL traffic on that interface until you turn it off.
# On "lo" that includes every local service on your machine. For everyday
# testing the demo's built-in --sim-* options are safer; use this to double
# check against the real network stack.

set -euo pipefail

ACTION="${1:-}"
DEV="${2:-lo}"

case "$ACTION" in
  on)
    DELAY="${3:-25ms}"
    JITTER="${4:-8ms}"
    LOSS="${5:-3%}"
    tc qdisc replace dev "$DEV" root netem delay "$DELAY" "$JITTER" distribution pareto loss "$LOSS"
    echo "netem on $DEV: delay $DELAY, jitter $JITTER, loss $LOSS"
    echo "Turn off with: sudo $0 off $DEV"
    ;;
  off)
    tc qdisc del dev "$DEV" root 2>/dev/null || true
    echo "netem off on $DEV"
    ;;
  show)
    tc qdisc show dev "$DEV"
    ;;
  *)
    echo "usage: sudo $0 on|off|show [interface] [delay] [jitter] [loss]"
    exit 2
    ;;
esac
