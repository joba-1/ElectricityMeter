#!/usr/bin/env python3
"""Move the meter entities in Home Assistant from configuration.yaml to MQTT discovery (v12).

v11: two MQTT sensors in configuration.yaml (unique_id energie_bezug /
energie_produktion, entity_id sensor.energie_bezug / sensor.energie_produktion)
on topics power3/Wh_In and power3/Wh_Out.
v12: the firmware announces its entities by MQTT discovery (unique_id
power3_a_plus / power3_a_minus, ...).

The recorder history and long-term statistics (and the energy dashboard) hang
on these two entities, so they are carried over instead of starting new ones:

  rekey     (HA stopped ~1 min) remove the two YAML sensors and re-key their
            registry entries to the discovery unique_ids. When the v12 firmware
            announces power3_a_plus, HA adopts the existing entity with its
            history. Backups of both files are kept next to them on the QNAP.
  rename    (token) rename the adopted entities to sensor.power3_a_plus /
            sensor.power3_a_minus; HA's recorder moves states and statistics
            itself. Then point the energy dashboard at the new ids.
  stats     (token) dump hourly long-term statistics of the given entities as
            JSON, for the before/after comparison

Run on job6; the QNAP has no python. Token: ha-token.sh (tmpfs, never on disk).
"""

import argparse
import json
import os
import ssl
import subprocess
import sys
import time
from datetime import datetime, timedelta, timezone

HA_URL = os.environ.get("HA_URL", "https://banzhaf.chickenkiller.com:8123")
TOKEN_FILE = "/run/user/1000/ha-token"
CONFIG = "/share/Docker/homeassistant/config"
DOCKER = "sudo /share/CACHEDEV1_DATA/.qpkg/container-station/bin/docker"
CONTAINER_PREFIX = "home-assistant"

# old unique_id -> (new unique_id, old entity_id, new entity_id)
ENTITIES = {
    "energie_bezug": ("power3_a_plus", "sensor.energie_bezug", "sensor.power3_a_plus"),
    "energie_produktion": ("power3_a_minus", "sensor.energie_produktion", "sensor.power3_a_minus"),
}
YAML_BLOCK = '''    - name: "Energie Bezug"
      unique_id: energie_bezug
      state_topic: "power3/Wh_In"
      unit_of_measurement: "Wh"
      device_class: energy
      state_class: total_increasing
    - name: "Energie Produktion"
      unique_id: energie_produktion
      state_topic: "power3/Wh_Out"
      unit_of_measurement: "Wh"
      device_class: energy
      state_class: total_increasing

'''


def qnap(cmd, stdin=None):
    r = subprocess.run(["ssh", "qnap", cmd], input=stdin, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"qnap: {cmd}: {r.stderr.strip()}")
    return r.stdout


def container():
    names = qnap(f"{DOCKER} ps -a --format '{{{{.Names}}}}'").split()
    found = [n for n in names if n.startswith(CONTAINER_PREFIX)]
    if len(found) != 1:
        sys.exit(f"expected one {CONTAINER_PREFIX}* container, found {found}")
    return found[0]


def cmd_rekey(args):
    reg_path = f"{CONFIG}/.storage/core.entity_registry"
    yaml_path = f"{CONFIG}/configuration.yaml"
    yaml = qnap(f"cat {yaml_path}")
    if YAML_BLOCK not in yaml:
        sys.exit("YAML sensor block not found verbatim - already migrated or edited? Nothing changed.")
    name = container()
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    if args.dry_run:
        print(f"dry run: would stop {name}, re-key {list(ENTITIES)}, remove the YAML block, start {name}")
        return
    qnap(f"cp -p {reg_path} {reg_path}.bak-v12-{stamp} && cp -p {yaml_path} {yaml_path}.bak-v12-{stamp}")
    print(f"backups: *.bak-v12-{stamp}")
    print(f"stopping {name} ...")
    qnap(f"{DOCKER} stop {name}")
    try:
        reg = json.loads(qnap(f"cat {reg_path}"))   # read only after the stop: HA writes it on shutdown
        done = 0
        for e in reg["data"]["entities"]:
            if e.get("platform") == "mqtt" and e.get("unique_id") in ENTITIES:
                new_uid, old_id, _ = ENTITIES[e["unique_id"]]
                if e["entity_id"] != old_id:
                    raise RuntimeError(f"unexpected entity_id {e['entity_id']} for {e['unique_id']}")
                e["unique_id"] = new_uid
                done += 1
        if done != len(ENTITIES):
            raise RuntimeError(f"re-keyed {done} of {len(ENTITIES)} entities")
        clash = [e for e in reg["data"].get("deleted_entities", [])
                 if e.get("platform") == "mqtt" and e.get("unique_id") in {v[0] for v in ENTITIES.values()}]
        if clash:
            raise RuntimeError(f"deleted_entities already hold a new unique_id: {clash}")
        qnap(f"cat > {reg_path}.new && mv {reg_path}.new {reg_path}", stdin=json.dumps(reg, indent=2, ensure_ascii=False))
        qnap(f"cat > {yaml_path}.new && mv {yaml_path}.new {yaml_path}", stdin=yaml.replace(YAML_BLOCK, ""))
        print(f"re-keyed {done} registry entries, removed the YAML sensors")
    finally:
        qnap(f"{DOCKER} start {name}")
        print(f"started {name}; waiting for the API ...")
    ctx = ssl.create_default_context()
    import urllib.request
    for _ in range(60):
        try:
            urllib.request.urlopen(f"{HA_URL}/api/", timeout=5, context=ctx)
        except Exception as e:  # 401 = up
            if getattr(e, "code", None) == 401:
                print("HA is up")
                return
        time.sleep(5)
    sys.exit("HA did not come back within 5 min - check `docker logs`")


# ---------------------------------------------------------------- WebSocket

class HA:
    def __init__(self):
        import websocket
        token = open(TOKEN_FILE).read().strip()
        self.ws = websocket.create_connection(HA_URL.replace("https", "wss").replace("http", "ws") + "/api/websocket",
                                              timeout=60)
        assert json.loads(self.ws.recv())["type"] == "auth_required"
        self.ws.send(json.dumps({"type": "auth", "access_token": token}))
        if json.loads(self.ws.recv())["type"] != "auth_ok":
            sys.exit("HA auth failed")
        self.id = 0

    def call(self, msg_type, **kw):
        self.id += 1
        self.ws.send(json.dumps(dict(kw, id=self.id, type=msg_type)))
        while True:
            r = json.loads(self.ws.recv())
            if r.get("id") == self.id:
                if not r.get("success"):
                    raise RuntimeError(f"{msg_type}: {r.get('error')}")
                return r.get("result")


def cmd_rename(args):
    ha = HA()
    for _, (uid, old, new) in ENTITIES.items():
        e = ha.call("config/entity_registry/get", entity_id=old)
        if e["unique_id"] != uid or not e.get("device_id"):
            sys.exit(f"{old}: unique_id={e['unique_id']} device_id={e.get('device_id')} - "
                     f"not yet adopted by discovery (flash v12 first)")
        ha.call("config/entity_registry/update", entity_id=old, new_entity_id=new)
        print(f"{old} -> {new}")
    prefs = ha.call("energy/get_prefs")
    rename = {old: new for _, old, new in ENTITIES.values()}
    text = json.dumps(prefs)
    for old, new in rename.items():
        text = text.replace(f'"{old}"', f'"{new}"')
    if text != json.dumps(prefs):
        new_prefs = json.loads(text)
        ha.call("energy/save_prefs", **{k: new_prefs[k] for k in
                ("energy_sources", "device_consumption", "device_consumption_water") if k in new_prefs})
        print("energy dashboard updated")


def cmd_stats(args):
    ha = HA()
    end = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
    start = end - timedelta(days=args.days)
    res = ha.call("recorder/statistics_during_period", statistic_ids=args.entities, period="hour",
                  start_time=start.isoformat(), end_time=end.isoformat(), types=["state", "sum"])
    json.dump(res, open(args.out, "w"), indent=1)
    for k, v in res.items():
        print(f"{k}: {len(v)} hours, last state={v[-1]['state'] if v else None} sum={v[-1]['sum'] if v else None}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("rekey")
    p.add_argument("--dry-run", action="store_true")
    sub.add_parser("rename")
    p = sub.add_parser("stats")
    p.add_argument("out")
    p.add_argument("entities", nargs="+")
    p.add_argument("--days", type=int, default=400)
    args = ap.parse_args()
    {"rekey": cmd_rekey, "rename": cmd_rename, "stats": cmd_stats}[args.cmd](args)


if __name__ == "__main__":
    main()
