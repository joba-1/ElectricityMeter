#!/usr/bin/env python3
"""Rewrite Grafana panels from power.energy (watt, watt_out) to power.smartmeter (a_plus, a_minus).

  plan    <backup-dir> <out-dir>   transform every dashboard JSON in backup-dir that
                                   reads the power DB; write the new JSON and a list of
                                   (old, new) query pairs to out-dir
  compare <out-dir> <from> <to>    run every old and new query through Grafana for a
                                   fixed range that ends before the cutover; they must
                                   return identical frames
  save    <out-dir>                upload the new dashboards (the backups stay in backup-dir)

Only targets that really read the power DB are touched: target datasource = the
power datasource, or no target datasource and the panel's is, or a fully
qualified "power"."autogen"."energy". Other `energy` measurements (energies DB,
openDtu) stay as they are. Every power target carries an alias, so series names
and overrides are unaffected (checked).

Uses ~/.claude/skills/grafana/scripts/grafana.sh (token in ~/.config/grafana/token).
"""

import copy
import json
import os
import re
import subprocess
import sys
import tempfile

POWER_DS = "syZc2Tdnz"
GRAFANA = os.path.expanduser("~/.claude/skills/grafana/scripts/grafana.sh")


def is_power_target(t, panel):
    tds = (t.get("datasource") or {}).get("uid")
    pds = (panel.get("datasource") or {}).get("uid")
    q = t.get("query") or ""
    return tds == POWER_DS or (tds in (None, "-") and pds == POWER_DS) or '"power"."autogen"."energy"' in q


def rewrite_raw(q):
    q = q.replace('"power"."autogen"."energy"', '"power"."autogen"."smartmeter"')
    q = re.sub(r'FROM\s+"energy"', 'FROM "smartmeter"', q)
    q = re.sub(r'FROM\s+energy\b', 'FROM smartmeter', q)
    q = re.sub(r'\bwatt_out\b', 'a_minus', q)
    q = re.sub(r'\bwatt\b', 'a_plus', q)
    return q


def rewrite_target(t):
    t = copy.deepcopy(t)
    if t.get("query"):
        t["query"] = rewrite_raw(t["query"])
    if t.get("measurement") == "energy":
        t["measurement"] = "smartmeter"
    for sel in t.get("select") or []:
        for part in sel:
            if part.get("type") == "field":
                part["params"] = [{"watt": "a_plus", "watt_out": "a_minus"}.get(p, p) for p in part["params"]]
    return t


def panels(d):
    for p in d.get("panels", []):
        yield p
        yield from p.get("panels", [])


def cmd_plan(backup, out):
    os.makedirs(out, exist_ok=True)
    pairs = []
    for f in sorted(os.listdir(backup)):
        doc = json.load(open(os.path.join(backup, f)))
        dash = doc["dashboard"]
        changed = 0
        for p in panels(dash):
            for i, t in enumerate(p.get("targets") or []):
                if not is_power_target(t, p):
                    continue
                nt = rewrite_target(t)
                if nt == t:
                    continue
                if not t.get("alias"):
                    sys.exit(f"{dash['title']} / {p['title']} / {t.get('refId')}: no alias, "
                             f"series name would change - handle by hand")
                p["targets"][i] = nt
                changed += 1
                ds = (t.get("datasource") or {}).get("uid")
                if ds in (None, "-"):
                    ds = (p.get("datasource") or {}).get("uid")
                pairs.append({"dashboard": dash["title"], "uid": dash["uid"], "panel": p["title"],
                              "refId": t.get("refId"), "ds": ds, "old": t, "new": nt})
        if changed:
            json.dump(doc, open(os.path.join(out, f), "w"), indent=1, ensure_ascii=False)
            print(f"{dash['title']} ({dash['uid']}): {changed} targets")
    json.dump(pairs, open(os.path.join(out, "pairs.json"), "w"), indent=1, ensure_ascii=False)
    print(f"{len(pairs)} query pairs -> {out}/pairs.json")


def ds_query(target, ds, frm, to):
    body = {"queries": [dict(target, refId="A", datasource={"uid": ds}, intervalMs=60000, maxDataPoints=2000)],
            "from": frm, "to": to}
    with tempfile.NamedTemporaryFile("w", suffix=".json") as f:
        json.dump(body, f)
        f.flush()
        r = subprocess.run([GRAFANA, "query", f.name], capture_output=True, text=True, check=True)
    res = json.loads(r.stdout)["results"]["A"]
    if res.get("error"):
        raise RuntimeError(res["error"])
    return [f["data"]["values"] for f in res.get("frames", [])]


def cmd_compare(out, frm, to):
    pairs = json.load(open(os.path.join(out, "pairs.json")))
    bad = 0
    for p in pairs:
        old = ds_query(p["old"], p["ds"], frm, to)
        new = ds_query(p["new"], p["ds"], frm, to)
        n = sum(len(v[0]) for v in old if v)
        same = old == new
        bad += not same
        print(f"{'OK  ' if same else 'DIFF'} {p['dashboard']} / {p['panel']} / {p['refId']}: {n} rows")
    print(f"{len(pairs) - bad}/{len(pairs)} identical")
    sys.exit(1 if bad else 0)


def cmd_save(out):
    for f in sorted(os.listdir(out)):
        if f == "pairs.json" or not f.endswith(".json"):
            continue
        doc = json.load(open(os.path.join(out, f)))
        save = {"dashboard": doc["dashboard"], "folderUid": doc["meta"].get("folderUid"),
                "overwrite": True, "message": "ElectricityMeter v12: power.energy -> power.smartmeter"}
        path = os.path.join(out, "save-" + f)
        json.dump(save, open(path, "w"))
        r = subprocess.run([GRAFANA, "save", path], capture_output=True, text=True)
        print(doc["dashboard"]["title"], "->", r.stdout.strip() or r.stderr.strip())


if __name__ == "__main__":
    cmd, *a = sys.argv[1:] or ["-h"]
    if cmd == "plan" and len(a) == 2:
        cmd_plan(*a)
    elif cmd == "compare" and len(a) == 3:
        cmd_compare(*a)
    elif cmd == "save" and len(a) == 1:
        cmd_save(*a)
    else:
        sys.exit(__doc__)
