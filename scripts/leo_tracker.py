#!/usr/bin/env python3
"""Live LEO tracker for the OCUDU gNB and OAI UE logs.

    ./scripts/leo_tracker.py [gnb logs...] [--ue PATH] [--port 8500] [--host 0.0.0.0]
"""
import collections, http.server, json, math, os, re, socketserver, sys, threading, time
from datetime import datetime, timedelta, timezone

PAIR = ["/tmp/gnb_leo_sat1.log", "/tmp/gnb_leo_sat2.log"]
SWITCH = ["/tmp/gnb_leo_satswitch.log"]
TRAIN = ["/tmp/gnb_leo_sattrain.log"]
SOLO = ["/tmp/gnb_leo.log"]
UE_CANDIDATES = ["/tmp/ue_leo_sattrain.log", "/tmp/ue_leo_satswitch.log", "/tmp/ue_leo_ho.log", "/tmp/ue_leo.log"]
LOGS, UE_LOG = [], None
PORT = 8500
HOST = "0.0.0.0"
B, R = 6356752.314, 6956752.314
C, F_DL = 299792458.0, 2185e6
START_TAIL = 3_000_000
TRACK_KEEP = 7200
TRACK_OUT = 600
EVENTS_KEEP = 400

SAMPLE = re.compile(r"^(\S+) .*Emulated NTN channel updated.*?rx_delay=(\d+)us drift=([-0-9.]+)(?:.*? sat=(\d+))?")
TARGET = re.compile(r"^(\S+) .*Sat-switch target geometry.*?rx_delay=(\d+)us drift=([-0-9.]+).*? sat=(\d+)")
GNB_LINE = re.compile(r"^(\d{4}-\d\d-\d\dT[0-9:.]+) \[([A-Za-z0-9_-]+)\s*\] \[([IWED])\] (?:\[\s*[0-9.]+\] )?(.*)$")
ANSI = re.compile(r"\x1b\[([0-9;]*)m")

GNB_EVENTS = [
    ("switch", "info",
     re.compile(r"Sat-train armed, cell=\S+ satellite (\d+) -> (\d+) at ([0-9:]+)\S* \(in (\d+) s\): \d+ sets "
                r"through ([-0-9.]+) deg, \d+ will be at ([-0-9.]+) deg"),
     lambda m: (f"Switch armed: SAT{m[1]} → SAT{m[2]}",
                f"at {m[3]} (in {m[4]} s) as SAT{m[1]} sets through {m[5]}°; SAT{m[2]} will be at {float(m[6]):.1f}°")),
    ("switch", "info", re.compile(r"Sat-switch promotion scheduled, cell=\S+ serving satellite (\d+) -> (\d+) at "
                                  r"t_service=([0-9:]+)"),
     lambda m: (f"Switch scheduled: SAT{m[1]} → SAT{m[2]}", f"at {m[3]}")),
    ("switch", "ok", re.compile(r"Sat-switch promotion applied, cell=\S+ serving satellite (\d+) -> (\d+)"),
     lambda m: (f"Satellite switched: SAT{m[1]} → SAT{m[2]}", "same cell, same UE context - no handover")),
    ("switch", "err", re.compile(r"Sat-train cell=\S+: satellite (\d+) is below the horizon at the switch"),
     lambda m: ("Coverage gap ahead", f"SAT{m[1]} will be below the horizon at the switch")),
    ("link", "ok", re.compile(r"satellite rose above the horizon.*?elevation ([-0-9.]+) deg"),
     lambda m: ("Satellite rose above the horizon", f"elevation {m[1]}° - emulated link up")),
    ("link", "warn", re.compile(r"satellite set below the horizon.*?elevation ([-0-9.]+) deg"),
     lambda m: ("Satellite set below the horizon", f"elevation {m[1]}° - emulated link down")),
    ("ho", "info", re.compile(r"Tx PDU: HandoverRequest$"), lambda m: ("Handover started", "HandoverRequest sent over Xn")),
    ("ho", "info", re.compile(r"HandoverRequest - extracted target cell.*cell_id=(\S+)"),
     lambda m: ("Handover requested", f"target cell {m[1]}")),
    ("ho", "info", re.compile(r"Rx PDU: HandoverRequestAcknowledge"),
     lambda m: ("Handover prepared", "target acknowledged, UE commanded")),
    ("ho", "ok", re.compile(r'"Path Switch Procedure" finished successfully'),
     lambda m: ("Path switch complete", "core now routes to the target")),
    ("ho", "err", re.compile(r'"Path Switch Procedure" (?:timed out|failed)|Path Switch Request rejected'),
     lambda m: ("Path switch failed", "")),
    ("ho", "err", re.compile(r"Did not receive RRC Reconfiguration Complete after HO"),
     lambda m: ("Handover failed", "UE never reached the target (t304)")),
    ("ue", "ok", re.compile(r"DCCH UL rrcSetupComplete"), lambda m: ("UE connected", "RRC setup complete")),
    ("ue", "ok", re.compile(r"Tx PDU .*InitialContextSetupResponse"), lambda m: ("UE registered", "initial context set up")),
    ("ue", "ok", re.compile(r"PDUSessionResourceSetupResponse|PDU Session Resource Setup.*finished successfully"),
     lambda m: ("PDU session up", "")),
    ("ue", "warn", re.compile(r"Tx PDU: UEContextReleaseRequest"), lambda m: ("gNB asked the core to release the UE", "")),
    ("ue", "warn", re.compile(r"DCCH DL rrcRelease"), lambda m: ("UE released", "RRC release sent")),
]
HO_RX = re.compile(r"Rx PDU: HandoverRequest$")
RRC_RECONF_DONE = re.compile(r"DCCH UL rrcReconfigurationComplete")
GNB_ERROR_SKIP = re.compile(r"Decoding failure|Buffer size limit \d+ was reached")
GNB_EVENT_COMPS = {"NGAP", "RRC", "XNAP", "NTN", "CU-CP", "CU-CP-F1", "CU-CP-E1", "DU-F1", "CU-UP", "GNB", "RF",
                   "APP", "DU", "DU-MNG"}

UE_EVENTS = [
    ("switch", "info", re.compile(r"satSwitchWithReSync-r18 armed: switching satellite in (\d+) ms"),
     lambda m: ("UE armed its satellite switch", f"fires in {int(m[1]) / 1000:.0f} s")),
    ("switch", "ok", re.compile(r"t-ServiceStart reached"),
     lambda m: ("UE switched satellite", "in-cell: no cell search, no random access")),
    ("ue", "ok", re.compile(r"4-Step RA procedure succeeded"), lambda m: ("UE random access OK", "")),
    ("ue", "ok", re.compile(r"Received Registration Accept"), lambda m: ("UE registration accepted", "")),
    ("ue", "ok", re.compile(r"TUN Interface oaitun_ue1 successfully configured, IPv4 ([0-9.]+)"),
     lambda m: ("UE got an IP", m[1])),
    ("ue", "warn", re.compile(r"RRC moved into IDLE"), lambda m: ("UE went idle", "")),
    ("ue", "warn", re.compile(r"Received RRC Release"), lambda m: ("UE released by the gNB", "")),
    ("ue", "err", re.compile(r"Registration Reject cause: (\S+)"), lambda m: ("UE registration rejected", m[1])),
    ("ho", "info", re.compile(r"Starting re-sync detection for target Nid_cell (\d+)"),
     lambda m: ("UE re-syncing to handover target", f"PCI {m[1]}")),
    ("ue", "err", re.compile(r"Assertion .* failed|Exiting OAI softmodem"), lambda m: ("UE crashed", "")),
]
UE_SYNC_FAIL = re.compile(r"synch Failed|pbch not decoded")


def geometry(rx_delay_us, drift):
    rng = (rx_delay_us * 1e-6 / 2) * C
    clamp = lambda v: max(-1.0, min(1.0, v))
    elev = math.degrees(math.asin(clamp((R * R - B * B - rng * rng) / (2 * B * rng))))
    theta = math.degrees(math.acos(clamp((R * R + B * B - rng * rng) / (2 * R * B))))
    return {"km": round(rng / 1000, 1), "el": round(elev, 2), "th": round(theta if drift > 0 else -theta, 3),
            "ms": round(rx_delay_us / 1000, 2), "drift": round(drift, 1),
            "dop": round(-(drift * 1e-6 / 2) * F_DL / 1e3, 1)}


def label_for(path):
    stem = os.path.basename(path).rsplit(".", 1)[0]
    for prefix in ("gnb_leo_", "gnb_", "leo_", "ue_leo_", "ue_"):
        if stem.startswith(prefix):
            stem = stem[len(prefix):]
    return (stem or "leo").upper()


def log_age(path):
    try:
        return time.time() - os.path.getmtime(path)
    except OSError:
        return None


def parse_ts(s):
    try:
        return datetime.fromisoformat(s)
    except ValueError:
        return None


def collapse(events, title, detail, ts):
    if events and events[-1]["title"] == title and events[-1]["detail"] == detail and \
            (ts - events[-1]["ts"]).total_seconds() < 120:
        events[-1]["n"] += 1
        return True
    return False


class Follower:

    def __init__(self, path):
        self.path, self.ino, self.pos, self.partial = path, None, 0, b""

    def poll(self):
        try:
            st = os.stat(self.path)
        except OSError:
            return [], False, False
        restarted = st.st_ino != self.ino or st.st_size < self.pos
        if restarted:
            self.ino, self.partial = st.st_ino, b""
            self.pos = max(0, st.st_size - START_TAIL)
        if st.st_size == self.pos:
            return [], restarted, True
        try:
            with open(self.path, "rb") as fh:
                fh.seek(self.pos)
                chunk = fh.read(st.st_size - self.pos)
        except OSError:
            return [], restarted, True
        base = self.pos - len(self.partial)
        data = self.partial + chunk
        self.pos += len(chunk)
        lines, start = [], 0
        if restarted and base > 0:
            nl = data.find(b"\n")
            start = nl + 1 if nl >= 0 else len(data)
        while True:
            nl = data.find(b"\n", start)
            if nl < 0:
                break
            lines.append((base + start, data[start:nl].decode("utf-8", "replace")))
            start = nl + 1
        self.partial = data[start:]
        return lines, restarted, True


class GnbLog:

    def __init__(self, path, pos):
        self.path, self.pos, self.follow = path, pos, Follower(path)
        self.reset()

    def reset(self):
        self.tracks = collections.OrderedDict()
        self.last_keep = {}
        self.events = []
        self.pending = None
        self.switch_el, self.last_ts, self.ho_rx_at, self.exists = None, None, None, False

    def poll(self):
        lines, restarted, self.exists = self.follow.poll()
        if restarted:
            self.reset()
            self.exists = True
        for off, line in lines:
            self._line(off, line)

    def _sample(self, m, serving):
        ts = parse_ts(m.group(1))
        if ts is None:
            return
        idx = int(m.group(4) or 0)
        self.last_ts = max(self.last_ts or ts, ts)
        prev = self.last_keep.get(idx)
        if prev is not None and (ts - prev).total_seconds() < 1.0:
            if serving and self.tracks[idx] and not self.tracks[idx][-1]["serving"]:
                self.tracks[idx][-1]["serving"] = True
            return
        pt = geometry(int(m.group(2)), float(m.group(3)))
        pt.update(clock=m.group(1)[11:19], ts=ts, serving=serving)
        self.tracks.setdefault(idx, collections.deque(maxlen=TRACK_KEEP)).append(pt)
        self.last_keep[idx] = ts

    def _line(self, off, line):
        m = SAMPLE.match(line)
        if m:
            return self._sample(m, True)
        m = TARGET.match(line)
        if m:
            return self._sample(m, False)
        g = GNB_LINE.match(line)
        if not g:
            return
        stamp, comp, lvl, msg = g.groups()
        ts = parse_ts(stamp)
        if ts is None:
            return
        self.last_ts = max(self.last_ts or ts, ts)
        if lvl not in "WE" and comp not in GNB_EVENT_COMPS:
            return
        if HO_RX.search(msg):
            self.ho_rx_at = ts
        if RRC_RECONF_DONE.search(msg) and self.ho_rx_at and (ts - self.ho_rx_at).total_seconds() < 30:
            self._event(off, ts, "ho", "ok", "Handover complete", "UE reached the target (RRC reconfiguration complete)")
            self.ho_rx_at = None
            return
        for kind, sev, rx, build in GNB_EVENTS:
            mm = rx.search(msg)
            if not mm:
                continue
            title, detail = build(mm)
            self._event(off, ts, kind, sev, title, detail)
            if title.startswith(("Switch armed", "Switch scheduled")):
                at = datetime.combine(ts.date(), datetime.strptime(mm[3][:8], "%H:%M:%S").time())
                if at < ts - timedelta(hours=12):
                    at += timedelta(days=1)
                self.pending = {"from": int(mm[1]), "to": int(mm[2]), "at": at,
                                "el": float(mm[6]) if title.startswith("Switch armed") else None}
                if title.startswith("Switch armed"):
                    self.switch_el = float(mm[5])
            elif title.startswith("Satellite switched"):
                self.pending = None
            break
        if lvl == "E" and not GNB_ERROR_SKIP.search(msg) and not any(e["off"] == off for e in self.events[-3:]):
            self._event(off, ts, "error", "err", f"gNB error ({comp})", msg[:160])

    def _event(self, off, ts, kind, sev, title, detail):
        if collapse(self.events, title, detail, ts):
            return
        self.events.append({"id": f"{self.pos}:{self.follow.ino}:{off}", "off": off, "ts": ts, "kind": kind,
                            "sev": sev, "title": title, "detail": detail, "src": "gNB", "n": 1})
        del self.events[:-EVENTS_KEEP]


class UeLog:

    def __init__(self, path):
        self.path, self.follow = path, Follower(path)
        self.reset()

    def reset(self):
        self.events = []
        self.state, self.ip, self.sync_fails, self.switches, self.exists = "no UE log", None, 0, 0, False
        self.primed = False

    def poll(self):
        lines, restarted, self.exists = self.follow.poll()
        if restarted:
            self.reset()
            self.exists = True
        now = datetime.now(timezone.utc).replace(tzinfo=None)
        for off, raw in lines:
            self._line(off, raw, now)
        self.primed = True

    def _line(self, off, raw, now):
        line = ANSI.sub("", raw).strip()
        if not line or line[0] != "[":
            return
        if UE_SYNC_FAIL.search(line):
            self.sync_fails += 1
            if self.state in ("connected", "registered", "data"):
                self.state = "lost sync"
                self._event(off, now, "ue", "err", "UE lost sync", "")
        for kind, esev, rx, build in UE_EVENTS:
            mm = rx.search(line)
            if not mm:
                continue
            title, detail = build(mm)
            self._event(off, now, kind, esev, title, detail)
            if title == "UE random access OK":
                self.state, self.sync_fails = "connected", 0
            elif title == "UE registration accepted":
                self.state = "registered"
            elif title == "UE got an IP":
                self.state, self.ip = "data", mm[1]
            elif title in ("UE went idle", "UE released by the gNB"):
                self.state = "idle"
            elif title == "UE registration rejected":
                self.state = "rejected"
            elif title == "UE crashed":
                self.state = "crashed"
            elif title == "UE switched satellite":
                self.switches += 1
            break

    def _event(self, off, now, kind, sev, title, detail):
        if collapse(self.events, title, detail, now):
            return
        self.events.append({"id": f"ue:{self.follow.ino}:{off}", "ts": now, "kind": kind, "sev": sev,
                            "title": title, "detail": detail, "src": "UE", "approx": True, "hist": not self.primed,
                            "n": 1})
        del self.events[:-EVENTS_KEEP]

    def alive(self):
        age = log_age(self.path)
        return age is not None and age < 6


GNBS, UE = [], None
LOCK = threading.Lock()


def build_data():
    with LOCK:
        return _build_data()


def _build_data():
    for g in GNBS:
        g.poll()
    if UE:
        UE.poll()

    sats, starts, now = [], [], None
    for g in GNBS:
        if g.last_ts:
            now = max(now or g.last_ts, g.last_ts)
        for trk in g.tracks.values():
            if trk:
                starts.append(trk[0]["ts"])
    if not starts:
        return {"error": "no NTN channel updates in any gNB log yet - is a gNB running?",
                "logs": [g.path for g in GNBS], "ue": ue_view(), "events": events_view(None, None)}
    t0 = min(starts)
    rel = lambda ts: round((ts - t0).total_seconds(), 1)

    for g in GNBS:
        multi = len(g.tracks) > 1
        newest = max((t[-1]["ts"] for t in g.tracks.values() if t), default=None)
        for idx, trk in g.tracks.items():
            if not trk:
                continue
            last = trk[-1]
            role = ("released" if (newest - last["ts"]).total_seconds() > 2
                    else "serving" if last["serving"] else "target")
            step = max(1, len(trk) // TRACK_OUT)
            pts = [{k: p[k] for k in ("th", "el")} | {"t": rel(p["ts"]), "s": p["serving"]}
                   for p in list(trk)[::step]]
            now_pt = {k: v for k, v in last.items() if k != "ts"}
            sats.append({"label": f"{label_for(g.path)} SAT{idx}" if multi else label_for(g.path),
                         "sat": idx, "color": idx if multi else g.pos, "role": role, "track": pts, "now": now_pt,
                         "since": rel(trk[0]["ts"]), "last": last["clock"]})

    pending, switch_el = None, None
    for g in GNBS:
        switch_el = g.switch_el or switch_el
        if g.pending:
            p = g.pending
            pending = {"from": p["from"], "to": p["to"], "at": p["at"].strftime("%H:%M:%S"),
                       "in": round((p["at"] - now).total_seconds()), "el": p["el"]}
    age = min((a for a in (log_age(g.path) for g in GNBS) if a is not None), default=None)
    stale = (f"{', '.join(g.path for g in GNBS)} not written for {age / 60:.0f} min - this is a finished run. "
             f"Is the gNB writing its log?") if age is not None and age > 10 else None
    return {"now": now.strftime("%H:%M:%S"), "t_now": rel(now), "sats": sats, "pending": pending,
            "switch_el": switch_el, "logs": [g.path for g in GNBS], "ue": ue_view(),
            "events": events_view(t0, now), "stale": stale}


def ue_view():
    if not UE:
        return {"path": None, "state": "no UE log"}
    return {"path": UE.path, "state": UE.state if UE.exists else "no UE log", "ip": UE.ip, "alive": UE.alive(),
            "sync_fails": UE.sync_fails, "switches": UE.switches}


def events_view(t0, now):
    evs = [e for g in GNBS for e in g.events] + (UE.events if UE else [])
    evs.sort(key=lambda e: e["ts"])
    out = []
    for e in evs[-EVENTS_KEEP:]:
        o = {k: e[k] for k in ("id", "kind", "sev", "title", "detail", "src", "n")}
        o["clock"] = "earlier" if e.get("hist") else ("~" if e.get("approx") else "") + e["ts"].strftime("%H:%M:%S")
        o["hist"] = e.get("hist", False)
        o["approx"] = e.get("approx", False)
        o["t"] = round((e["ts"] - t0).total_seconds(), 1) if t0 and not e.get("hist") else None
        out.append(o)
    return out


def local_addresses():
    found = ["127.0.0.1"]
    try:
        import socket
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            if info[4][0] not in found:
                found.append(info[4][0])
    except OSError:
        pass
    return found


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/data"):
            body, ctype = json.dumps(build_data()).encode(), "application/json"
        else:
            body, ctype = PAGE.encode(), "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


PAGE = r"""<!doctype html><meta charset="utf-8"><title>LEO Tracker</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{color-scheme:light;--bg:#f2f2f0;--card:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--mute:#898781;--rule:#d6d5cc;
 --sky:#e8eef6;--earth:#cfd8e3;--ok:#1f8a4c;--info:#2a78d6;--warn:#b86e00;--err:#d23b3b;--hl:#fff6d6;
 --s0:#2a78d6;--s1:#c1761b;--s2:#1f9e89;--s3:#c2417a;--s4:#6a55c7;--s5:#5b8c2a;--s6:#d0492f;--s7:#2e8fb3;
 --s8:#9c6b3c;--s9:#8a8a1e;--s10:#b0439b;--s11:#3d6e99}
@media(prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#111110;--card:#1a1a19;--ink:#f4f4f2;--ink2:#c3c2b7;
 --mute:#898781;--rule:#34342f;--sky:#15181d;--earth:#2c3440;--ok:#48b878;--info:#5a9ff0;--warn:#e0a33c;--err:#ef6b6b;
 --hl:#2e2a1a;--s0:#5a9ff0;--s1:#e0a33c;--s2:#3cc4ad;--s3:#e56aa0;--s4:#9a86f0;--s5:#8cbf52;--s6:#f07a5c;
 --s7:#5bbde0;--s8:#c99a6a;--s9:#c7c74a;--s10:#dc6fc8;--s11:#78a6d4}}
*{box-sizing:border-box}
body{background:var(--bg);color:var(--ink);margin:0;font:14px/1.5 ui-sans-serif,system-ui,sans-serif}
.wrap{max-width:1400px;margin:0 auto;padding:22px 18px 48px;display:flex;flex-direction:column;gap:14px}
header{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;flex-wrap:wrap}
h1{margin:0;font-size:22px;font-weight:600;letter-spacing:-.02em}
h2{margin:0;font:11px ui-monospace,monospace;letter-spacing:.13em;text-transform:uppercase;color:var(--mute);
 font-weight:500}
.eyebrow{font:11px ui-monospace,monospace;letter-spacing:.13em;text-transform:uppercase;color:var(--mute)}
.chips{display:flex;gap:8px;flex-wrap:wrap}
.chip{background:var(--card);border:1px solid var(--rule);padding:6px 10px;font:12px ui-monospace,monospace;
 display:flex;gap:7px;align-items:center;white-space:nowrap}
.chip b{font-weight:600}.chip span{color:var(--mute)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--ok);flex:none}.dot.stale{background:var(--mute)}
.card{background:var(--card);border:1px solid var(--rule)}
.card>h2{padding:9px 14px;border-bottom:1px solid var(--rule);display:flex;gap:10px;align-items:center}
.card>h2 .r{margin-left:auto;text-transform:none;letter-spacing:0}
svg{display:block;width:100%;height:auto}
.scroll{overflow-x:auto}.scroll svg{min-width:760px}
.grid{display:grid;gap:14px}
.g31{grid-template-columns:minmax(260px,1fr) minmax(0,2.2fr)}
@media(max-width:900px){.g31{grid-template-columns:minmax(0,1fr)}}
.next{padding:16px 16px 18px;display:flex;flex-direction:column;gap:8px}
.next .big{font-size:40px;font-weight:650;font-variant-numeric:tabular-nums;letter-spacing:-.02em;line-height:1.05}
.next .route{font:14px ui-monospace,monospace}.next .sub{color:var(--ink2);font-size:13px}
.bar{height:6px;background:var(--rule);position:relative;overflow:hidden}
.bar i{position:absolute;inset:0 auto 0 0;background:var(--info);transition:width .9s linear}
.sats{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:1px;background:var(--rule)}
.sat{background:var(--card);padding:11px 13px}
.sat .h{display:flex;align-items:center;gap:8px;font:12px ui-monospace,monospace;letter-spacing:.06em}
.sat .h i{width:10px;height:10px;border-radius:50%;flex:none}
.role{font:10px ui-monospace,monospace;letter-spacing:.1em;padding:1px 6px;border:1px solid currentColor}
.role.serving{color:var(--ok)}.role.target{color:var(--info)}.role.released{color:var(--mute)}
.kv{display:grid;grid-template-columns:repeat(3,1fr);gap:4px 10px;margin-top:8px}
.kv div{font-size:17px;font-weight:600;font-variant-numeric:tabular-nums}
.kv dt{font:10px ui-monospace,monospace;letter-spacing:.08em;text-transform:uppercase;color:var(--mute)}
.kv span{font-size:11px;font-weight:400;color:var(--ink2)}
.gone{padding:8px 13px;font:12px ui-monospace,monospace;color:var(--mute);background:var(--card)}
.events{max-height:420px;overflow:auto}
.ev{display:grid;grid-template-columns:86px 64px minmax(0,1fr);gap:10px;padding:7px 14px;border-bottom:1px solid var(--rule);
 align-items:baseline}
.ev:last-child{border-bottom:0}.ev.new{background:var(--hl)}
.ev .c{font:12px ui-monospace,monospace;color:var(--mute);font-variant-numeric:tabular-nums}
.tag{font:10px ui-monospace,monospace;letter-spacing:.08em;text-transform:uppercase;padding:1px 5px;text-align:center;
 border:1px solid currentColor;justify-self:start}
.k-switch{color:var(--s4)}.k-ho{color:var(--s1)}.k-ue{color:var(--info)}.k-link{color:var(--s2)}.k-error{color:var(--err)}
.ev b{font-weight:600}.ev .d{color:var(--ink2);font-size:13px}
.ev.sev-err b{color:var(--err)}.ev.sev-warn b{color:var(--warn)}.ev.sev-ok b{color:var(--ok)}
.empty{padding:14px;color:var(--mute);font:12px ui-monospace,monospace}
.path{font:11px ui-monospace,monospace;color:var(--mute);text-transform:none;letter-spacing:0}
.err{background:var(--card);border:1px solid var(--rule);padding:14px;color:var(--err);font:13px ui-monospace,monospace}
#toasts{position:fixed;top:14px;right:14px;display:flex;flex-direction:column;gap:8px;z-index:9;width:min(380px,92vw)}
.toast{background:var(--card);border:1px solid var(--rule);border-left:4px solid var(--info);padding:10px 12px;
 box-shadow:0 6px 24px rgba(0,0,0,.14);animation:in .25s ease-out}
.toast b{display:block;font-size:14px}.toast span{color:var(--ink2);font-size:12.5px}
.toast .m{font:10px ui-monospace,monospace;letter-spacing:.1em;text-transform:uppercase;color:var(--mute)}
.toast.sev-ok{border-left-color:var(--ok)}.toast.sev-warn{border-left-color:var(--warn)}.toast.sev-err{border-left-color:var(--err)}
@keyframes in{from{opacity:0;transform:translateY(-6px)}to{opacity:1;transform:none}}
button{font:11px ui-monospace,monospace;background:transparent;color:var(--ink2);border:1px solid var(--rule);
 padding:2px 8px;cursor:pointer}button[aria-pressed=true]{color:var(--ink);border-color:var(--ink2)}
</style>
<div id="toasts" aria-live="polite"></div>
<div class="wrap">
<header>
 <div><div class="eyebrow">OCUDU gNB &middot; rfsimulator &middot; live from the gNB and UE logs</div>
 <h1>LEO Tracker</h1></div>
 <div class="chips">
  <div class="chip"><span class="dot stale" id="live"></span><b id="clock">--:--:--</b><span>gNB UTC</span></div>
  <div class="chip"><span>serving</span><b id="serving">&mdash;</b></div>
  <div class="chip"><span>UE</span><b id="uestate">&mdash;</b><span id="ueip"></span></div>
  <div class="chip"><span>switches</span><b id="nswitch">0</b></div>
 </div>
</header>
<div id="err" class="err" hidden></div>

<div class="card">
 <h2>Sky &middot; side view along the orbit, to scale<span class="r path" id="skykey"></span></h2>
 <div class="scroll"><svg id="sky" viewBox="0 0 1300 280" role="img" aria-label="Side view of the satellite passes">
  <defs><clipPath id="cp"><rect width="1300" height="280"/></clipPath></defs>
  <g clip-path="url(#cp)">
   <rect width="1300" height="280" fill="var(--sky)"/>
   <path id="earth" fill="var(--earth)"/>
   <line id="hz" stroke="var(--rule)" stroke-width="1.5" stroke-dasharray="6 5"/>
   <g id="mask"></g><g id="layer"></g>
   <circle id="gs" r="5" fill="var(--ink)"/>
   <text id="gsl" font-size="11" fill="var(--ink2)" text-anchor="middle">GROUND STATION / UE</text>
   <text id="hzl" font-size="10" fill="var(--mute)" letter-spacing="1">HORIZON &middot; 0&deg;</text>
  </g></svg></div>
</div>

<div class="grid g31">
 <div class="card"><h2>Next satellite switch</h2>
  <div class="next" id="next"><div class="big" id="cd">&mdash;</div><div class="route" id="route">no switch pending</div>
   <div class="bar"><i id="cdbar" style="width:0"></i></div><div class="sub" id="nextsub"></div></div></div>
 <div class="card"><h2>Satellites<span class="r path" id="satsum"></span></h2>
  <div class="sats" id="sats"></div><div class="gone" id="gone" hidden></div></div>
</div>

<div class="card"><h2>Elevation over time<span class="r path">thick = serving &middot; dashed line = switch threshold</span></h2>
 <div class="scroll"><svg id="chart" viewBox="0 0 1300 250" role="img" aria-label="Elevation of each satellite over time"></svg></div></div>

<div class="card"><h2>Events<span class="r"><button id="mute" aria-pressed="false">notifications on</button></span></h2>
 <div class="events" id="events"><div class="empty">Nothing yet.</div></div></div>

</div>
<script>
const $=i=>document.getElementById(i), NS="http://www.w3.org/2000/svg";
const CX=650,CY=1620,RE=1400,RV=1532,GSY=CY-RE;
const COL=i=>`var(--s${((i%12)+12)%12})`;
const esc=s=>String(s??"").replace(/[&<>]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;"}[c]));
const mk=(t,a,p)=>{const e=document.createElementNS(NS,t);for(const k in a)e.setAttribute(k,a[k]);if(p)p.append(e);return e;};
const pos=th=>{const r=th*Math.PI/180;return [CX+RV*Math.sin(r),CY-RV*Math.cos(r)];};
const yEdge=CY-RE*Math.cos(Math.asin(CX/RE));
$("earth").setAttribute("d",`M0 ${yEdge} A ${RE} ${RE} 0 0 1 1300 ${yEdge} L1300 280 L0 280 Z`);
for(const [k,v] of Object.entries({x1:0,x2:1300,y1:GSY,y2:GSY}))$("hz").setAttribute(k,v);
$("gs").setAttribute("cx",CX);$("gs").setAttribute("cy",GSY);
$("gsl").setAttribute("x",CX);$("gsl").setAttribute("y",GSY+19);
$("hzl").setAttribute("x",14);$("hzl").setAttribute("y",GSY-8);

let muted=false; $("mute").onclick=()=>{muted=!muted;$("mute").setAttribute("aria-pressed",muted);
 $("mute").textContent=muted?"notifications off":"notifications on";};
let seen=null, lastMask=null, skyNodes={}, fresh=new Set();

function toast(e){
 if(muted) return;
 const d=document.createElement("div"); d.className=`toast sev-${e.sev}`;
 d.innerHTML=`<div class="m">${esc(e.src)} &middot; ${esc(e.kind)} &middot; ${esc(e.clock)}</div><b>${esc(e.title)}</b>`+
  (e.detail?`<span>${esc(e.detail)}</span>`:"");
 $("toasts").prepend(d); while($("toasts").children.length>5)$("toasts").lastChild.remove();
 setTimeout(()=>d.remove(), e.sev==="err"?12000:7000);
}

function drawMask(el){
 if(el===lastMask) return; lastMask=el; const g=$("mask"); g.innerHTML="";
 if(el==null) return;
 const r=el*Math.PI/180, L=760;
 for(const s of [-1,1]){
  mk("line",{x1:CX,y1:GSY,x2:CX+s*L*Math.cos(r),y2:GSY-L*Math.sin(r),stroke:"var(--err)","stroke-width":1,
   "stroke-dasharray":"4 5",opacity:.6},g);
 }
 const t=mk("text",{x:CX+L*Math.cos(r)-4,y:GSY-L*Math.sin(r)-6,"font-size":10,fill:"var(--err)","text-anchor":"end",
  "letter-spacing":1},g); t.textContent=`SWITCH ${el}°`;
}

function drawSky(sats){
 const keys=new Set(sats.map(s=>s.label));
 for(const k of Object.keys(skyNodes)) if(!keys.has(k)){Object.values(skyNodes[k]).forEach(n=>n.remove());delete skyNodes[k];}
 for(const s of sats){
  let N=skyNodes[s.label];
  if(!N){ const L=$("layer"), c=COL(s.color);
   N=skyNodes[s.label]={track:mk("polyline",{fill:"none",stroke:c,"stroke-width":1.4,"stroke-dasharray":"3 4"},L),
    beam:mk("line",{stroke:c,"stroke-width":2},L),dot:mk("circle",{fill:c},L),
    name:mk("text",{"font-size":10,fill:c,"text-anchor":"middle","letter-spacing":1},L)};
   N.name.textContent=`SAT${s.sat}`;
  }
  const n=s.now, up=n.el>0, gone=s.role==="released", [x,y]=pos(n.th);
  N.track.setAttribute("points",s.track.map(p=>pos(p.th).join(",")).join(" "));
  N.track.setAttribute("opacity",gone?.25:.55);
  N.dot.setAttribute("cx",x);N.dot.setAttribute("cy",y);N.dot.setAttribute("r",s.role==="serving"?8:6);
  N.dot.setAttribute("opacity",gone?.3:up?1:.4);
  N.name.setAttribute("x",x);N.name.setAttribute("y",y-13);N.name.setAttribute("opacity",gone?.35:1);
  N.beam.setAttribute("x1",CX);N.beam.setAttribute("y1",GSY);N.beam.setAttribute("x2",x);N.beam.setAttribute("y2",y);
  N.beam.setAttribute("stroke-dasharray",s.role==="target"?"5 5":"");
  N.beam.setAttribute("opacity",!gone&&up?(s.role==="serving"?.95:.5):0);
 }
}

function drawChart(d){
 const svg=$("chart"); svg.innerHTML="";
 const W=1300,H=250,L=44,Rm=14,T=16,Bm=26, lo=-5, hi=90;
 const tEnd=d.t_now, tStart=Math.max(0,tEnd-1500);
 const X=t=>L+(W-L-Rm)*(t-tStart)/Math.max(1,tEnd-tStart), Y=e=>T+(H-T-Bm)*(1-(Math.max(lo,Math.min(hi,e))-lo)/(hi-lo));
 for(const e of [0,30,60,90]){
  mk("line",{x1:L,x2:W-Rm,y1:Y(e),y2:Y(e),stroke:"var(--rule)","stroke-width":1,"stroke-dasharray":e?"":"6 4"},svg);
  const t=mk("text",{x:L-8,y:Y(e)+4,"font-size":11,fill:"var(--mute)","text-anchor":"end"},svg); t.textContent=e+"°";
 }
 for(let k=0;k<=5;k++){const tt=tStart+(tEnd-tStart)*k/5;
  const t=mk("text",{x:X(tt),y:H-8,"font-size":11,fill:"var(--mute)","text-anchor":"middle"},svg);
  t.textContent=`-${Math.round((tEnd-tt)/60)} min`;}
 if(d.switch_el!=null){
  mk("line",{x1:L,x2:W-Rm,y1:Y(d.switch_el),y2:Y(d.switch_el),stroke:"var(--err)","stroke-width":1,"stroke-dasharray":"4 5",opacity:.7},svg);
  const t=mk("text",{x:W-Rm-4,y:Y(d.switch_el)-5,"font-size":10,fill:"var(--err)","text-anchor":"end"},svg);
  t.textContent=`switch ${d.switch_el}°`;
 }
 for(const e of d.events){
  if(e.t==null||e.t<tStart||e.approx) continue;
  const strong=/switched|Handover complete|Path switch complete/.test(e.title);
  if(!strong && !/Handover failed|UE released|Coverage gap/.test(e.title)) continue;
  mk("line",{x1:X(e.t),x2:X(e.t),y1:T,y2:H-Bm,stroke:e.sev==="err"?"var(--err)":e.kind==="ho"?"var(--s1)":"var(--s4)",
   "stroke-width":strong?1.5:1,"stroke-dasharray":strong?"":"3 3",opacity:.8},svg);
 }
 for(const s of d.sats){
  const pts=s.track.filter(p=>p.t>=tStart); if(pts.length<2) continue;
  mk("polyline",{points:pts.map(p=>`${X(p.t)},${Y(p.el)}`).join(" "),fill:"none",stroke:COL(s.color),
   "stroke-width":1.4,opacity:s.role==="released"?.45:.8},svg);
  let seg=[]; const flush=()=>{if(seg.length>1)mk("polyline",{points:seg.join(" "),fill:"none",stroke:COL(s.color),
   "stroke-width":4,"stroke-linecap":"round"},svg);seg=[];};
  for(const p of pts){ if(p.s) seg.push(`${X(p.t)},${Y(p.el)}`); else flush(); } flush();
  const last=pts[pts.length-1];
  const t=mk("text",{x:Math.min(X(last.t)+6,W-Rm-30),y:Y(last.el)-6,"font-size":10,fill:COL(s.color)},svg);
  t.textContent=`SAT${s.sat}`;
 }
}

function fmtCd(s){ if(s<0) return "now"; const m=Math.floor(s/60), r=s%60; return m?`${m}:${String(r).padStart(2,"0")}`:`${r}s`; }
let cdTotal=null;
function drawNext(d){
 const p=d.pending;
 if(!p){ $("cd").textContent="—"; $("route").textContent="no switch pending"; $("cdbar").style.width="0";
  $("nextsub").textContent=d.switch_el!=null?`the next one is armed as soon as this one completes`:""; cdTotal=null; return; }
 if(cdTotal===null||p.in>cdTotal) cdTotal=Math.max(p.in,1);
 $("cd").textContent=fmtCd(p.in);
 $("route").innerHTML=`<b style="color:${COL(p.from)}">SAT${p.from}</b> &rarr; <b style="color:${COL(p.to)}">SAT${p.to}</b> at ${esc(p.at)} UTC`;
 $("cdbar").style.width=`${Math.max(0,Math.min(100,100*(1-p.in/cdTotal)))}%`;
 $("nextsub").textContent=(d.switch_el!=null?`as SAT${p.from} sets through ${d.switch_el}°`:"")+
  (p.el!=null?` · SAT${p.to} will then be at ${p.el.toFixed(1)}°`:"");
}

function drawSats(sats){
 const live=sats.filter(s=>s.role!=="released"), gone=sats.filter(s=>s.role==="released");
 $("sats").innerHTML=live.sort((a,b)=>(a.role==="serving"?-1:1)-(b.role==="serving"?-1:1)).map(s=>{const n=s.now;
  return `<div class="sat"><div class="h"><i style="background:${COL(s.color)}"></i><b>SAT${s.sat}</b>
   <span class="role ${s.role}">${s.role.toUpperCase()}</span><span style="margin-left:auto;color:var(--mute)">${esc(s.last)}</span></div>
   <dl class="kv"><div><dt>Elev</dt>${n.el.toFixed(1)}<span>°</span></div><div><dt>Range</dt>${n.km.toFixed(0)}<span>km</span></div>
   <div><dt>RTT</dt>${n.ms.toFixed(2)}<span>ms</span></div><div><dt>Drift</dt>${n.drift.toFixed(1)}<span>µs/s</span></div>
   <div><dt>Doppler</dt>${n.dop>0?"+":""}${n.dop.toFixed(1)}<span>kHz</span></div>
   <div><dt>Link</dt>${n.el>0?'<span style="color:var(--ok)">up</span>':'<span style="color:var(--err)">below</span>'}</div></dl></div>`;}).join("")
  || `<div class="empty">No satellite in view.</div>`;
 $("gone").hidden=!gone.length;
 $("gone").innerHTML="released: "+gone.map(s=>`<span style="color:${COL(s.color)}">SAT${s.sat}</span> (last ${esc(s.last)})`).join(" · ");
 $("satsum").textContent=`${live.length} in view · ${gone.length} released`;
}

function drawEvents(evs){
 if(!evs.length){ $("events").innerHTML='<div class="empty">Nothing yet.</div>'; return; }
 $("events").innerHTML=evs.slice().reverse().slice(0,120).map(e=>
  `<div class="ev sev-${e.sev}${fresh.has(e.id)?" new":""}"><span class="c">${esc(e.clock)}</span>`+
  `<span class="tag k-${e.kind}">${esc(e.kind)}</span><span><b>${esc(e.title)}</b>${e.n>1?` <span class="d">×${e.n}</span>`:""}`+
  `${e.detail?` <span class="d">${esc(e.detail)}</span>`:""} <span class="d" style="color:var(--mute)">· ${esc(e.src)}</span></span></div>`).join("");
}

async function tick(){
 let d;
 try{ d=await (await fetch("/data",{cache:"no-store"})).json(); }
 catch(e){ $("live").className="dot stale"; return; }
 $("err").hidden=!(d.error||d.stale); $("err").textContent=d.error||d.stale||"";
 $("live").className=d.error||d.stale?"dot stale":"dot";
 const ue=d.ue||{};
 $("uestate").textContent=(ue.state||"—")+(ue.path&&!ue.alive&&ue.state!=="no UE log"?" (log idle)":"");
 $("uestate").style.color={data:"var(--ok)",connected:"var(--ok)",registered:"var(--ok)",idle:"var(--warn)",
  "lost sync":"var(--err)",rejected:"var(--err)",crashed:"var(--err)"}[ue.state]||"";
 $("ueip").textContent=ue.ip||"";
 const evs=d.events||[];
 if(seen===null){ seen=new Set(evs.map(e=>e.id)); }
 else for(const e of evs) if(!seen.has(e.id)){ seen.add(e.id); if(!e.hist){ toast(e); fresh.add(e.id);
  setTimeout(()=>fresh.delete(e.id),15000);} }
 drawEvents(evs);
 $("nswitch").textContent=evs.filter(e=>e.title.startsWith("Satellite switched")).length;
 if(d.error) return;
 $("clock").textContent=d.now;
 const srv=d.sats.find(s=>s.role==="serving");
 $("serving").innerHTML=srv?`<span style="color:${COL(srv.color)}">SAT${srv.sat}</span> ${srv.now.el.toFixed(1)}°`:"—";
 $("skykey").textContent=d.sats.length+" satellites tracked";
 drawMask(d.switch_el); drawSky(d.sats); drawNext(d); drawSats(d.sats); drawChart(d);
}
tick(); setInterval(tick,1000);
</script>
"""


def newest_group(groups):
    present = [g for g in groups if any(os.path.exists(p) for p in g)]
    if not present:
        return None
    return max(present, key=lambda g: max(os.path.getmtime(p) for p in g if os.path.exists(p)))


if __name__ == "__main__":
    args = list(sys.argv[1:])
    if "-h" in args or "--help" in args:
        print(__doc__)
        sys.exit(0)
    if "--port" in args:
        PORT = int(args.pop(args.index("--port") + 1)); args.remove("--port")
    if "--host" in args:
        HOST = args.pop(args.index("--host") + 1); args.remove("--host")
    if "--ue" in args:
        UE_LOG = args.pop(args.index("--ue") + 1); args.remove("--ue")
    LOGS = args or newest_group([PAIR, SWITCH, TRAIN, SOLO]) or SOLO
    if UE_LOG is None:
        UE_LOG = (newest_group([[p] for p in UE_CANDIDATES]) or [None])[0]
    GNBS = [GnbLog(p, i) for i, p in enumerate(LOGS)]
    UE = UeLog(UE_LOG) if UE_LOG else None
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer((HOST, PORT), Handler) as srv:
        print(f"LEO tracker: gNB {', '.join(LOGS)} | UE {UE_LOG or 'none'} | {HOST}:{PORT}", flush=True)
        for addr in local_addresses():
            print(f"    http://{addr}:{PORT}", flush=True)
        srv.serve_forever()
