# Energy Meter Gateway with ESP8266

* Receive messages from IR serial interface and post them on syslog and influx database
* Validate meter readings against configured power limits to reject bogus data
* Optionally send power status (feeding to grid or high load) to WLED with UDP
* Optionally set limit of an OpenDTU inverter via MQTT to avoid high feed to grid
* Modern web UI with live power and rolling-window consumption (last/prev 24h, 7d, 30d, 365d)

## Web UI

The device serves a small set of pages; every page links to the others with a consistent
dark-themed nav bar:

* `/` – Home: live power, meter status, optional inverter and WLED cards
* `/monitor` – auto-refreshing live power plus consumption over **rolling windows ending
  now**: last 24h vs the 24h before it, last 7d vs the 7d before, last 30d vs prior 30d,
  last 365d vs prior 365d. Because each "last" and "prev" pair has identical length, the
  comparison is fair at any moment (unlike calendar periods, where a mid-day "today" only
  covered part of the day). The window edges are fetched from InfluxDB in a single batched
  multi-statement query (refreshed ~once a minute, matching the meter's sample rate); a
  window whose edge can't be read yet shows a dash.
* `/wled` – WLED settings: runtime-configurable color thresholds, hysteresis delays, colors,
  and a toggle for WLED live-receive mode (only shown when `WLED_LEDS` is defined)
* `/json` – stable JSON API (unchanged across versions)
* `/sml` – last raw SML record (binary download, unchanged)
* `/update` – OTA firmware upload
* `POST /reset` – reboot the device

## Features

### Meter Reading Validation
Readings are validated against configured maximum power thresholds (`PROD_KW_MAX`, `USAGE_KW_MAX`) to detect and reject anomalous data from the meter. If any reading exceeds the limits, the entire SML message is discarded as invalid. Rejected readings are never forwarded to InfluxDB, MQTT, WLED, or the live power display; the next reading is always compared against the last *accepted* baseline.

Two classes of outlier are detected:

**Backwards readings** — the A+ or A- counter is lower than the previous accepted value, which can indicate a transient bit error in the SML stream or a genuine meter reset after a power failure. Isolated bit errors are common and expected; a sustained run means the meter actually reset.

**Power-limit spikes** — the energy delta between two consecutive readings implies a power level that exceeds the configured maximum, which is physically impossible and indicates a corrupt reading.

**Frame-level corruption** (since v11.3) — every SML envelope's CRC-16 is verified before parsing, and the first valid frame's meter serial is "locked in" so later frames with a mismatching id/serial, impossible counter magnitudes, or unrealistic uptime jumps are dropped before they can pollute the baseline. Together these gates catch optical-IR bit errors that the delta-based check above cannot see.

#### Syslog messages

Every message below is at `LOG_NOTICE` or higher and appears under the device hostname.

| Message | Meaning |
|---------|---------|
| `Backwards #N: A+=X (was Y) A-=P (was Q)` | N-th consecutive backwards reading in the current run. X/P are the new (rejected) values; Y/Q are the last accepted values. A single `#1` followed by a normal reading is a harmless bit error. |
| `Meter reset confirmed after N backwards readings, new baseline A+=X A-=Y` | N consecutive backwards readings were seen — interpreted as a genuine meter reset after power loss. The baseline is updated and the live power display is cleared. |
| `Rejected: A+ delta=D/10 Wh in T s = K.KK kW, limit=L kW` | A+ energy jump of D/10 Wh over T seconds implies K.KK kW, exceeding the `USAGE_KW_MAX` limit of L kW. Reading discarded. |
| `Rejected: A- delta=D/10 Wh in T s = K.KK kW, limit=L kW` | Same for A- (backfeed), compared against `PROD_KW_MAX`. |
| `Stats: T readings, A accepted, B backwards (R runs, max M in a row), P power-rejected, I insane, crc=OK/BAD ovf=O, heap=H frag=F% rssi=R` | Periodic summary (~once per minute). T = SML frames that passed CRC and parsed cleanly; A accepted; B/P/I = rejections by category (see above); `crc=OK/BAD` = frames passing/failing CRC at the wire; `ovf` = oversized frames dropped; `heap`/`frag`/`rssi` give RAM and link-quality trend between Stats lines. |
| `Itron valid=…` | Periodic confirmation of a good reading (once per minute). |
| `Boot #N: reset=… prev=… heap=… maxblk=… frag=…% rssi=… rtc=…` | Emitted once per boot. `reset` is the ESP's own reason (`Software/System restart`, `Exception`, `Hardware Watchdog`, `External System`/brownout). `prev` shows whether the previous restart was a deliberate one (e.g. the web `/reset` endpoint) or an unexplained crash. The heap/RSSI snapshot helps spot creeping RAM exhaustion or weak WiFi over time. |
| `Locked meter serial = …` | Logged once on the first valid SML frame after boot. All later frames must match this serial; otherwise they are dropped as `Insane SML frame dropped`. |
| `Insane SML frame dropped: …` | A frame that parsed but failed a sanity gate (wrong meter id/serial, impossible counter magnitude, or unrealistic uptime gap). |

#### Interpreting the Stats line

- `B=0` — no outliers in the last minute, everything clean.
- `B>0, R=B, max=1` — every backwards reading was isolated (one per run). Classic bit-error pattern, not a problem.
- `B>0, max>1` — consecutive backwards readings in a cluster. Runs of 2–3 occasionally happen; 4 means one more would have triggered a meter-reset declaration.
- `max≥5` — a meter reset was confirmed; check for a preceding `Meter reset confirmed` warning.
- `P>0` — power-limit spikes occurred; inspect the individual `Rejected` lines to see how far the values exceeded the threshold. If the computed kW is only marginally above the limit, consider raising `USAGE_KW_MAX` / `PROD_KW_MAX`.
- `crc=OK/BAD` — a small bad count (a few percent) is normal noise from the optical IR link. A bad count comparable to OK suggests the sensor positioning has drifted; reseat the IR head.
- `I>0` — sanity gates dropped a frame. Look at the preceding `Insane SML frame dropped` line for which field was out of bounds.
- Repeated `Boot #N` lines minutes apart point to firmware crashes; check the `reset=` field — `Exception` and `Hardware Watchdog` are bugs, `External System` is a brownout / power glitch, `Software/System restart` after a `/reset` POST or a flash is expected.

### InfluxDB Storage

Accepted readings are posted to InfluxDB 1.x on host `job4`, database `power`, measurement
`energy`, tag `meter=<10-byte serial>`. Two integer fields are written:

| Field | Stores | Note |
|-------|--------|------|
| `watt` | A+ counter (consumption) in **Wh** | Despite the name, **not** instantaneous watts. Computed as `(aPlus + 5) / 10` from the raw 1/10 Wh meter counter. |
| `watt_out` | A- counter (backfeed) in **Wh** | Same misnomer. |

Both names are historical and kept for Grafana/InfluxDB compatibility. To get instantaneous
power in watts from these cumulative counters, apply a derivative, e.g.
`SELECT derivative(mean("watt"), 1s) * 3600 FROM energy WHERE time > now() - 1h GROUP BY time(1m)`.

### WLED Visual Feedback
Enabled if `WLED_LEDS` is defined. Provides color-coded visual feedback via WLED using UDP protocol (DRGB).

Color thresholds and priority (highest wins; all configurable via `/wled` or `platformio.ini`):

| Priority | Color | Condition | Default threshold |
|----------|-------|-----------|-------------------|
| 1 (highest) | **Green** | Backfeed too high (> `WLED_BACKFEED_TOO_HIGH` W) | 7000 W |
| 2 | **Red** | Consumption high (> `WLED_CONSUMPTION_HIGH` W) | 4000 W |
| 3 | **Blue** | Good backfeed (> `WLED_BACKFEED_GOOD` W) | 500 W |
| 4 (lowest) | **Violet** | Daytime error (no meter / DB / MQTT for too long) | `WLED_DAY_START`–`WLED_DAY_END` |

All colors use configurable hysteresis delays to avoid rapid flickering.

### Dynamic OpenDTU Inverter Limit
Enabled if `DTU_TOPIC` is defined. Automatically adjusts the inverter power limit via MQTT to keep grid backfeed within a target range (`BACKFEED_MIN` to `BACKFEED_MAX`).

**Note:** Set `BACKFEED_MIN` = `BACKFEED_MAX` to disable automatic limit adjustment (inverter runs at maximum).

Configuration in platformio.ini:
- `INVERTER_LIMIT` - Maximum inverter output (W)
- `BACKFEED_MIN` / `BACKFEED_MAX` - Target backfeed range (W)
- `LIMIT_CHECK_INTERVAL_S` - Minimum seconds between adjustments
- `LIMIT_ROUND_GRANULARITY` - Round limit changes to multiples of this value (W)

The MQTT topic format is: `DTU_TOPIC/INVERTER_SERIAL/cmd/limit_nonpersistent_absolute`

## Hardware

* Wemos Mini D1 ESP8266
* IR LED (940nm) used as photodiode receiver
* NPN transistor (e.g. BC546)

```
        3.3V
         │
       [10kΩ]
         │
         ├──────────────── GPIO Rx (Serial)
         │
         C
         │
        ─┤
   B ───┤   NPN (BC546)
   │    ─┤
   │     │
   │     E
   │     │
   ▼     │     IR LED (940nm)
  ─┴─    │      ▼  = anode (+)
   │     │     ─┴─ = cathode bar (−)
   │     │
   └─────┴── GND
```

The IR LED acts as a photodiode: meter IR pulses generate a small photocurrent
into the NPN base, pulling the collector low against the 10 kΩ pull-up to 3.3V.
The resulting active-low signal feeds the hardware serial Rx pin.

## Building and Uploading

```bash
# Activate PlatformIO environment
. ~/.platformio/penv/bin/activate

# Build
platformio run

# Upload via OTA (requires hostname/instance configured)
platformio run --target upload

# Upload via USB
# Set upload_port in platformio.ini first
platformio run --target upload --upload-port /dev/ttyUSB0
```
## Grafana Dashboard

![image](https://user-images.githubusercontent.com/32450554/144091536-94630249-3fab-48d6-807d-f92a7e7a44a1.png)

## Configuration

All power limits and thresholds are configurable in `platformio.ini` under the `[program]` section:

```ini
# Meter reading validation (reject bogus readings)
prod_kw_max = 15          # Max production/feed-in power (kW)
usage_kw_max = 20         # Max consumption power (kW)

# WLED visual feedback (see /wled page for runtime adjustment)
wled_leds = 60                   # Number of LEDs
wled_brightness = 100            # Master brightness (0–255)
wled_consumption_high = 4000     # Red: high consumption threshold (W)
wled_backfeed_too_high = 7000    # Green: backfeed too high threshold (W)
wled_backfeed_good = 500         # Blue: good backfeed threshold (W)
wled_day_start = 7               # Violet error indicator: start hour (0–23)
wled_day_end = 21                # Violet error indicator: end hour (0–23)

# Inverter control
inverter_limit = 800             # Max inverter output (W)
backfeed_min = 5000              # Target backfeed range min (W)
backfeed_max = 5000              # Target backfeed range max (W) - set equal to min to disable
limit_check_interval_s = 10      # Min seconds between limit adjustments
limit_round_granularity = 100    # Round limits to multiples of this (W)
```

Copy `inverter_template.ini` to `inverter.ini` and set your inverter serial number.

## SML Messages
Example message from my itron electricity meter
```
//0 1   2   3   4   5  level   
76,
    09, ae,01,00,00, 00,10,b6,88,
    62, 00,
    62, 00,
    72,
        65, 00,00,01,01,  // SML open()
        76,
            01,
            01,
            09, 00,00,00,00, 00,05,93,db,
            0b, 0a,01,49,54, 52,xx,xx,xx, xx,xx,
            72,
                62, 01,
                65, 00,05,93,dc,
            01,
    63, f5,44,
    00,
76,
    09, ae,01,00,00, 00,10,b6,89,
    62, 00,
    62, 00,
    72,
        65, 00,00,07,01,  // SML get_list()
        77,
            01,
            0b, 0a,01,49,54, 52,xx,xx,xx, xx,xx,
            07, 01,00,62,0a, ff,ff,
            72,
                62, 01,
                65, 00,05,93,dc,
            74,
                77,
                    07, 01,00,60,32, 01,01,  // obis meter id
                    01,
                    01,
                    01,
                    01,
                    04, 49,54,52,  // "ITR"
                    01,
                77,
                    07, 01,00,60,01, 00,ff,  // 07=6 bytes obis serial no
                    01,  // 01=no data, means default
                    01,
                    01,
                    01,
                    0b, 0a,01,49,54, 52,xx,xx,xx, xx,xx,
                    01,
                77,
                    07, 01,00,01,08, 00,ff,  // obis A+
                    65, 00,1c,01,04,
                    01,
                    62, 1e,  // 6=unsigned, 2=itemLen -> byte data: 0x1e=30 = unit
                    52, 03,  // 5=signed: 0x03=3 = scale
                    69, 00,00,00,00, 00,00,00,5a,  // 6=unsigned, 9=itemLen -> dword: 0x5a
                    01,
                77,
                    07, 01,00,02,08, 00,ff,  // obis A-
                    01,
                    01,
                    62, 1e,
                    52, 03,
                    69, 00,00,00,00, 00,00,00,00,
                    01,
            01,
            01,
    63, 3c,dc, 
    00,
76,
    09, ae,01,00,00, 00,10,b6,8a,
    62, 00,
    62, 00,
    72,
        65, 00,00,02,01,  // SML close()
        71,
            01,
    63, 67,a9,
    00,
00,00,
```

## Documentation
* http://www.schatenseite.de/2016/05/30/smart-message-language-stromzahler-auslesen/ 
* https://www.bsi.bund.de/SharedDocs/Downloads/DE/BSI/Publikationen/TechnischeRichtlinien/TR03109/TR-03109-1_Anlage_Feinspezifikation_Drahtgebundene_LMN-Schnittstelle_Teilb.pdf?__blob=publicationFile
* https://www.edi-energy.de/index.php?id=38&tx_bdew_bdew%5Buid%5D=1158&tx_bdew_bdew%5Baction%5D=download&tx_bdew_bdew%5Bcontroller%5D=Dokument&cHash=4ee767287a68d3f0fbbc3caf2974d97f
* https://www.itron.com/-/media/feature/products/documents/brochure/openway_kl.pdf
* https://www.devolo.ch/fileadmin/Web-Content/DE/products/sg/3hz-basiszaehler/documents/de/Handbuch_devolo_3.HZ_Basiszaehler_D3B60_D3B100_0419_Online_DE.pdf
