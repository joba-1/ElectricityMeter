# ElectricityMeter — specification (v12)

ESP8266 gateway (`power3`) for an Itron 3.HZ meter: reads SML frames from the IR
interface (one per meter second), validates them, and publishes counters, live
power and status to InfluxDB, MQTT/Home Assistant, syslog and WLED. Hardware, web
UI and the frame validation gates are described in the [Readme](../Readme.md).
This file records the v12 design and why it is that way.

## Identity

| Item | Value | Source |
|------|-------|--------|
| device | hostname, `power3` | `${program.name}${program.instance}` |
| devicetype | firmware name, `power` | `${program.name}` |
| meter | Itron server id as hex, `0a-01-49-54-52-00-03-4f-65-82` | first frame that passes every gate ("locked serial") |

The same three appear as InfluxDB tags and, for Home Assistant, as the device
(name = hostname, model = firmware name) plus the `serial` sensor.

## Live power (PowerEstimator)

`lib/PowerEstimator/src/PowerEstimator.h`, header-only, unit tested natively.

The counters have 0.1 Wh resolution and arrive once per second, so one unit per
second is 360 W. Any estimate trades latency against quantisation noise. v11
reset its window after 3 frames that ticked, which meant ±7 % steps at high load
and 20–100 % errors at low load, where the value faded towards 0 between ticks.

v12, per direction:

* **Windows run between edges**, i.e. frames where the counter changed. At an edge the
  true energy is known to within min(1 unit, rate·1 s), so the relative error of an
  edge-to-edge window is about 1/max(Δunits, Δseconds). The window grows until that
  is at most 1/30 (`QUALITY`): about 2–3 s at 5 kW and about 30 s at 100 W.
* **Step detection.** If the newest segment or a 6-unit window disagrees with the long
  window by more than twice the sum of both error bounds, a barrier is set and the long
  window restarts there. A switched-on load is followed within 1–2 s. After a step the
  estimate settles monotonically instead of averaging across the step.
* **Decay bound.** With no edge for g seconds, the rate is below 1 unit/g. Clamping to
  that bound is the fastest decay the data allows when a load switches off, and it never
  undershoots.
* **Edge cases.** Edges older than 300 s are ignored, so power reads 0 after 5 min
  without a change. A frame that goes backwards resets both estimators.

Simulated accuracy (the `test/test_power` meter model, worst case after settling):

| Load | v11 max / rms error | v12 max / rms error |
|------|--------------------|--------------------|
| 2000 W | 10 % / 5.9 % | 2.0 % / 1.4 % |
| 500 W | 44 % / 23 % | 2.8 % / 1.9 % |
| 100 W | 100 % / 53 % | 1.9 % / 1.5 % |

## InfluxDB (database `power`)

Since v12 the gateway writes one point every 10 accepted frames (about 10 s):

```
smartmeter,device=power3,devicetype=power,meter=<serial> a_plus=<Wh>,a_minus=<Wh>,p_in=<W>,p_out=<W>,detailed=<bool>
```

| Field | Type | Meaning |
|-------|------|---------|
| `a_plus` / `a_minus` | float, Wh, 0.1 Wh resolution | cumulative import / export counter |
| `p_in` / `p_out` | float, W | live power from PowerEstimator |
| `detailed` | bool | meter delivers 0.1 Wh resolution (false = whole kWh after a power cut, PIN needed) |

Reasons:

* **A new measurement instead of new fields in `energy`.** The old measurement could
  stay untouched as a backup, and a rename that leaves a mixed schema behind is
  impossible.
* **Names `a_plus`/`a_minus`.** The v11 names `watt`/`watt_out` stored Wh, not W, and
  were a constant source of mistakes. OBIS A+/A− is what the meter calls them.
* **10 s cadence.** Roughly 8.6k points a day: cheap for InfluxDB. It gives Grafana
  power resolution without the derivative tricks and keeps the ESP's blocking HTTP
  posts rare. The serial RX buffer was raised to 1 KB so one post cannot overrun a
  frame.
* **Floats.** v11 wrote untyped numbers, so `watt` was already a float. The migrated
  history and the new 0.1 Wh values share one type.

### Data migration (v11 → v12)

`tools/influx_migrate_v12.py meter` copied every point of `energy` into `smartmeter`
with the original timestamp:

* `watt` → `a_plus` and `watt_out` → `a_minus`.
* Tags: `device=power3,devicetype=power`, keeping `meter`.
* The 2020–2021 OCR-camera series `meter=main` became `device=ocv-cam,devicetype=Stromableser`.

History therefore has whole-Wh counters and no `p_*` fields. `energy` is kept,
read-only, as a backup (cutover 2026-10-16T12:47:39Z, last v11 point
12:47:38Z).

## MQTT / Home Assistant

The broker alias is `mqtt` (runtime configurable, see Readme). MQTT is now always
compiled in. The OpenDTU inverter cap stays optional (`DTU_TOPIC`).

| Topic | Payload | Retained | When |
|-------|---------|----------|------|
| `power3/a_plus`, `power3/a_minus` | Wh, one decimal | yes | on change, at most every 10 s |
| `power3/p_in`, `power3/p_out` | W | no | change ≥ max(10 W, 2 %), else every 60 s |
| `power3/p_net` | W, + = import | no | with `p_in`/`p_out` |
| `power3/serial` | meter serial (hex) | yes | on change / connect |
| `power3/detailed` | `ON`/`OFF` | yes | on change / connect |
| `power3/influx` | `ON`/`OFF` (last post 2xx) | yes | on change / connect |
| `power3/influx_status` | last HTTP code | yes | on change / connect |
| `power3/LWT` | `Online`/`Offline` | yes | availability for all entities |
| `power3/Version` | firmware version | yes | connect |

Home Assistant discovery runs under `homeassistant/<component>/power3/<key>/config`.
It is sent retained at every connect and again on HA's birth message
(`homeassistant/status` = `online`). Entities:

* `sensor.power3_a_plus`, `sensor.power3_a_minus`: energy, `total_increasing`
* `sensor.power3_p_in`, `sensor.power3_p_out`, `sensor.power3_p_net`
* diagnostics: `sensor.power3_serial`, `binary_sensor.power3_detailed`,
  `binary_sensor.power3_influx`, `sensor.power3_influx_status`

Reasons:

* **Counters are retained** because v11's were not. After an HA restart HA had no
  value until the counter moved. Power is not retained: a stale power reading would
  be wrong, not merely old.
* **The deadband and counter limits** keep HA's recorder (SQLite on the QNAP) and the
  `homeassistant` InfluxDB at about the v11 row rate, while power still updates about
  every second when it really changes.
* **`default_entity_id` instead of `object_id`.** HA removed `object_id` in 2026.4.
* **The discovery payloads are about 550 B**, so the PubSubClient buffer was raised
  from 256 to 768 B. A too-small buffer makes `publish()` fail silently.

### HA migration (v11 → v12)

v11 had two YAML MQTT sensors (`sensor.energie_bezug`, `sensor.energie_produktion`).
Recorder history, long-term statistics and the energy dashboard hang on them.
`tools/ha_migrate_v12.py`:

1. **`rekey`** (HA stopped for about a minute): removes the YAML sensors and gives their
   registry entries the discovery unique_ids `power3_a_plus`/`power3_a_minus`. When the
   v12 firmware announces itself, HA adopts these entries, with their history, into the
   new device.
2. **`rename`**: renames them to `sensor.power3_a_plus`/`sensor.power3_a_minus`. HA's
   recorder moves states and statistics itself. The energy dashboard is re-pointed.
3. **The `homeassistant` InfluxDB** (measurement `Wh`) gets its old `entity_id` tags
   copied to the new ids by `tools/influx_migrate_v12.py ha` and verified point by point.
   The old series are dumped and then dropped.

### HA statistics repair (pre-existing damage)

The HA long-term statistics of both counters had been broken since the v11.0–v11.2
bit errors (2026-05-24 to 2026-06-18). Corrupt states such as 9.2e17 Wh put jumps of
up to 1e18 into `sum`. At that magnitude a float64 resolves only ~1 kWh, so the energy
dashboard showed no grid import or export from June on.
`tools/ha_fix_statistics_v12.py` (run on 2026-10-16 after a DB backup,
`home-assistant_v2.db.bak-v12-stats-20261016-154932` on the QNAP) does the following:

* **Rebuild.** Hourly sums are rebuilt from the hourly states, with absurd, backwards
  or too fast rises skipped. The allowance is counted from the last real rise: on
  2026-06-06 and 06-17 HA's state froze all day and caught up in the evening.
* **Write-back.** `recorder/adjust_sum_statistics` removes the offset from the hourly
  and 5-minute tables, and `recorder/import_statistics` writes the rebuilt hourly rows.
* **Async recorder.** The recorder applies both calls asynchronously from its queue,
  so the tool polls until each step is visible. An earlier run with fixed sleeps
  raced, and its first import overwrote the original spike-hour states. The rebuild
  therefore takes its states from the stats snapshot taken before the rename
  (`--snapshot`).

Result: for all 144 days since 2026-05-24 the daily HA import and export match the
meter counters in InfluxDB within 100 Wh. The only exception is a deliberate −173 Wh
on 2026-05-27, the rounding remainder of the 5-minute base. The 5-minute values of
the 10 days before the repair stay coarse until HA purges them.

## Consumers updated at the cutover

| Consumer | Change |
|----------|--------|
| Grafana: Energie, PV Energie, Stromverbrauch, PV Strategie | 19 targets `energy`/`watt`/`watt_out` → `smartmeter`/`a_plus`/`a_minus` (`tools/grafana_migrate_v12.py`) |
| Joba_Modbus `misc/energy_daily.py`, `misc/verify_meter_phases.py`, `misc/optimize/optimize/influx_client.py` | same rename |
| battery-optimiser `daily_report.py`, `house_profile.py`, `pv_refine.py`, `backtest.py` | same rename |
| `/monitor` rolling windows (firmware) | query `smartmeter` filtered by `device` |

All of these queries are cadence independent, which was checked: they use
`spread`/`derivative` with `GROUP BY time()`, or resample before `diff()`.
