#!/usr/bin/env python3
"""Live LEO tracker: satellites, satellite switches, handovers and the UE, straight from the logs.

Follows the gNB log(s) and the UE log and serves a page with: a to-scale side view of every satellite, an
elevation-vs-time chart with the switch threshold, a countdown to the next satellite switch, a timeline of
switches / handovers / UE state changes with pop-up notifications.

    ./scripts/leo_tracker.py                                   # auto: newest run on disk + newest UE log
    ./scripts/leo_tracker.py /tmp/gnb_leo_sattrain.log         # one gNB, a satellite train
    ./scripts/leo_tracker.py /tmp/gnb_leo_sat1.log /tmp/gnb_leo_sat2.log --ue /tmp/ue_leo_ho.log
    ./scripts/leo_tracker.py /tmp/gnb_leo_nrppa.log --ue /tmp/ue_leo_nrppa.log   # positioning run
    ./scripts/leo_tracker.py [gnb logs...] [--ue PATH] [--lmf CONTAINER|none] [--truth LAT,LON] [--port 8500] [--host 0.0.0.0]

Positioning (configs/ntn/leo_nrppa.yml runs): gNB Rx-Tx and UL-RTOA from the NRPPa MeasurementResponse the gNB
logs at nrppa_level debug, SRS reception and TA commands from the PHY/MAC lines, DL-PRS ToA and LPP exchanges from
the UE log; the LMF's NTN Multi-RTT position fixes (phase 4) from `docker logs -f oai-lmf` (--lmf none to skip), scored
against --truth (default the testbed's UE, the North Pole).

Nothing is instrumented: satellites come from the "Emulated NTN channel updated" (serving) and "Sat-switch target
geometry" (next satellite) lines, tagged sat=N so each satellite stays one track across a switch; events come from
the NGAP / RRC / XnAP / NTN lines the gNB already writes and the UE's NAS / RRC / MAC lines. Logs are followed
incrementally, so a long run keeps its whole history, and a log that is recreated for a new run resets its view.
Works on finished logs as well as live ones. The UE log has no timestamps; its events carry the time the tracker
first saw them, marked with a ~.
"""
import collections, http.server, json, math, os, re, socketserver, subprocess, sys, threading, time
from datetime import datetime, timedelta, timezone

PAIR = ["/tmp/gnb_leo_sat1.log", "/tmp/gnb_leo_sat2.log"]
SWITCH = ["/tmp/gnb_leo_satswitch.log"]
TRAIN = ["/tmp/gnb_leo_sattrain.log"]
SOLO = ["/tmp/gnb_leo.log"]
NRPPA = ["/tmp/gnb_leo_nrppa.log"]     # configs/ntn/leo_nrppa.yml, the positioning runs
UE_CANDIDATES = ["/tmp/ue_leo_nrppa.log", "/tmp/ue_leo_sattrain.log", "/tmp/ue_leo_satswitch.log", "/tmp/ue_leo_ho.log", "/tmp/ue_leo.log"]
LOGS, UE_LOG = [], None
PORT = 8500
# Binds every interface so the page can be opened from another machine. Read-only and takes no input, but
# unauthenticated - keep it on a trusted network, or pass --host 127.0.0.1.
HOST = "0.0.0.0"
# Must match configs/ntn/leo_rfsim_gnb.yml: WGS84 polar radius and the orbit radius. Every satellite of a train
# shares the orbit, so one pair of constants covers them all.
B, R = 6356752.314, 6956752.314
C, F_DL = 299792458.0, 2185e6
START_TAIL = 3_000_000   # bytes read from the end of a log when first opened
TRACK_KEEP = 7200        # samples kept per satellite, thinned to one a second: two hours
TRACK_OUT = 600          # samples per satellite handed to the page
EVENTS_KEEP = 400
POS_KEEP = 2000          # positioning measurements kept (NRPPa responses, SRS, PRS)

# Positioning. NRPPa timing values are k_i-encoded (TS 38.455; phy_time_unit::to_ul_rtoa() in the gNB): steps of
# 2^i Tc, and a time of zero reads (985023 >> i) + 1. The UE's PRS ToA is in samples of its FFT, N * SCS.
TC_NS = 1e9 / (480000 * 4096)
SCS_KHZ = 15
TA_STEP_M = 16 * 64 * TC_NS * 1e-9 * C / 2       # one TA command step (TS 38.213 4.2) as slant range, mu = 0
MR_START = re.compile(r"Containerized successfulOutcome\.MeasurementResponse")
MR_KIND = re.compile(r'"(uL-RTOA|gNB-RxTxTimeDiff)"')
MR_K = re.compile(r'"k(\d)": (\d+)')
SRS_LINE = re.compile(r"SRS: .*t_align=([+-][\d.]+)ns .*epre=(-?[\d.inf]+)dB")
TA_CMD = re.compile(r"TA_CMD: tag_id=\d+, ta_cmd=(\d+)")
# One line per TRP per occasion. The gNB index is the TRP index of the LPP assistance data: 0 is the
# reference (serving) TRP, the rest are neighbour satellites (HANDOFF_POSITIONING.md section 19).
UE_PRS = re.compile(r"\[gNB (\d+)\]\[rsc \d+\].*DL PRS ToA ==> ([-\d.]+) / (\d+) samples, "
                    r"peak channel power (\S+) dBm")


# The LMF's NTN Multi-RTT results live only in its container log (docker logs oai-lmf).
LMF_FIX = re.compile(r"NTN Multi-RTT fix \S+: lat ([-\d.]+) lon ([-\d.]+), 1-sigma (\d+) x (\d+) m \(major axis ([-\d]+) deg "
                     r"from north\), range rms ([\d.]+) m, (\d+) rounds over (\d+) s( AMBIGUOUS)?(?: \| alt ([-\d.]+),([-\d.]+))?")
LMF_WAIT = re.compile(r"NTN Multi-RTT fix \S+: not yet \((\d+) rounds (?:from \d+ satellite\(s\) )?over (\d+) s\)")
LMF_RTT = re.compile(r"Multi-RTT: UE Rx-Tx .* = RTT ([\d.]+) us -> ([\d.]+) km")
# Each timed round as the solver sees it: an instant, a range, and where the satellite was.
LMF_ROUND = re.compile(r"NTN Multi-RTT round \S+: t ([\d.]+) .*-> range ([\d.]+) m, satellite (\d+) at "
                       r"([-\d.]+) ([-\d.]+) ([-\d.]+)")
LMF_CONTAINER = "oai-lmf"
TRUTH = (90.0, 0.0)  # where the emulation puts the UE: the gNB's reference_location (--truth lat,lon)
WGS84_A, WGS84_B = 6378137.0, 6356752.314245


def ecef(lat, lon):
    lat, lon = math.radians(lat), math.radians(lon)
    e2 = 1 - WGS84_B ** 2 / WGS84_A ** 2
    n = WGS84_A / math.sqrt(1 - e2 * math.sin(lat) ** 2)
    return (n * math.cos(lat) * math.cos(lon), n * math.cos(lat) * math.sin(lon), n * (1 - e2) * math.sin(lat))


class LmfLog:
    """Follows `docker logs -f oai-lmf` on a thread: the NTN Multi-RTT fixes of the current run and its ranges.
    A fix with fewer rounds than the one before means the LMF started over (restart, or a new pass)."""

    def __init__(self, container):
        self.container, self.lock = container, threading.Lock()
        self.fixes, self.waiting, self.range_km, self.rounds, self.error = [], None, None, 0, None
        self.meas = collections.deque(maxlen=600)  # (t, range m, satellite ECEF)
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                proc = subprocess.Popen(["docker", "logs", "-f", "--tail", "20000", self.container],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
                for line in proc.stdout:
                    self._line(line)
                proc.wait()
                self.error = f"docker logs {self.container} ended (container restarted or gone)"
            except OSError as e:
                self.error = f"cannot run docker logs: {e}"
            time.sleep(3)

    def _line(self, line):
        m = LMF_FIX.search(line)
        with self.lock:
            self.error = None
            if m:
                rounds = int(m[7])
                if self.fixes and rounds < self.fixes[-1]["rounds"]:
                    self.fixes, self.rounds = [], 0
                f = {"lat": float(m[1]), "lon": float(m[2]), "a": float(m[3]), "b": float(m[4]), "orient": float(m[5]),
                     "rms": float(m[6]), "rounds": rounds, "span": float(m[8]), "ambiguous": bool(m[9]),
                     "alt": [float(m[10]), float(m[11])] if m[10] else None}
                f["err"] = round(math.dist(ecef(f["lat"], f["lon"]), ecef(*TRUTH)), 1)
                self.fixes.append(f)
                del self.fixes[:-500]
                self.waiting = None
                return
            m = LMF_WAIT.search(line)
            if m:
                self.waiting = {"rounds": int(m[1]), "span": int(m[2])}
                return
            m = LMF_RTT.search(line)
            if m:
                self.range_km, self.rounds = float(m[2]), self.rounds + 1
                return
            m = LMF_ROUND.search(line)
            if m:
                # A new run, by the LMF's own rule: time went backwards, or more than 120 s since the last round.
                if self.meas and not (0 <= float(m[1]) - self.meas[-1][0] <= 120):
                    self.meas.clear()
                self.meas.append((float(m[1]), float(m[2]), (float(m[4]), float(m[5]), float(m[6])), int(m[3])))

    def view(self):
        with self.lock:
            v = {"fixes": list(self.fixes), "waiting": self.waiting, "range_km": self.range_km,
                 "rounds": self.rounds, "lmf_error": self.error, "truth": TRUTH}
            v["map"] = self.map_view()
            return v

    def map_view(self):
        """The multilateration itself: where the satellite was for each round (its ground point) and the circle
        of positions that round's range allows - the UE is where the circles cross. Drawn in an azimuthal
        equidistant projection centred on the truth, so every great-circle distance from the centre is to scale;
        x east, y north, in km."""
        if not self.meas:
            return None
        lat0, lon0 = (math.radians(v) for v in TRUTH)
        r_surface = math.dist((0, 0, 0), ecef(*TRUTH)) / 1e3  # km

        def project(lat, lon):
            """Great-circle distance and bearing from the centre, as (east, north) km."""
            dlon = lon - lon0
            cosd = math.sin(lat0) * math.sin(lat) + math.cos(lat0) * math.cos(lat) * math.cos(dlon)
            d = math.acos(max(-1.0, min(1.0, cosd))) * r_surface
            brg = math.atan2(math.sin(dlon) * math.cos(lat),
                             math.cos(lat0) * math.sin(lat) - math.sin(lat0) * math.cos(lat) * math.cos(dlon))
            return [round(d * math.sin(brg), 3), round(d * math.cos(brg), 3)]

        def project_deg(lat_deg, lon_deg):
            return project(math.radians(lat_deg), math.radians(lon_deg))

        def sub_point(sat):
            """Sub-satellite latitude and longitude (rad) and the satellite's radius (km)."""
            r = math.dist((0, 0, 0), sat)
            return math.asin(sat[2] / r), math.atan2(sat[1], sat[0]), r / 1e3

        def ring(sat, range_m):
            """The circle of surface points at this slant range from the satellite, projected."""
            lat_s, lon_s, r = sub_point(sat)
            cosang = (r_surface ** 2 + r ** 2 - (range_m / 1e3) ** 2) / (2 * r_surface * r)
            if not -1 <= cosang <= 1:
                return None
            ang = math.acos(cosang)  # central angle from the sub-satellite point
            pts = []
            for k in range(0, 91):
                brg = k * 2 * math.pi / 90
                lat = math.asin(math.sin(lat_s) * math.cos(ang) + math.cos(lat_s) * math.sin(ang) * math.cos(brg))
                lon = lon_s + math.atan2(math.sin(brg) * math.sin(ang) * math.cos(lat_s),
                                         math.cos(ang) - math.sin(lat_s) * math.sin(lat))
                pts.append(project(lat, lon))
            return pts

        meas = list(self.meas)
        # One track per satellite: a satellite switch gives the same cell a second ground track.
        tracks = {}
        for _, _, sat, sid in meas[:: max(1, len(meas) // 160)]:
            lat_s, lon_s, _ = sub_point(sat)
            tracks.setdefault(sid, []).append(project(lat_s, lon_s))
        rings = []
        for t, rng, sat, sid in meas[:: max(1, len(meas) // 6)][:7] + [meas[-1]]:
            pts = ring(sat, rng)
            if pts:
                lat_s, lon_s, _ = sub_point(sat)
                rings.append({"pts": pts, "sub": project(lat_s, lon_s), "t": round(t - meas[0][0], 1),
                              "range_km": round(rng / 1e3, 1), "sat": sid})
        f = self.fixes[-1] if self.fixes else None
        return {"tracks": [{"sat": k, "pts": v} for k, v in sorted(tracks.items())], "rings": rings,
                "span": meas[-1][0] - meas[0][0], "rounds": len(meas),
                "fix": project_deg(f["lat"], f["lon"]) if f else None,
                "alt": project_deg(*f["alt"]) if f and f["alt"] else None,
                "ell": [f["a"] / 1e3, f["b"] / 1e3, f["orient"]] if f else None,
                "ambiguous": bool(f and f["ambiguous"])}

def k_to_ns(i, v):
    return (v - ((985023 >> i) + 1)) * (1 << i) * TC_NS

SAMPLE = re.compile(r"^(\S+) .*Emulated NTN channel updated.*?rx_delay=([\d.]+)us drift=([-0-9.]+)(?:.*? sat=(\d+))?")
TARGET = re.compile(r"^(\S+) .*Sat-switch target geometry.*?rx_delay=([\d.]+)us drift=([-0-9.]+).*? sat=(\d+)")
GNB_LINE = re.compile(r"^(\d{4}-\d\d-\d\dT[0-9:.]+) \[([A-Za-z0-9_-]+)\s*\] \[([IWED])\] (?:\[\s*[0-9.]+\] )?(.*)$")
ANSI = re.compile(r"\x1b\[([0-9;]*)m")

# gNB events: kind, severity, pattern, (title, detail) builder. Order matters: first match wins.
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
    ("pos", "ok", re.compile(r'"TRP Information Exchange Procedure" finished successfully'),
     lambda m: ("LMF read the TRP information", "NRPPa TRP Information Exchange: position and PRS configuration")),
    ("pos", "err", re.compile(r'"(TRP Information Exchange|Measurement) Procedure" (?:failed|timed out)'),
     lambda m: (f"NRPPa {m[1]} failed", "")),
]
HO_RX = re.compile(r"Rx PDU: HandoverRequest$")
RRC_RECONF_DONE = re.compile(r"DCCH UL rrcReconfigurationComplete")
# Errors that are noise, not failures: a core whose PathSwitchRequestAcknowledge carries an ack transfer the gNB
# cannot decode logs one of these per nested structure, while the handover itself completes and the gNB keeps the
# uplink tunnel it already had. The NGAP warning that explains it stays in the gNB log.
GNB_ERROR_SKIP = re.compile(r"Decoding failure|Buffer size limit \d+ was reached")
GNB_EVENT_COMPS = {"NGAP", "RRC", "XNAP", "NTN", "CU-CP", "CU-CP-F1", "CU-CP-E1", "DU-F1", "CU-UP", "GNB", "RF",
                   "APP", "DU", "DU-MNG", "NRPPA"}

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
    ("pos", "ok", re.compile(r"LPP: RequestCapabilities, transaction \d+ -> ProvideCapabilities \((.*)\)"),
     lambda m: ("LPP capability transfer", m[1])),
]
UE_SYNC_FAIL = re.compile(r"synch Failed|pbch not decoded")


def geometry(rx_delay_us, drift):
    """Slant range, elevation and central angle from the round-trip delay."""
    rng = (rx_delay_us * 1e-6 / 2) * C
    clamp = lambda v: max(-1.0, min(1.0, v))
    elev = math.degrees(math.asin(clamp((R * R - B * B - rng * rng) / (2 * B * rng))))
    theta = math.degrees(math.acos(clamp((R * R + B * B - rng * rng) / (2 * R * B))))
    # Sign the along-track angle from the drift: closing means the satellite has not reached zenith, receding that it has.
    return {"km": round(rng / 1000, 1), "el": round(elev, 2), "th": round(theta if drift > 0 else -theta, 3),
            "ms": round(rx_delay_us / 1000, 2), "drift": round(drift, 1),
            "dop": round(-(drift * 1e-6 / 2) * F_DL / 1e3, 1)}


def label_for(path):
    """Short name for a log, so the page says "SAT1" rather than a full path."""
    stem = os.path.basename(path).rsplit(".", 1)[0]
    for prefix in ("gnb_leo_", "gnb_", "leo_", "ue_leo_", "ue_"):
        if stem.startswith(prefix):
            stem = stem[len(prefix):]
    return (stem or "leo").upper()


def log_age(path):
    """Seconds since the file was last written, None if it is gone."""
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
    """Folds a repeat of the previous event into it (a UE looping on a failing re-sync logs the same line hundreds of
    times). Returns True if folded."""
    if events and events[-1]["title"] == title and events[-1]["detail"] == detail and \
            (ts - events[-1]["ts"]).total_seconds() < 120:
        events[-1]["n"] += 1
        return True
    return False


class Follower:
    """Tails one file incrementally. Starts START_TAIL bytes from the end; a recreated or truncated file restarts."""

    def __init__(self, path):
        self.path, self.ino, self.pos, self.partial = path, None, 0, b""

    def poll(self):
        """Returns (new complete lines as (offset, text), restarted, exists)."""
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
            nl = data.find(b"\n")          # skip the partial first line of a mid-file start
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
    """One gNB log: satellite tracks, events and the pending switch."""

    def __init__(self, path, pos):
        self.path, self.pos, self.follow = path, pos, Follower(path)
        self.reset()

    def reset(self):
        self.tracks = collections.OrderedDict()   # sat index -> deque of points
        self.last_keep = {}                        # sat index -> datetime of last kept sample
        self.events = []
        self.pending = None                        # next switch: {"from","to","at","el"}
        self.switch_el, self.last_ts, self.ho_rx_at, self.exists = None, None, None, False
        self.meas = collections.deque(maxlen=POS_KEEP)   # NRPPa responses: {"ts", "rtoa", "rxtx"} in ns
        self.srs_live, self.srs_silent, self.ta_cmds, self.ta_net = 0, 0, 0, 0
        self.mr, self.mr_kind = None, None                # MeasurementResponse being read from its JSON dump

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
            # Thin to one sample a second, but keep the role current so a promotion shows at once.
            if serving and self.tracks[idx] and not self.tracks[idx][-1]["serving"]:
                self.tracks[idx][-1]["serving"] = True
            return
        pt = geometry(float(m.group(2)), float(m.group(3)))
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
            return self._mr_line(line)
        stamp, comp, lvl, msg = g.groups()
        ts = parse_ts(stamp)
        if ts is None:
            return
        self.last_ts = max(self.last_ts or ts, ts)
        self._close_mr()
        if MR_START.search(msg):
            self.mr = {"ts": ts, "rtoa": None, "rxtx": None}
            return
        m = SRS_LINE.search(msg)
        if m:
            if "inf" in m[2]:
                self.srs_silent += 1
            else:
                self.srs_live += 1
            return
        m = TA_CMD.search(msg)
        if m:
            # 31 is "no change"; the others are signed steps around it (TS 38.213 4.2).
            self.ta_cmds, self.ta_net = self.ta_cmds + 1, self.ta_net + int(m[1]) - 31
            return
        if lvl not in "WE" and comp not in GNB_EVENT_COMPS:
            return      # RLC / PDCP / GTP-U / scheduler chatter: no events live there
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

    def _mr_line(self, line):
        """A line of the pretty-printed MeasurementResponse: the measured value follows its quantity's name."""
        if not self.mr:
            return
        m = MR_KIND.search(line)
        if m:
            self.mr_kind = "rtoa" if m[1] == "uL-RTOA" else "rxtx"
            return
        m = MR_K.search(line)
        if m and self.mr_kind and self.mr[self.mr_kind] is None:
            self.mr[self.mr_kind] = round(k_to_ns(int(m[1]), int(m[2])), 1)
            self.mr_kind = None

    def _close_mr(self):
        if self.mr and (self.mr["rtoa"] is not None or self.mr["rxtx"] is not None):
            self.meas.append(self.mr)
        self.mr, self.mr_kind = None, None

    def _event(self, off, ts, kind, sev, title, detail):
        if collapse(self.events, title, detail, ts):
            return
        self.events.append({"id": f"{self.pos}:{self.follow.ino}:{off}", "off": off, "ts": ts, "kind": kind,
                            "sev": sev, "title": title, "detail": detail, "src": "gNB", "n": 1})
        del self.events[:-EVENTS_KEEP]


class UeLog:
    """The UE log: events stamped with the time they were first seen, and the derived UE state."""

    def __init__(self, path):
        self.path, self.follow = path, Follower(path)
        self.reset()

    def reset(self):
        self.events = []
        self.state, self.ip, self.sync_fails, self.switches, self.exists = "no UE log", None, 0, 0, False
        self.primed = False
        self.prs = collections.deque(maxlen=POS_KEEP)  # (TRP, ToA in samples, FFT size, peak power dBm or None)
        self.lpp = 0

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
        m = UE_PRS.search(line)
        if m:
            pwr = None if "inf" in m[4] else float(m[4])
            self.prs.append((int(m[1]), float(m[2]), int(m[3]), pwr))
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
            elif title == "LPP capability transfer":
                self.lpp += 1
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


GNBS, UE, LMF = [], None, None
# Requests are served on threads, and polling mutates the followers: one poll at a time.
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
                "logs": [g.path for g in GNBS], "ue": ue_view(), "events": events_view(None, None),
                "pos": pos_view(None)}
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
    # A gNB that cannot open its log (e.g. a root gNB over a /tmp file another user owns) leaves the page on the
    # previous run with nothing to say so: flag it.
    age = min((a for a in (log_age(g.path) for g in GNBS) if a is not None), default=None)
    stale = (f"{', '.join(g.path for g in GNBS)} not written for {age / 60:.0f} min - this is a finished run. "
             f"Is the gNB writing its log?") if age is not None and age > 10 else None
    return {"now": now.strftime("%H:%M:%S"), "t_now": rel(now), "sats": sats, "pending": pending,
            "switch_el": switch_el, "logs": [g.path for g in GNBS], "ue": ue_view(),
            "events": events_view(t0, now), "stale": stale, "pos": pos_view(rel)}


def pos_view(rel):
    """What the positioning stack has measured so far. There is no position fix to show: Multi-RTT needs the UE's
    Rx-Tx (phase 3) and an LMF solver (phase 4). These are its inputs, plus the testbed's TA-based claim check."""
    meas = sorted((m for g in GNBS for m in g.meas), key=lambda m: m["ts"])
    live = sum(g.srs_live for g in GNBS)
    silent = sum(g.srs_silent for g in GNBS)
    ta_net = sum(g.ta_net for g in GNBS)
    rtoa = [m["rtoa"] for m in meas if m["rtoa"] is not None]
    out = {"n_meas": len(meas), "srs_live": live, "srs_silent": silent,
           "ta_cmds": sum(g.ta_cmds for g in GNBS), "ta_net": ta_net, "ta_step_m": round(TA_STEP_M, 1),
           "claim_err_m": round(ta_net * TA_STEP_M), "coverage": round(live / (live + silent), 3) if live + silent else None,
           "rtoa_last": rtoa[-1] if rtoa else None,
           "rtoa_mean": round(sum(rtoa) / len(rtoa), 1) if rtoa else None,
           "rtoa_sd": round((sum((v - sum(rtoa) / len(rtoa)) ** 2 for v in rtoa) / len(rtoa)) ** .5, 1) if rtoa else None,
           "rxtx_last": next((m["rxtx"] for m in reversed(meas) if m["rxtx"] is not None), None)}
    if rel:
        step = max(1, len(meas) // TRACK_OUT)
        out["series"] = [{"t": rel(m["ts"]), "rtoa": m["rtoa"], "rxtx": m["rxtx"]} for m in meas[::step]]
    if UE:
        prs = list(UE.prs)
        # The reference TRP fills the tiles; the neighbours get their own line, because a neighbour satellite
        # is measured far less often and mixing the two hid that completely.
        ref = [p for p in prs if p[0] == 0]
        hits = [p for p in ref if p[3] is not None]
        out.update(prs_n=len(ref), prs_hits=len(hits), lpp=UE.lpp)
        if hits:
            _, toa, n, pwr = hits[-1]
            mode = collections.Counter(p[1] for p in hits).most_common(1)[0]
            out.update(prs_toa=toa, prs_toa_ns=round(toa / (n * SCS_KHZ * 1e3) * 1e9, 1), prs_pwr=pwr,
                       prs_mode=mode[0], prs_mode_share=round(mode[1] / len(hits), 3))
        neighbours = []
        for trp in sorted({p[0] for p in prs if p[0] != 0}):
            own = [p for p in prs if p[0] == trp]
            got = [p for p in own if p[3] is not None]
            e = {"trp": trp, "n": len(own), "hits": len(got)}
            if got:
                _, toa, n, pwr = got[-1]
                e.update(toa=toa, toa_ns=round(toa / (n * SCS_KHZ * 1e3) * 1e9, 1), pwr=pwr,
                         # what locates the UE: the neighbour's arrival against the reference's
                         rstd_samples=round(toa - (out.get("prs_toa") or 0), 1))
            neighbours.append(e)
        if neighbours:
            out["prs_neighbours"] = neighbours
    if LMF:
        out["lmf"] = LMF.view()
    return out


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
        # UE lines carry no time: live ones get the moment the tracker saw them (~), history just "earlier".
        o["clock"] = "earlier" if e.get("hist") else ("~" if e.get("approx") else "") + e["ts"].strftime("%H:%M:%S")
        o["hist"] = e.get("hist", False)
        o["approx"] = e.get("approx", False)
        # UE times are wall clock and gNB times are the gNB's clock; both are UTC on this host.
        o["t"] = round((e["ts"] - t0).total_seconds(), 1) if t0 and not e.get("hist") else None
        out.append(o)
    return out


def local_addresses():
    """Addresses this host can be reached on, so the printed URL is one you can actually open."""
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
.pos{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:1px;background:var(--rule)}
.pos>div{background:var(--card);padding:10px 13px}
.pos dt{font:10px ui-monospace,monospace;letter-spacing:.08em;text-transform:uppercase;color:var(--mute)}
.pos dd{margin:2px 0 0;font-size:17px;font-weight:600;font-variant-numeric:tabular-nums}
.pos dd span{font-size:11px;font-weight:400;color:var(--ink2)}
.pos p{margin:3px 0 0;font-size:12px;color:var(--ink2)}
.fix{padding:9px 14px;border-bottom:1px solid var(--rule);font-size:13px;color:var(--ink2)}
.gone{padding:8px 13px;font:12px ui-monospace,monospace;color:var(--mute);background:var(--card)}
.events{max-height:420px;overflow:auto}
.ev{display:grid;grid-template-columns:86px 64px minmax(0,1fr);gap:10px;padding:7px 14px;border-bottom:1px solid var(--rule);
 align-items:baseline}
.ev:last-child{border-bottom:0}.ev.new{background:var(--hl)}
.ev .c{font:12px ui-monospace,monospace;color:var(--mute);font-variant-numeric:tabular-nums}
.tag{font:10px ui-monospace,monospace;letter-spacing:.08em;text-transform:uppercase;padding:1px 5px;text-align:center;
 border:1px solid currentColor;justify-self:start}
.k-switch{color:var(--s4)}.k-ho{color:var(--s1)}.k-ue{color:var(--info)}.k-link{color:var(--s2)}.k-pos{color:var(--s3)}.k-error{color:var(--err)}
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

<div class="card"><h2>UE positioning &middot; NRPPa, LPP, DL-PRS<span class="r path" id="possum"></span></h2>
 <div class="fix" id="fix"></div>
 <div class="pos" id="pos"></div>
 <div class="scroll"><svg id="poschart" viewBox="0 0 1300 200" role="img" aria-label="UL-RTOA reported to the LMF over time"></svg></div>
 <div class="fix" style="border-top:1px solid var(--rule)">How the fix is made: each round's range is a circle of possible positions around the satellite's ground point; the UE is where they cross. Left: the whole geometry. Right: zoom on the truth, with the 1-sigma ellipse.</div>
 <div class="grid" style="grid-template-columns:1.35fr 1fr;gap:1px;background:var(--rule)">
  <div style="background:var(--card)"><svg id="map" viewBox="0 0 760 560" role="img" aria-label="Circles of position and the ground track"></svg></div>
  <div style="background:var(--card)"><svg id="zoom" viewBox="0 0 560 560" role="img" aria-label="Fix against the truth, zoomed"></svg></div>
 </div>
 <div class="fix" style="border-top:1px solid var(--rule)">LMF position fix over the pass: error against the truth (solid) and the LMF's own 1-sigma (dashed), log scale; hollow = ambiguous</div>
 <div class="scroll"><svg id="fixchart" viewBox="0 0 1300 200" role="img" aria-label="LMF fix error and uncertainty over the pass"></svg></div></div>

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

const f1=v=>v==null?"—":(v>0?"+":"")+v.toFixed(1);
// The multilateration, in the local plane: circles of position, the satellite's ground track, the fix.
// One colour per satellite: a satellite switch puts a second ground track on the same cell.
const satColour=(s,ring)=>ring?`var(--s${s?3:2})`:`var(--s${s?4:1})`;
function drawMap(M){
 for(const id of ["map","zoom"]) $(id).innerHTML="";
 if(!M){ mk("text",{x:380,y:280,"font-size":13,fill:"var(--mute)","text-anchor":"middle"},$("map")).textContent=
  "waiting for the LMF's Multi-RTT rounds"; return; }
 const draw=(id,W,H,ext,detail)=>{
  const svg=$(id), cx=W/2, cy=H/2, k=Math.min(W,H)/2.3/ext;   // ext = half-width in km
  const X=x=>cx+x*k, Y=y=>cy-y*k;
  // range rings (km) as a scale
  const raw=ext/3, mag=Math.pow(10,Math.floor(Math.log10(raw)));
  const step=(raw/mag>=5?5:raw/mag>=2?2:1)*mag;   // ~3 rings, at a round number
  for(let r=step;r<=ext*1.05;r+=step){
   mk("circle",{cx:X(0),cy:Y(0),r:r*k,fill:"none",stroke:"var(--rule)","stroke-width":1},svg);
   const t=mk("text",{x:X(r*0.707),y:Y(r*0.707)-4,"font-size":10,fill:"var(--mute)","text-anchor":"middle"},svg);
   t.textContent=r<1?`${(r*1000).toFixed(0)} m`:`${r.toFixed(r<10?1:0)} km`;
  }
  mk("line",{x1:X(-ext),x2:X(ext),y1:Y(0),y2:Y(0),stroke:"var(--rule)","stroke-width":1},svg);
  mk("line",{x1:X(0),x2:X(0),y1:Y(-ext),y2:Y(ext),stroke:"var(--rule)","stroke-width":1},svg);
  mk("text",{x:X(0)+4,y:Y(ext)+12,"font-size":10,fill:"var(--mute)"},svg).textContent="N";
  if(detail){
   // circles of position + the satellite ground track
   const last=M.rings.length-1;
   M.rings.forEach((g,i)=>{
    const op=0.3+0.6*i/Math.max(1,last);
    mk("polyline",{points:g.pts.map(p=>`${X(p[0])},${Y(p[1])}`).join(" "),fill:"none",stroke:satColour(g.sat,true),
      "stroke-width":1.3,opacity:op},svg);
    mk("circle",{cx:X(g.sub[0]),cy:Y(g.sub[1]),r:3,fill:satColour(g.sat),opacity:op},svg);
    // Label the ends only: the ground points crowd together, and a label per ring is unreadable.
    if(i===0||i===last){
     const t=mk("text",{x:X(g.sub[0]),y:Y(g.sub[1])+(i===0?-10:18),"font-size":10,fill:"var(--s1)","text-anchor":"middle"},svg);
     t.textContent=`satellite ${g.sat} +${g.t.toFixed(0)} s · ${g.range_km.toFixed(0)} km away`;
    }
   });
   mk("text",{x:24,y:22,"font-size":11,fill:"var(--s2)"},svg).textContent=
     `${M.rings.length} circles of position, from ${M.rounds} round trips over ${M.span.toFixed(0)} s`;
   M.tracks.forEach((tr,i)=>mk("text",{x:24,y:38+i*15,"font-size":11,fill:satColour(tr.sat)},svg).textContent=
     `satellite ${tr.sat}: ground point and track`);
   for(const tr of M.tracks) if(tr.pts.length>1)
    mk("polyline",{points:tr.pts.map(p=>`${X(p[0])},${Y(p[1])}`).join(" "),fill:"none",
      stroke:satColour(tr.sat),"stroke-width":1.5,"stroke-dasharray":"5 4"},svg);
  }
  // truth, fix, ellipse, mirror
  mk("path",{d:`M${X(0)-7},${Y(0)} h14 M${X(0)},${Y(0)-7} v14`,stroke:"var(--ink)","stroke-width":1.6},svg);
  mk("text",{x:X(0)+9,y:Y(0)+13,"font-size":11,fill:"var(--ink2)"},svg).textContent="UE (truth)";
  if(M.alt) { const a=M.alt;
   if(Math.abs(a[0])<ext&&Math.abs(a[1])<ext){
    mk("circle",{cx:X(a[0]),cy:Y(a[1]),r:5,fill:"none",stroke:"var(--warn)","stroke-width":1.5},svg);
    mk("text",{x:X(a[0])+7,y:Y(a[1])+4,"font-size":10,fill:"var(--warn)"},svg).textContent="mirror";}}
  if(M.fix&&!detail){ const f=M.fix, d=Math.hypot(f[0],f[1]);
   mk("line",{x1:X(0),y1:Y(0),x2:X(f[0]),y2:Y(f[1]),stroke:"var(--ink2)","stroke-width":1,"stroke-dasharray":"2 3"},svg);
   const t=mk("text",{x:(X(0)+X(f[0]))/2+8,y:(Y(0)+Y(f[1]))/2,"font-size":11,fill:"var(--ink2)"},svg);
   t.textContent=d<1?`${(d*1000).toFixed(0)} m from the truth`:`${d.toFixed(1)} km from the truth`;
  }
  if(M.fix){ const f=M.fix, col=M.ambiguous?"var(--warn)":"var(--ok)";
   if(M.ell){ const [a,b,o]=M.ell;
    mk("ellipse",{cx:X(f[0]),cy:Y(f[1]),rx:Math.max(a*k,0.5),ry:Math.max(b*k,0.5),fill:col,"fill-opacity":.12,
      stroke:col,"stroke-width":1,"stroke-dasharray":"4 3",transform:`rotate(${o} ${X(f[0])} ${Y(f[1])})`},svg); }
   mk("circle",{cx:X(f[0]),cy:Y(f[1]),r:4.5,fill:col},svg);
   // In the wide view the fix sits on top of the truth: the zoom beside it is where the two are told apart.
   if(!detail) mk("text",{x:X(f[0])+8,y:Y(f[1])-6,"font-size":11,fill:col},svg).textContent=
     M.ambiguous?"LMF fix (ambiguous)":"LMF fix";
  }
 };
 const wide=Math.max(300,...M.rings.map(g=>Math.hypot(g.sub[0],g.sub[1])+40));
 draw("map",760,560,wide,true);
 const e=M.ell?Math.max(M.ell[0]*2.5,0.05):0.5, f=M.fix?Math.hypot(M.fix[0],M.fix[1])*1.6:0.5;
 draw("zoom",560,560,Math.max(e,f,0.05),false);
}

function drawFixes(L){
 const svg=$("fixchart"); svg.innerHTML="";
 const f=L?L.fixes:[];
 if(f.length<2){ mk("text",{x:650,y:105,"font-size":12,fill:"var(--mute)","text-anchor":"middle"},svg).textContent=
  "appears after the LMF's second position fix"; return; }
 const W=1300,H=200,Lm=56,Rm=14,T=14,Bm=24, s0=f[0].span, s1=f[f.length-1].span;
 const lo=1, hi=Math.log10(Math.max(1e3,...f.map(x=>Math.max(x.err,x.a))));
 const X=s=>Lm+(W-Lm-Rm)*(s-s0)/Math.max(1,s1-s0), Y=v=>T+(H-T-Bm)*(1-(Math.log10(Math.max(10,v))-lo)/(hi-lo));
 for(let e=1;e<=Math.ceil(hi);e++){ mk("line",{x1:Lm,x2:W-Rm,y1:Y(10**e),y2:Y(10**e),stroke:"var(--rule)","stroke-width":1},svg);
  mk("text",{x:Lm-6,y:Y(10**e)+4,"font-size":11,fill:"var(--mute)","text-anchor":"end"},svg).textContent=e>=3?(10**(e-3))+" km":(10**e)+" m"; }
 mk("polyline",{points:f.map(x=>`${X(x.span)},${Y(x.a)}`).join(" "),fill:"none",stroke:"var(--mute)","stroke-width":1.2,"stroke-dasharray":"5 4"},svg);
 mk("polyline",{points:f.map(x=>`${X(x.span)},${Y(x.err)}`).join(" "),fill:"none",stroke:"var(--s2)","stroke-width":1.6},svg);
 for(const x of f) mk("circle",{cx:X(x.span),cy:Y(x.err),r:3,fill:x.ambiguous?"var(--card)":"var(--s2)",stroke:"var(--s2)","stroke-width":1.2},svg);
 for(const k of [0,.5,1]){ const s=s0+(s1-s0)*k;
  mk("text",{x:X(s),y:H-6,"font-size":11,fill:"var(--mute)","text-anchor":"middle"},svg).textContent=Math.round(s)+" s of data"; }
}

function drawPos(p){
 if(!p){return;}
 drawFixes(p.lmf);
 drawMap(p.lmf?p.lmf.map:null);
 const cov=p.coverage, verified=cov!=null&&cov>=0.2;
 const LM=p.lmf, fx=LM&&LM.fixes.length?LM.fixes[LM.fixes.length-1]:null;
 const km=m=>m>=1000?(m/1000).toFixed(m>=1e4?0:1)+" km":m.toFixed(0)+" m";
 $("fix").innerHTML=!LM?`<b>UE position:</b> LMF not followed (--lmf none).`:
  LM.lmf_error&&!fx?`<b>UE position:</b> <span style="color:var(--err)">${esc(LM.lmf_error)}</span>`:
  !fx?`<b>UE position: not yet.</b> `+(LM.waiting?`The LMF has ${LM.waiting.rounds} round trips over ${LM.waiting.span} s; a fix needs 5 over 20 s.`:
       `Waiting for the LMF's NTN Multi-RTT rounds (TS 38.305 8.10: one satellite, several instants).`):
  fx.ambiguous?`<b>UE position: ambiguous</b> after ${fx.rounds} round trips over ${fx.span.toFixed(0)} s - before the satellite passes the UE, `+
    `two points fit (${fx.lat.toFixed(4)}°, ${fx.lon.toFixed(4)}° and ${fx.alt?fx.alt[0].toFixed(4)+"°, "+fx.alt[1].toFixed(4)+"°":"?"}); no verdict yet.`:
  `<b>UE position: ${fx.lat.toFixed(6)}°, ${fx.lon.toFixed(6)}°</b> ± ${km(fx.a)} × ${km(fx.b)} (1σ) from ${fx.rounds} round trips `+
    `over ${fx.span.toFixed(0)} s to one satellite · <b>${km(fx.err)}</b> from the truth (${LM.truth[0]}°, ${LM.truth[1]}°).`;
 const tile=(k,v,u,note)=>`<div><dt>${k}</dt><dd>${v}${u?`<span> ${u}</span>`:""}</dd>${note?`<p>${note}</p>`:""}</div>`;
 $("pos").innerHTML=[
  tile("UE position (LMF)",fx?km(fx.err):"—",fx?"from truth":"",fx?`${fx.ambiguous?"AMBIGUOUS · ":""}1σ ${km(fx.a)} × ${km(fx.b)} · range rms ${fx.rms.toFixed(0)} m`:"NTN Multi-RTT, phase 4"),
  tile("Range to satellite",LM&&LM.range_km!=null?LM.range_km.toFixed(1):"—","km",LM?`c·RTT/2 · ${LM.rounds} Multi-RTT rounds (UE + gNB Rx-Tx)`:""),
  tile("gNB Rx-Tx",p.rxtx_last==null?"—":(p.rxtx_last/1000).toFixed(3),"µs",`NRPPa · ${p.n_meas} measurement responses`),
  tile("UL-RTOA",f1(p.rtoa_last),"ns",p.rtoa_mean==null?"no SRS measured":`mean ${f1(p.rtoa_mean)} · sd ${p.rtoa_sd.toFixed(0)} ns · ${(p.rtoa_sd*0.2998).toFixed(0)} m`),
  tile("SRS received",cov==null?"—":(100*cov).toFixed(0),"%",`${p.srs_live} with energy · ${p.srs_silent} silent`),
  tile("DL-PRS ToA",p.prs_toa==null?"—":f1(p.prs_toa),p.prs_toa==null?"":`samples (${f1(p.prs_toa_ns)} ns)`,
   p.prs_n?`serving TRP · ${p.prs_hits}/${p.prs_n} occasions with a peak${p.prs_pwr!=null?` · peak ${f1(p.prs_pwr)} dBm`:""}`+
   (p.prs_mode!=null?` · ${(100*p.prs_mode_share).toFixed(0)}% at ${f1(p.prs_mode)}`:""):"UE saw no PRS occasion"),
  // A neighbour satellite's own DL-PRS. Its arrival against the serving TRP's is the range difference that
  // places the UE at ONE instant instead of over a pass (HANDOFF_POSITIONING.md section 19).
  ...((p.prs_neighbours||[]).map(nb=>tile(`Neighbour TRP ${nb.trp}`,
    nb.hits?f1(nb.toa):"—",nb.hits?`samples (${f1(nb.toa_ns)} ns)`:"",
    nb.hits?`${nb.hits}/${nb.n} occasions${nb.pwr!=null?` · peak ${f1(nb.pwr)} dBm`:""}`+
      (nb.rstd_samples!=null?` · ${nb.rstd_samples>0?"+":""}${f1(nb.rstd_samples)} samples from the serving TRP`:"")
      :`0/${nb.n} occasions detected - its PRS is not being found`))),
  tile("LPP",p.lpp??"—","capability transfers","UE: NR Multi-RTT with NTN measurement and report; then assistance data and location information"),
  tile("Claim check (TA)",verified?(p.claim_err_m>0?"+":"")+p.claim_err_m:"—","m",
   verified?`net TA ${p.ta_net>0?"+":""}${p.ta_net} steps (±${p.ta_step_m} m each) · testbed check, not a 3GPP method`:
   `unverified: SRS coverage ${cov==null?"none":(100*cov).toFixed(0)+"%"} (need 20%)`),
 ].join("");
 $("possum").textContent=p.n_meas?`chart: UL-RTOA reported to the LMF, the residual after the UE's own NTN pre-compensation`:"no NRPPa measurement yet";
 const svg=$("poschart"); svg.innerHTML="";
 const pts=(p.series||[]).filter(m=>m.rtoa!=null);
 if(pts.length<2){ mk("text",{x:650,y:105,"font-size":12,fill:"var(--mute)","text-anchor":"middle"},svg).textContent=
  "UL-RTOA over time appears once the LMF has made two measurements"; return; }
 const W=1300,H=200,L=56,Rm=14,T=14,Bm=24, t0=pts[0].t, t1=pts[pts.length-1].t;
 let lo=Math.min(...pts.map(m=>m.rtoa)), hi=Math.max(...pts.map(m=>m.rtoa)); if(hi-lo<100){const c=(hi+lo)/2;lo=c-50;hi=c+50;}
 const X=t=>L+(W-L-Rm)*(t-t0)/Math.max(1,t1-t0), Y=v=>T+(H-T-Bm)*(1-(v-lo)/(hi-lo));
 for(const v of [lo,(lo+hi)/2,hi]){
  mk("line",{x1:L,x2:W-Rm,y1:Y(v),y2:Y(v),stroke:"var(--rule)","stroke-width":1},svg);
  mk("text",{x:L-6,y:Y(v)+4,"font-size":11,fill:"var(--mute)","text-anchor":"end"},svg).textContent=Math.round(v)+" ns";
 }
 if(lo<0&&hi>0) mk("line",{x1:L,x2:W-Rm,y1:Y(0),y2:Y(0),stroke:"var(--ink2)","stroke-width":1,"stroke-dasharray":"6 4"},svg);
 mk("polyline",{points:pts.map(m=>`${X(m.t)},${Y(m.rtoa)}`).join(" "),fill:"none",stroke:"var(--s3)","stroke-width":1.4},svg);
 for(const m of pts) mk("circle",{cx:X(m.t),cy:Y(m.rtoa),r:2,fill:"var(--s3)"},svg);
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
 drawEvents(evs); drawPos(d.pos);
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
    if "--truth" in args:
        TRUTH = tuple(float(v) for v in args.pop(args.index("--truth") + 1).split(",")); args.remove("--truth")
    if "--lmf" in args:
        LMF_CONTAINER = args.pop(args.index("--lmf") + 1); args.remove("--lmf")
    if "--ue" in args:
        UE_LOG = args.pop(args.index("--ue") + 1); args.remove("--ue")
    # The run written most recently wins, so a stale earlier run never shadows the live one.
    LOGS = args or newest_group([PAIR, SWITCH, TRAIN, SOLO, NRPPA]) or SOLO
    if UE_LOG is None:
        UE_LOG = (newest_group([[p] for p in UE_CANDIDATES]) or [None])[0]
    GNBS = [GnbLog(p, i) for i, p in enumerate(LOGS)]
    UE = UeLog(UE_LOG) if UE_LOG else None
    LMF = LmfLog(LMF_CONTAINER) if LMF_CONTAINER != "none" else None
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer((HOST, PORT), Handler) as srv:
        print(f"LEO tracker: gNB {', '.join(LOGS)} | UE {UE_LOG or 'none'} | {HOST}:{PORT}", flush=True)
        for addr in local_addresses():
            print(f"    http://{addr}:{PORT}", flush=True)
        srv.serve_forever()
