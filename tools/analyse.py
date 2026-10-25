#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare a leader log and a follower log from b2b_demo and report how well they synced.

Usage:
    python3 tools/analyse.py leader.csv follower.csv
    python3 tools/analyse.py leader.csv follower.csv --different-machines

Two kinds of accuracy are reported:

1. Musical accuracy (always valid)
   Did every DJ move land on the exact intended sample of the music on the
   follower? This is what listeners actually hear.

2. Wall-clock alignment (only when both peers ran on the same computer)
   Did the follower play each click exactly `delay` after the leader, in real
   time? This measures the clock sync itself, using the computer's own clock
   as ground truth. On two different machines there is no shared ground truth,
   so this part is skipped.
"""

import argparse
import csv
import statistics
import sys


def load(path):
    header = {}
    rows = []
    with open(path) as f:
        lines = []
        for line in f:
            if line.startswith("# "):
                key, _, value = line[2:].strip().partition("=")
                header[key] = value
            else:
                lines.append(line)
    for r in csv.DictReader(lines):
        rows.append(
            {
                "kind": r["kind"],
                "seq": int(r["seq"]),
                "beat": float(r["beat"]),
                "channel": int(r["channel"]),
                "value": float(r["value"]),
                "true_ns": int(r["true_ns"]),
                "extra": float(r["extra"]),
            }
        )
    return header, rows


def summarise(values):
    if not values:
        return None
    a = sorted(abs(v) for v in values)
    return {
        "n": len(values),
        "mean": statistics.fmean(values),
        "p50": a[len(a) // 2],
        "p95": a[min(len(a) - 1, int(len(a) * 0.95))],
        "max": a[-1],
    }


def fmt(s, unit="ms"):
    if s is None:
        return "no data"
    return (
        f"n={s['n']:<5} mean {s['mean']:+.3f} {unit} | |error| median {s['p50']:.3f}, "
        f"95% {s['p95']:.3f}, worst {s['max']:.3f} {unit}"
    )


def local_clock(header, t):
    """Convert ground-truth time to a peer's (fake) local clock, as LocalClock::fromTrue does."""
    off = int(header.get("fake_offset_ns", 0))
    drift = float(header.get("fake_drift_ppm", 0)) * 1e-6
    origin = int(header.get("origin_ns", 0))
    return t + off + (t - origin) * drift


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("leader")
    ap.add_argument("follower")
    ap.add_argument("--different-machines", action="store_true",
                    help="logs come from two computers: skip wall-clock comparisons")
    ap.add_argument("--threshold-ms", type=float, default=1.0,
                    help="wall-clock pass threshold for 95%% of clicks (default 1.0)")
    args = ap.parse_args()

    lh, lrows = load(args.leader)
    fh, frows = load(args.follower)
    delay_ns = int(fh.get("follower_delay_ns", lh.get("follower_delay_ns", 0)))
    sample_rate = int(fh.get("sample_rate", 48000))
    one_sample_ms = 1000.0 / sample_rate

    print("=" * 72)
    print("B2B sync report")
    print("=" * 72)
    print(f"Tempo {float(lh.get('bpm', 0)):.1f} BPM, follower delay {delay_ns / 1e6:.0f} ms")
    for label, h in (("leader", lh), ("follower", fh)):
        print(
            f"  {label:<9} fake network: {float(h.get('sim_delay_ms', 0)):.0f} ms delay, "
            f"{float(h.get('sim_jitter_ms', 0)):.0f} ms jitter, {float(h.get('sim_loss_pct', 0)):.1f}% loss; "
            f"fake clock offset {int(h.get('fake_offset_ns', 0)) / 1e6:+.1f} ms, "
            f"drift {float(h.get('fake_drift_ppm', 0)):+.0f} ppm"
        )

    # ---- 1. Musical accuracy -------------------------------------------------
    l_events = {r["seq"]: r for r in lrows if r["kind"] == "E"}
    f_events = {r["seq"]: r for r in frows if r["kind"] == "E"}
    f_clicks = [r for r in frows if r["kind"] == "C"]
    f_last = max((r["true_ns"] for r in f_clicks), default=0)
    # Only expect events the follower had time to reach before it stopped.
    expected = {s: r for s, r in l_events.items() if r["true_ns"] + delay_ns < f_last - 50_000_000}
    missing = [s for s in expected if s not in f_events]
    lateness = [f_events[s]["extra"] for s in expected if s in f_events]
    on_time = sum(1 for x in lateness if x <= one_sample_ms)
    late = [x for x in lateness if x > one_sample_ms]

    print()
    print("1. Musical accuracy (what listeners hear)")
    print(f"   DJ moves made by leader:                 {len(expected)}")
    print(f"   Applied on the exact sample on follower: {on_time}")
    print(f"   Applied late:                            {len(late)}"
          + (f" (worst {max(late):.1f} ms)" if late else ""))
    print(f"   Never arrived:                           {len(missing)}")
    musical_ok = len(expected) > 0 and on_time == len(expected)

    resyncs = [r for r in frows if r["kind"] == "R"]
    if resyncs:
        print(f"   Follower playback jumps (resyncs):       {len(resyncs)}")

    # ---- 2. Wall-clock alignment --------------------------------------------
    wall_ok = None
    if not args.different_machines:
        l_clicks = {(r["channel"], r["seq"]): r["true_ns"] for r in lrows if r["kind"] == "C"}
        click_err = [
            (r["true_ns"] - l_clicks[(r["channel"], r["seq"])] - delay_ns) / 1e6
            for r in f_clicks
            if (r["channel"], r["seq"]) in l_clicks
        ]
        event_err = [
            (f_events[s]["true_ns"] - l_events[s]["true_ns"] - delay_ns) / 1e6
            for s in expected
            if s in f_events
        ]
        clock_err = []
        for r in frows:
            if r["kind"] == "S":
                t = r["true_ns"]
                true_offset = local_clock(lh, t) - local_clock(fh, t)
                clock_err.append((r["extra"] - true_offset) / 1e6)

        print()
        print("2. Wall-clock alignment (same computer, real clock as ground truth)")
        print(f"   Clock offset estimate error: {fmt(summarise(clock_err))}")
        print(f"   Click timing error:          {fmt(summarise(click_err))}")
        print(f"   DJ move timing error:        {fmt(summarise(event_err))}")
        s = summarise(click_err)
        wall_ok = s is not None and s["p95"] < args.threshold_ms
        print(f"   (error = how far from exactly {delay_ns / 1e6:.0f} ms behind the leader)")

    # ---- Verdict ---------------------------------------------------------------
    print()
    print("-" * 72)
    print(f"Musical accuracy:      {'PASS' if musical_ok else 'FAIL'}")
    if wall_ok is not None:
        print(f"Wall-clock alignment:  {'PASS' if wall_ok else 'FAIL'} "
              f"(95% of clicks within {args.threshold_ms} ms)")
    print("-" * 72)
    return 0 if musical_ok and wall_ok is not False else 1


if __name__ == "__main__":
    sys.exit(main())
