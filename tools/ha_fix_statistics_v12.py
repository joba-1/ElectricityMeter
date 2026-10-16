#!/usr/bin/env python3
"""Repair HA long-term statistics of the grid counters damaged by v11.0-v11.2 bit errors.

Between 2026-05-24 and 2026-06-18 corrupted SML frames reached HA as states like
9.2e17 Wh. Each one added a huge jump to the statistics `sum`; at ~1e18 a float64
resolves only ~1 kWh, so every later hourly difference collapsed to 0 or ~1 kWh and
the energy dashboard has shown no grid import/export since.

The hourly `state` column is intact apart from the spike hours, so the sums are
rebuilt from it:

  1. find the first hour whose sum jumps more than MAX_WH_PER_H
  2. rebuild sums from there: add state - last good state, skipping states that
     are absurd, go backwards or rise faster than MAX_WH_PER_H (the spikes)
  3. recorder/adjust_sum_statistics removes the offset from the hourly and the
     5-minute table (the 5-minute sums are what the next compile continues from),
     unless the 5-minute table is already free of it
  4. recorder/import_statistics replaces the hourly rows with the rebuilt ones,
     shifted by the small (< ~2 kWh) rounding left in the 5-minute base, so the
     next compiled hour continues seamlessly; the shift lands on the first
     damaged hour in May

  ha_fix_statistics_v12.py [--apply] sensor.power3_a_plus sensor.power3_a_minus

Without --apply it only prints what it would do. Back up home-assistant_v2.db first.
The recorder applies these calls from its queue, so every step polls until its
effect is visible before the next one reads data (a fixed sleep raced on
2026-10-16). Every step is idempotent; --from forces the first damaged hour
when an earlier partial run already removed the visible jump.
Token: /run/user/1000/ha-token (see ha_migrate_v12.py).
"""

import argparse
import sys
import time
from datetime import datetime, timedelta, timezone

from ha_migrate_v12 import HA

MAX_WH_PER_H = 25000     # > USAGE_KW_MAX (20 kW) for one hour
ABSURD_WH = 1e9          # 1 GWh: no house meter gets there


def hourly(ha, sid, days=800):
    end = datetime.now(timezone.utc)
    res = ha.call("recorder/statistics_during_period", statistic_ids=[sid], period="hour",
                  start_time=(end - timedelta(days=days)).isoformat(), end_time=end.isoformat(),
                  types=["state", "sum"])
    return res.get(sid, [])


def latest_5min(ha, sid):
    end = datetime.now(timezone.utc)
    res = ha.call("recorder/statistics_during_period", statistic_ids=[sid], period="5minute",
                  start_time=(end - timedelta(hours=2)).isoformat(), end_time=end.isoformat(),
                  types=["state", "sum"])
    return res[sid][-1]


def rebuild(rows):
    """Return (index of first damaged hour, [(start_ms, state, sum)] rebuilt from there)."""
    j0 = next((i for i in range(1, len(rows)) if rows[i]["sum"] - rows[i - 1]["sum"] > MAX_WH_PER_H), None)
    if j0 is None:
        return None, []
    return j0, rebuild_from(rows, j0)


def rebuild_from(rows, j0):
    total = rows[j0 - 1]["sum"]
    good = rows[j0 - 1]["state"]
    good_start = rows[j0 - 1]["start"]
    out, skipped = [], 0
    for r in rows[j0:]:
        s = r["state"]
        # allowance since the counter last rose: when HA's state froze for a day
        # (no updates from the gateway) the catch-up in the evening is genuine
        hours = max(1, (r["start"] - good_start) / 3600000)
        if s is not None and s < ABSURD_WH and 0 <= s - good <= MAX_WH_PER_H * hours:
            total += s - good
            if s > good:
                good_start = r["start"]
            good = s
        else:
            skipped += 1
        out.append((r["start"], good, total))
    print(f"  rebuilt {len(out)} hours from {iso(rows[j0]['start'])}, skipped {skipped} spike hours")
    return out


def iso(ms):
    return datetime.fromtimestamp(ms / 1000, timezone.utc).isoformat()


def wait_for(what, check, timeout=180):
    """The recorder applies WebSocket changes from its queue: poll until visible."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = check()
        if r:
            return r
        time.sleep(3)
    sys.exit(f"timeout waiting for {what} - recorder queue busy? Re-run; every step is idempotent.")


def bad_steps(rows):
    return [(iso(b["start"]), b["sum"] - a["sum"]) for a, b in zip(rows, rows[1:])
            if not 0 <= b["sum"] - a["sum"] <= MAX_WH_PER_H]


def fix(ha, sid, apply, start=None, snapshot=None):
    rows = hourly(ha, sid)
    if snapshot:
        # original hourly states from a stats dump taken before any repair (an earlier
        # import replaced the spike states with carried-forward values)
        orig = {r["start"]: r["state"] for r in snapshot}
        n = sum(1 for r in rows if r["start"] in orig)
        rows = [dict(r, state=orig.get(r["start"], r["state"])) for r in rows]
        print(f"  states of {n} hours taken from the snapshot")
    if start:
        t = datetime.fromisoformat(start).timestamp() * 1000
        j0 = next(i for i, r in enumerate(rows) if r["start"] >= t)
        print(f"  start forced to {iso(rows[j0]['start'])}")
    else:
        j0, _ = rebuild(rows)
        if j0 is None:
            print(f"{sid}: no damage found")
            return
    new = rebuild_from(rows, j0)
    print(f"{sid}: sum now {rows[-1]['sum']:.6g}, rebuilt {new[-1][2]:.1f}")
    if not apply:
        return

    def expected_now(s5):   # rebuilt value at the time of the latest 5-minute row
        return new[-1][2] + (s5["state"] - new[-1][1])

    # 3. the 5-minute table still carries the offset? remove it (hourly + 5-minute)
    s5 = latest_5min(ha, sid)
    if abs(s5["sum"] - expected_now(s5)) > 1e6:
        ha.call("recorder/adjust_sum_statistics", statistic_id=sid, start_time=iso(rows[j0]["start"]),
                adjustment=expected_now(s5) - s5["sum"], adjustment_unit_of_measurement="Wh")
        s5 = wait_for("adjust", lambda: (lambda x: x if abs(x["sum"] - expected_now(x)) < 1e6 else None)(
            latest_5min(ha, sid)))

    # 4. hourly rows := rebuilt, shifted onto the 5-minute base the next compile continues from
    shift = s5["sum"] - expected_now(s5)
    if abs(shift) > 5000:
        sys.exit(f"  shift {shift:.1f} Wh is not a rounding remainder - stopping, nothing imported")
    print(f"  5-minute base off by {shift:.1f} Wh; shifting the rebuilt hours by it")
    stats = [{"start": iso(t), "state": st, "sum": sm + shift} for t, st, sm in new]
    ha.call("recorder/import_statistics",
            metadata={"has_mean": False, "mean_type": 0, "has_sum": True, "name": None, "source": "recorder",
                      "statistic_id": sid, "unit_of_measurement": "Wh", "unit_class": "energy"},
            stats=stats)
    want = {datetime.fromisoformat(x["start"]).timestamp() * 1000: x["sum"] for x in stats}

    def landed():
        r = hourly(ha, sid)
        return r if all(abs(want[x["start"]] - x["sum"]) < 0.5 for x in r if x["start"] in want) else None
    check = wait_for("import", landed, timeout=900)
    neg = [b for b in bad_steps(check) if b[1] < 0]
    print(f"  after: last hour sum {check[-1]['sum']:.1f}, max state {max(r['state'] for r in check):.1f}, "
          f"decreasing steps {neg[:3]} (steps > {MAX_WH_PER_H} Wh are catch-ups after frozen states)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--from", dest="start", help="first damaged hour (ISO, UTC) if the sums no longer show it")
    ap.add_argument("--snapshot", nargs=2, metavar=("JSON", "STATISTIC_ID"),
                    help="ha_migrate_v12.py stats dump with the original states, and the id in it")
    ap.add_argument("entities", nargs="+")
    args = ap.parse_args()
    minute = datetime.now().minute
    if args.apply and (minute >= 58 or minute < 3):
        sys.exit("too close to the hourly compile - run again a few minutes after the full hour")
    ha = HA()
    for sid in args.entities:
        snap = None
        if args.snapshot:
            import json
            snap = json.load(open(args.snapshot[0]))[args.snapshot[1]]
        fix(ha, sid, args.apply, args.start, snap)


if __name__ == "__main__":
    main()
