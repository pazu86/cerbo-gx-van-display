"""Generates nodered/display-feed.json - a standalone Node-RED tab.

Every 10 s it reads the travel logger's state.json and trips.json (written by
the "Travel Log" tab, which is left untouched) and publishes a compact
summary, retained, to VanDisplay/travel on the Cerbo's local MQTT broker for the
display.

    python nodered/build_feed_flow.py
"""
import json
import pathlib

# Where your travel logger writes state.json / trips.json (an example - adapt to your own flow)
DIR = "/data/home/nodered/.node-red/travel_log"
# Node id of an existing "mqtt-broker" config node pointing at localhost:1883 on the Cerbo
# (open it in Node-RED, the id is in the info panel)
BROKER = "REPLACE_WITH_YOUR_BROKER_NODE_ID"
# Your UTC offset, for the next-service date (e.g. "+10:00" Brisbane, "+08:00" Perth, "+00:00" UK)
UTC_OFFSET = "+10:00"
TAB = "display_feed"

FUNC = r"""
// Compact travel summary for the display (keep it < 2 kB).
let s = {}, t = [];
try { s = JSON.parse(msg.state || '{}'); } catch (e) {}
try { t = JSON.parse(msg.payload || '[]'); } catch (e) {}
if (!Array.isArray(t)) t = [];

const r1 = n => +Number(n || 0).toFixed(1);
const fuel = Number(s.fuel_l_per_100km || 16);
const step = Number(s.service_step_km || 5000);
const since = Number(s.since_service_km || 0);
const kmLeft = Math.max(0, step - since);
const nd = new Date(`${s.next_service_date || ''}T00:00:00__UTC_OFFSET__`);
const days = Number.isFinite(nd.getTime()) ? Math.ceil((nd - new Date()) / 86400000) : null;
const trip = s.trip || {};
const name = x => x.name || (x.place_label ? `Trip to ${x.place_label}` : 'Trip');

const total = t.reduce((a, x) => a + Number(x.distance_km || 0), 0);
const monthly = {};
for (const x of t) {
    const d = new Date(x.end_ts || x.start_ts);
    if (!Number.isFinite(d.getTime())) continue;
    const k = d.toISOString().slice(0, 7);
    monthly[k] = (monthly[k] || 0) + Number(x.distance_km || 0);
}

const last = t[0];
msg.topic = 'VanDisplay/travel';
msg.retain = true;
msg.payload = JSON.stringify({
    v: 1,
    ts: Math.round(Date.now() / 1000),
    odo_km: r1(s.total_km),
    service: {
        km_left: r1(kmLeft), pct: Math.min(100, Math.round(since / step * 100)),
        days_left: days, next_date: s.next_service_date || null,
        due: kmLeft <= 0 || (days !== null && days <= 0),
        warn: kmLeft <= 500 || (days !== null && days <= 30),
    },
    at_home: !!s.at_home,
    home: s.home ? { lat: s.home.lat, lon: s.home.lon } : null,
    trip: {
        active: !!trip.active, km: r1(trip.distance_km), fuel_l: r1((trip.distance_km || 0) * fuel / 100),
        start: trip.start_ts || null, max_kmh: r1((trip.max_speed_mps || 0) * 3.6),
        max_from_home_km: r1(trip.max_dist_home_km),
    },
    last: last ? {
        name: name(last), km: r1(last.distance_km), nights: Number(last.nights_away || 0),
        dur: last.duration_text || '', end: (last.end_ts || '').slice(0, 10),
        fuel_l: r1(last.fuel_litres || (last.distance_km || 0) * fuel / 100),
    } : null,
    recent: t.slice(0, 6).map(x => ({ name: name(x), km: r1(x.distance_km), nights: Number(x.nights_away || 0),
                                      end: (x.end_ts || '').slice(0, 10) })),
    stats: { trips: t.length, nights: t.reduce((a, x) => a + Number(x.nights_away || 0), 0), km: r1(total),
             longest_km: r1(Math.max(0, ...t.map(x => Number(x.distance_km || 0)))) },
    monthly: Object.keys(monthly).sort().reverse().slice(0, 6).map(k => ({ m: k, km: r1(monthly[k]) })),
});
return msg;
""".strip()


def node(id, type, x, y, wires=None, **kw):
    n = {"id": f"{TAB}_{id}", "type": type, "z": TAB, "name": kw.pop("name", ""), "x": x, "y": y}
    if wires is not None:
        n["wires"] = [[f"{TAB}_{w}" for w in wires]]
    n.update(kw)
    return n


nodes = [
    node("inject", "inject", 140, 80, ["state"], name="Every 10 s", props=[], repeat="10",
         crontab="", once=True, onceDelay="5", topic=""),
    node("state", "file in", 340, 80, ["keep"], name="state.json", filename=f"{DIR}/state.json",
         filenameType="str", format="utf8", chunk=False, sendError=False, encoding="none", allProps=False),
    node("keep", "change", 520, 80, ["trips"], name="msg.state = payload",
         rules=[{"t": "move", "p": "payload", "pt": "msg", "to": "state", "tot": "msg"}]),
    node("trips", "file in", 700, 80, ["build"], name="trips.json", filename=f"{DIR}/trips.json",
         filenameType="str", format="utf8", chunk=False, sendError=False, encoding="none", allProps=False),
    node("build", "function", 880, 80, ["out"], name="Build display summary", func=FUNC.replace("__UTC_OFFSET__", UTC_OFFSET), outputs=1,
         timeout=0, noerr=0, initialize="", finalize="", libs=[]),
    node("out", "mqtt out", 1080, 80, [], name="VanDisplay/travel", topic="", qos="0", retain="",
         respTopic="", contentType="", userProps="", correl="", expiry="", broker=BROKER),
]
for n in nodes:
    if n["type"] == "mqtt out":
        n["wires"] = []

flow = {
    "id": TAB,
    "label": "Van Display feed",
    "info": "Publishes a compact travel summary to MQTT VanDisplay/travel (retained) every 10 s for the "
            "ESP32 display. Reads the Travel Log's files; doesn't modify that tab. "
            "Source: Van Display repo, nodered/build_feed_flow.py",
    "nodes": nodes,
}

out = pathlib.Path(__file__).with_name("display-feed.json")
out.write_text(json.dumps(flow, indent=2))
print(f"wrote {out}")
