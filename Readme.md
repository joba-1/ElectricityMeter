# Energy Meter Gateway with ESP8266

* Receive messages from IR serial interface and post them on syslog and influx database
* Validate meter readings against configured power limits to reject bogus data
* Optionally send power status (feeding to grid or high load) to WLED with UDP
* Optionally set limit of an OpenDTU inverter via MQTT to avoid high feed to grid

## Features

### Meter Reading Validation
Readings are validated against configured maximum power thresholds (`PROD_KW_MAX`, `USAGE_KW_MAX`) to detect and reject anomalous data from the meter. If any reading exceeds the limits, the entire SML message is discarded as invalid.

### WLED Visual Feedback
Enabled if `WLED_LEDS` is defined. Provides color-coded visual feedback via WLED using UDP protocol (DRGB).

Color thresholds (configurable in platformio.ini):
- **Red** - High consumption (> `WLED_CONSUMPTION_HIGH` W, default 4000 W)
- **Blue** - Too high backfeed (> `WLED_BACKFEED_TOO_HIGH` W, default disabled)
- **Cyan** - Very high backfeed (> `WLED_BACKFEED_VERY_HIGH` W, default disabled)
- **Green** - Good backfeed (> `WLED_BACKFEED_GOOD` W, default 200 W)

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
* IR LED (940nm) as receiver
* NPN transistor (e.g. BC546)

```
GND -- -_LED_+ -- NPN_B
GND -- E_NPN_C -- R_10k -- V_3.3
NPN_C -- GPIO_Rx
```

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

# WLED visual feedback thresholds (in W)
wled_consumption_high = 4000     # Red warning
wled_backfeed_too_high = 99999   # Blue (disabled by default)
wled_backfeed_very_high = 99999  # Cyan (disabled by default)
wled_backfeed_good = 200         # Green

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
