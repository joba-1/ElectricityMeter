#!/usr/bin/env python3
"""Old vs new: the meter queries of the scripts that read power.energy must give
identical results on power.smartmeter for a range that ends before the cutover.

  compare_queries_v12.py <end-iso> [days]

<end-iso> is the cutover time (first v12 point); the range is [end - days, end).
The queries are the ones in Joba_Modbus/misc and battery-optimiser, with their
time conditions replaced by the fixed range. See docs/test.md.
"""

import json
import sys
import urllib.parse
import urllib.request
from datetime import datetime, timedelta

INFLUX = "http://influx:8086"
METER = "0a-01-49-54-52-00-03-4f-65-82"

# (name, old, new) with {R} = time range condition
PAIRS = [
    ("energy_daily grid_import",
     f"SELECT spread(watt)/1000 FROM energy WHERE meter='{METER}' AND {{R}} GROUP BY time(1d) tz('Europe/Berlin')",
     f"SELECT spread(a_plus)/1000 FROM smartmeter WHERE meter='{METER}' AND {{R}} GROUP BY time(1d) tz('Europe/Berlin')"),
    ("energy_daily grid_export",
     f"SELECT spread(watt_out)/1000 FROM energy WHERE meter='{METER}' AND {{R}} GROUP BY time(1d) tz('Europe/Berlin')",
     f"SELECT spread(a_minus)/1000 FROM smartmeter WHERE meter='{METER}' AND {{R}} GROUP BY time(1d) tz('Europe/Berlin')"),
    ("daily_report/house_profile import",
     f"SELECT non_negative_derivative(mean(watt),1h) FROM energy WHERE meter='{METER}' AND {{R}} GROUP BY time(1h)",
     f"SELECT non_negative_derivative(mean(a_plus),1h) FROM smartmeter WHERE meter='{METER}' AND {{R}} GROUP BY time(1h)"),
    ("daily_report/house_profile export",
     f"SELECT non_negative_derivative(mean(watt_out),1h) FROM energy WHERE meter='{METER}' AND {{R}} GROUP BY time(1h)",
     f"SELECT non_negative_derivative(mean(a_minus),1h) FROM smartmeter WHERE meter='{METER}' AND {{R}} GROUP BY time(1h)"),
    ("pv_refine export max",
     "SELECT max(exp) FROM (SELECT non_negative_derivative(max(watt_out),1h) AS exp FROM energy "
     "WHERE {R} GROUP BY time(15m)) GROUP BY time(1h)",
     "SELECT max(exp) FROM (SELECT non_negative_derivative(max(a_minus),1h) AS exp FROM smartmeter "
     "WHERE {R} GROUP BY time(15m)) GROUP BY time(1h)"),
    ("backtest 5m",
     f"SELECT non_negative_derivative(max(watt),1h) AS imp, non_negative_derivative(max(watt_out),1h) AS ex "
     f"FROM energy WHERE meter='{METER}' AND {{R}} GROUP BY time(5m) fill(previous)",
     f"SELECT non_negative_derivative(max(a_plus),1h) AS imp, non_negative_derivative(max(a_minus),1h) AS ex "
     f"FROM smartmeter WHERE meter='{METER}' AND {{R}} GROUP BY time(5m) fill(previous)"),
    ("backtest raw / influx_client cumulative",
     f"SELECT watt AS import_wh, watt_out AS export_wh FROM energy WHERE meter='{METER}' AND {{R}}",
     f"SELECT a_plus AS import_wh, a_minus AS export_wh FROM smartmeter WHERE meter='{METER}' AND {{R}}"),
    ("verify_meter_phases",
     f"SELECT (non_negative_derivative(first(\"watt\"),1s) - non_negative_derivative(first(\"watt_out\"),1s))*3600 "
     f"FROM energy WHERE meter='{METER}' AND {{R}} GROUP BY time(1m) fill(linear)",
     f"SELECT (non_negative_derivative(first(\"a_plus\"),1s) - non_negative_derivative(first(\"a_minus\"),1s))*3600 "
     f"FROM smartmeter WHERE meter='{METER}' AND {{R}} GROUP BY time(1m) fill(linear)"),
]


def query(q):
    url = f"{INFLUX}/query?" + urllib.parse.urlencode({"db": "power", "q": q, "epoch": "s"})
    with urllib.request.urlopen(url, timeout=300) as r:
        res = json.load(r)["results"][0]
    if "error" in res:
        raise RuntimeError(res["error"])
    return [s["values"] for s in res.get("series", [])]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    end = datetime.fromisoformat(sys.argv[1].replace("Z", "+00:00"))
    days = int(sys.argv[2]) if len(sys.argv) > 2 else 35
    r = f"time >= '{(end - timedelta(days=days)).strftime('%Y-%m-%dT%H:%M:%SZ')}' " \
        f"AND time < '{end.strftime('%Y-%m-%dT%H:%M:%SZ')}'"
    bad = 0
    for name, old, new in PAIRS:
        a, b = query(old.replace("{R}", r)), query(new.replace("{R}", r))
        rows = sum(len(s) for s in a)
        same = a == b and rows > 0
        bad += not same
        print(f"{'OK  ' if same else 'DIFF'} {name}: {rows} rows")
    print(f"{len(PAIRS) - bad}/{len(PAIRS)} identical")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
