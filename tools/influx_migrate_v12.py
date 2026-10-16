#!/usr/bin/env python3
"""Migrate the v11 meter data in InfluxDB 1.x to the v12 format and prove it unchanged.

  meter         power.energy  (meter=<serial> watt, watt_out)
                -> power.smartmeter (device, devicetype, meter; a_plus, a_minus)
  meter-verify  point-by-point: every old (time, watt, watt_out) must reappear
                as (time, a_plus, a_minus) in the same meter series
  ha            homeassistant.<unit> series entity_id=<from> -> entity_id=<to>
                (HA's InfluxDB integration; tag rename, all fields kept)
  ha-verify     point-by-point compare of the two HA series
  ha-drop       drop the old HA series (only after ha-verify passed; dump first)
  dump          write a query result as line protocol (backup before destructive steps)

All copies keep the original timestamps (ns), so they are idempotent: a rerun
overwrites the same points. Standard library only; the server has no auth.
See docs/spec.md "Data migration".
"""

import argparse
import json
import sys
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone

INFLUX = "http://influx:8086"
CHUNK = timedelta(days=30)
START = datetime(2015, 1, 1, tzinfo=timezone.utc)

# v11 series -> v12 tags. `main` is the OCR camera reader (ocv-cam Stromableser,
# 2020-2021, A+ only), kept distinguishable from the gateway's own series.
DEVICE_TAGS = {
    "main": ("ocv-cam", "Stromableser"),
}
DEFAULT_DEVICE = ("power3", "power")


def query(db, q):
    url = f"{INFLUX}/query?" + urllib.parse.urlencode({"db": db, "q": q, "epoch": "ns"})
    with urllib.request.urlopen(url, timeout=120) as r:
        res = json.load(r)["results"][0]
    if "error" in res:
        raise RuntimeError(f"{q}: {res['error']}")
    out = []
    for s in res.get("series", []):
        cols = s["columns"]
        out += [dict(zip(cols, v)) for v in s["values"]]
    return out


def write(db, lines):
    if not lines:
        return
    url = f"{INFLUX}/write?" + urllib.parse.urlencode({"db": db, "precision": "ns"})
    for i in range(0, len(lines), 5000):
        body = ("\n".join(lines[i:i + 5000]) + "\n").encode()
        req = urllib.request.Request(url, data=body, method="POST")
        with urllib.request.urlopen(req, timeout=120) as r:
            if r.status != 204:
                raise RuntimeError(f"write: HTTP {r.status}")


def esc_tag(v):
    return str(v).replace("\\", "\\\\").replace(",", "\\,").replace("=", "\\=").replace(" ", "\\ ")


def fmt_field(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(float(v))       # always float, like the original fields
    return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'


def chunks(end):
    t = START
    while t < end:
        yield t, min(t + CHUNK, end)
        t += CHUNK


def ts(t):
    return t.strftime("%Y-%m-%dT%H:%M:%SZ")


def now():
    return datetime.now(timezone.utc)


def tag_values(db, measurement, key):
    return [r["value"] for r in query(db, f'SHOW TAG VALUES FROM "{measurement}" WITH KEY = "{key}"')]


# ---------------------------------------------------------------- meter

def meter_series():
    return tag_values("power", "energy", "meter")


def old_points(meter, a, b):
    return query("power", f"SELECT watt, watt_out FROM energy WHERE meter='{meter}' "
                          f"AND time >= '{ts(a)}' AND time < '{ts(b)}'")


def new_points(meter, a, b):
    return query("power", f"SELECT a_plus, a_minus FROM smartmeter WHERE meter='{meter}' "
                          f"AND time >= '{ts(a)}' AND time < '{ts(b)}'")


def cmd_meter(args):
    end = now()
    for meter in meter_series():
        device, devicetype = DEVICE_TAGS.get(meter, DEFAULT_DEVICE)
        tags = f"smartmeter,device={esc_tag(device)},devicetype={esc_tag(devicetype)},meter={esc_tag(meter)}"
        total = 0
        for a, b in chunks(end):
            lines = []
            for p in old_points(meter, a, b):
                fields = []
                if p["watt"] is not None:
                    fields.append("a_plus=" + fmt_field(p["watt"]))
                if p["watt_out"] is not None:
                    fields.append("a_minus=" + fmt_field(p["watt_out"]))
                if fields:
                    lines.append(f"{tags} {','.join(fields)} {p['time']}")
            if not args.dry_run:
                write("power", lines)
            total += len(lines)
        print(f"meter={meter} device={device}: {total} points {'(dry run)' if args.dry_run else 'copied'}")


def cmd_meter_verify(args):
    end = now()
    ok = True
    for meter in meter_series():
        n = 0
        bad = 0
        for a, b in chunks(end):
            old = {p["time"]: (p["watt"], p["watt_out"]) for p in old_points(meter, a, b)}
            new = {p["time"]: (p["a_plus"], p["a_minus"]) for p in new_points(meter, a, b)}
            for t, v in old.items():
                n += 1
                if new.get(t) != v:
                    bad += 1
                    if bad <= 5:
                        print(f"  MISMATCH meter={meter} t={t}: old={v} new={new.get(t)}")
        status = "OK" if bad == 0 else "FAIL"
        ok &= bad == 0
        print(f"meter={meter}: {n} old points, {bad} mismatches -> {status}")
    sys.exit(0 if ok else 1)


# ---------------------------------------------------------------- HA

def ha_points(measurement, entity, a, b):
    return query("homeassistant", f'SELECT * FROM "{measurement}" WHERE entity_id=\'{entity}\' '
                                  f"AND time >= '{ts(a)}' AND time < '{ts(b)}'")


def cmd_ha(args):
    end = now()
    total = 0
    for a, b in chunks(end):
        lines = []
        for p in ha_points(args.measurement, args.src, a, b):
            t = p.pop("time")
            p.pop("entity_id")
            tags = {"entity_id": args.dst}
            if p.get("domain") is not None:
                tags["domain"] = p.pop("domain")
            if args.friendly_name and "friendly_name_str" in p:
                p["friendly_name_str"] = args.friendly_name
            fields = ",".join(f"{k}={fmt_field(v)}" for k, v in p.items() if v is not None)
            tagstr = ",".join(f"{k}={esc_tag(v)}" for k, v in sorted(tags.items()))
            lines.append(f"{esc_tag(args.measurement)},{tagstr} {fields} {t}")
        if not args.dry_run:
            write("homeassistant", lines)
        total += len(lines)
    print(f"{args.measurement}: {args.src} -> {args.dst}: {total} points "
          f"{'(dry run)' if args.dry_run else 'copied'}")


def cmd_ha_verify(args):
    end = now()
    n = bad = 0
    for a, b in chunks(end):
        old = {p["time"]: p["value"] for p in ha_points(args.measurement, args.src, a, b)}
        new = {p["time"]: p["value"] for p in ha_points(args.measurement, args.dst, a, b)}
        for t, v in old.items():
            n += 1
            if new.get(t) != v:
                bad += 1
                if bad <= 5:
                    print(f"  MISMATCH t={t}: {args.src}={v} {args.dst}={new.get(t)}")
    print(f"{args.src}: {n} points, {bad} missing/different in {args.dst} -> {'OK' if bad == 0 else 'FAIL'}")
    sys.exit(0 if bad == 0 else 1)


def cmd_ha_drop(args):
    q = f'DROP SERIES FROM "{args.measurement}" WHERE entity_id=\'{args.src}\''
    url = f"{INFLUX}/query?" + urllib.parse.urlencode({"db": "homeassistant", "q": q})
    req = urllib.request.Request(url, data=b"", method="POST")
    with urllib.request.urlopen(req, timeout=120) as r:
        print(q, "->", r.status, r.read().decode().strip())


# ---------------------------------------------------------------- backup

def cmd_dump(args):
    end = now()
    n = 0
    with open(args.file, "w") as f:
        for a, b in chunks(end):
            for p in query(args.db, f"{args.select} AND time >= '{ts(a)}' AND time < '{ts(b)}'"):
                t = p.pop("time")
                tags = ",".join(f"{k}={esc_tag(p.pop(k))}" for k in args.tags if p.get(k) is not None)
                fields = ",".join(f"{k}={fmt_field(v)}" for k, v in p.items() if v is not None)
                f.write(f"{esc_tag(args.measurement)}{',' + tags if tags else ''} {fields} {t}\n")
                n += 1
    print(f"{n} points -> {args.file}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("meter")
    p.add_argument("--dry-run", action="store_true")
    sub.add_parser("meter-verify")
    for name in ("ha", "ha-verify", "ha-drop"):
        p = sub.add_parser(name)
        p.add_argument("--measurement", default="Wh")
        p.add_argument("--from", dest="src", required=True)
        p.add_argument("--to", dest="dst", required=name != "ha-drop")
        if name == "ha":
            p.add_argument("--friendly-name")
            p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("dump")
    p.add_argument("db")
    p.add_argument("measurement")
    p.add_argument("select", help="SELECT ... FROM ... WHERE <cond> (time range is appended)")
    p.add_argument("file")
    p.add_argument("--tags", nargs="*", default=[], help="result columns that are tags")
    args = ap.parse_args()
    {"meter": cmd_meter, "meter-verify": cmd_meter_verify, "ha": cmd_ha,
     "ha-verify": cmd_ha_verify, "ha-drop": cmd_ha_drop, "dump": cmd_dump}[args.cmd](args)


if __name__ == "__main__":
    main()
