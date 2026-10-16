# ElectricityMeter — test plan

## Unit tests (every commit)

```bash
~/.platformio/penv/bin/pio test -e native      # PowerEstimator, simulated meter
~/.platformio/penv/bin/pio run -e d1_mini_ota  # firmware builds
```

`test/test_power` feeds a meter model with exact `floor(energy)` quantisation and covers:

* constant loads from 40 W to 5.4 kW (≤ 4–5 % worst case)
* no false step detections at constant load
* a step up followed within 4 s, a step down without undershoot
* decay to 0 after switch-off
* backwards frames and the start after reset

Unity's double asserts need `-DUNITY_INCLUDE_DOUBLE`. Without it they compile to
nothing and the accuracy tests pass vacuously.

## System test: v11 → v12 migration (major release)

The rule is that old and new must be the same. Every comparison uses a fixed range that
ends at the cutover (2026-10-16T12:47:39Z), so the old measurement and its copy hold the
same data.

| Check | Command | Result 2026-10-16 |
|-------|---------|-------------------|
| Influx history copied point by point | `tools/influx_migrate_v12.py meter-verify` | 2,339,829 + 3,575 points, 0 mismatches |
| Script queries old = new (35 days) | `tools/compare_queries_v12.py 2026-10-16T12:47:39Z 35` | 8/8 identical |
| Grafana targets old = new (7 days and 2021 for `meter=main`) | `tools/grafana_migrate_v12.py compare <dir> <from> <to>` | 19/19 identical |
| Panels continuous across the cutover | new panel query, last 3 h | no gap except the ~1.5 min reboot |
| Firmware power vs counter derivative | `mean(p_out)` vs `derivative(a_minus)` per minute | agree (one-bucket lag of the derivative panel) |
| HA entities adopted with history | registry: `sensor.energie_bezug` has unique_id `power3_a_plus` and the power3 device | ok |
| HA long-term statistics unchanged by the rename | `tools/ha_migrate_v12.py stats` before/after, diff | see the session notes |
| HA InfluxDB series copied | `tools/influx_migrate_v12.py ha-verify` | see the session notes |

## Live checks after a firmware update

* `curl -s http://power3/json | jq .meta.version`
* `mosquitto_sub -h mqtt -v -W 30 -t 'power3/#'`: counters about every 10 s, power on change
* `SELECT * FROM smartmeter WHERE time > now()-2m`: a point every 10 s
* http://power3/monitor: all eight rolling rows filled
