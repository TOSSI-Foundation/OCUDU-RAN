import math

from dataclasses import dataclass

EARTH_RADIUS_M = 6371000.0

EARTH_MU = 3.986004418e14

SPEED_OF_LIGHT = 299792458.0

def _norm(v):

    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])

def _sub(a, b):

    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])

def _dot(a, b):

    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

def geodetic_to_ecef(lat_deg, lon_deg, alt_m=0.0):

    lat, lon = math.radians(lat_deg), math.radians(lon_deg)

    r = EARTH_RADIUS_M + alt_m

    return (r * math.cos(lat) * math.cos(lon), r * math.cos(lat) * math.sin(lon), r * math.sin(lat))

@dataclass

class LinkState:

    range_m: float

    range_rate_mps: float

    elevation_deg: float

    @property

    def delay_s(self):

        return self.range_m / SPEED_OF_LIGHT

    def doppler_hz(self, carrier_hz):

        return -self.range_rate_mps / SPEED_OF_LIGHT * carrier_hz

class CircularOrbit:

    def __init__(self, altitude_m, ref_lat_deg, ref_lon_deg, heading_deg=0.0):

        self.altitude_m = altitude_m

        self.radius_m = EARTH_RADIUS_M + altitude_m

        self.omega_rad_s = math.sqrt(EARTH_MU / self.radius_m**3)

        self.speed_mps = self.omega_rad_s * self.radius_m

        self.period_s = 2.0 * math.pi / self.omega_rad_s

        lat, lon = math.radians(ref_lat_deg), math.radians(ref_lon_deg)

        u = (math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat))

        east = (-math.sin(lon), math.cos(lon), 0.0)

        north = (

            -math.sin(lat) * math.cos(lon),

            -math.sin(lat) * math.sin(lon),

            math.cos(lat),

        )

        h = math.radians(heading_deg)

        w = tuple(math.cos(h) * north[i] + math.sin(h) * east[i] for i in range(3))

        self._u, self._w = u, w

    def state_at(self, t_s):

        a = self.omega_rad_s * t_s

        ca, sa = math.cos(a), math.sin(a)

        pos = tuple(self.radius_m * (ca * self._u[i] + sa * self._w[i]) for i in range(3))

        vel = tuple(self.speed_mps * (-sa * self._u[i] + ca * self._w[i]) for i in range(3))

        return pos, vel

    def link_to(self, ground_ecef, t_s):

        pos, vel = self.state_at(t_s)

        d = _sub(pos, ground_ecef)

        rng = _norm(d)

        if rng == 0.0:

            return LinkState(0.0, 0.0, 90.0)

        range_rate = _dot(vel, d) / rng

        gr = _norm(ground_ecef)

        sin_el = _dot(d, ground_ecef) / (rng * gr)

        elevation = math.degrees(math.asin(max(-1.0, min(1.0, sin_el))))

        return LinkState(rng, range_rate, elevation)

    def horizon_window_s(self):

        return math.acos(EARTH_RADIUS_M / self.radius_m) / self.omega_rad_s

    EPHEMERIS_POS_STEP_M = 1.3

    EPHEMERIS_VEL_STEP_MPS = 0.06

    def ephemeris_at(self, t_s):

        pos, vel = self.state_at(t_s)

        q = lambda v, step: round(v / step) * step

        return {

            "pos_x": q(pos[0], self.EPHEMERIS_POS_STEP_M),

            "pos_y": q(pos[1], self.EPHEMERIS_POS_STEP_M),

            "pos_z": q(pos[2], self.EPHEMERIS_POS_STEP_M),

            "vel_x": q(vel[0], self.EPHEMERIS_VEL_STEP_MPS),

            "vel_y": q(vel[1], self.EPHEMERIS_VEL_STEP_MPS),

            "vel_z": q(vel[2], self.EPHEMERIS_VEL_STEP_MPS),

        }

class NtnChannel:

    def __init__(self, orbit, ue_ecef, gateway_ecef=None, t_offset=0.0, frozen=False):

        self.orbit = orbit

        self.ue_ecef = ue_ecef

        self.gateway_ecef = gateway_ecef

        self.t_offset = t_offset

        self.frozen = frozen

    def one_way(self, t_s):

        t_s = self.t_offset if self.frozen else t_s + self.t_offset

        svc = self.orbit.link_to(self.ue_ecef, t_s)

        delay = svc.delay_s

        rate = svc.range_rate_mps

        if self.gateway_ecef is not None:

            feeder = self.orbit.link_to(self.gateway_ecef, t_s)

            delay += feeder.delay_s

            rate += feeder.range_rate_mps

        return delay, rate, svc.elevation_deg

    def doppler_hz(self, t_s, carrier_hz):

        _, rate, _ = self.one_way(t_s)

        return -rate / SPEED_OF_LIGHT * carrier_hz

def _self_check():

    orbit = CircularOrbit(altitude_m=600e3, ref_lat_deg=0.0, ref_lon_deg=0.0)

    assert 96.0 * 60 < orbit.period_s < 97.5 * 60, orbit.period_s

    assert 7500 < orbit.speed_mps < 7600, orbit.speed_mps

    ue = geodetic_to_ecef(0.0, 0.0)

    top = orbit.link_to(ue, 0.0)

    assert abs(top.range_m - 600e3) < 1.0, top.range_m

    assert abs(top.range_rate_mps) < 1e-6, top.range_rate_mps

    assert abs(top.elevation_deg - 90.0) < 1e-6, top.elevation_deg

    edge = orbit.link_to(ue, orbit.horizon_window_s())

    assert abs(edge.elevation_deg) < 1e-3, edge.elevation_deg

    assert edge.range_m > 2800e3, edge.range_m

    before = orbit.link_to(ue, -60.0)

    after = orbit.link_to(ue, 60.0)

    assert abs(before.range_m - after.range_m) < 1.0

    assert before.range_rate_mps < 0 < after.range_rate_mps

    assert before.doppler_hz(2.185e9) > 0 > after.doppler_hz(2.185e9)

    assert orbit.link_to(ue, 61.0).delay_s > orbit.link_to(ue, 60.0).delay_s

    svc_only = NtnChannel(orbit, ue).one_way(30.0)[0]

    sat_pos, _ = orbit.state_at(30.0)

    co_located = NtnChannel(orbit, ue, gateway_ecef=sat_pos).one_way(30.0)[0]

    assert abs(svc_only - co_located) < 1e-9

    eph = orbit.ephemeris_at(12.0)

    pos, vel = orbit.state_at(12.0)

    assert abs(eph["pos_x"] - pos[0]) <= CircularOrbit.EPHEMERIS_POS_STEP_M

    assert abs(eph["vel_z"] - vel[2]) <= CircularOrbit.EPHEMERIS_VEL_STEP_MPS

    assert 6.9e6 < math.sqrt(sum(eph[k] ** 2 for k in ("pos_x", "pos_y", "pos_z"))) < 7.0e6

    assert 7500 < math.sqrt(sum(eph[k] ** 2 for k in ("vel_x", "vel_y", "vel_z"))) < 7600

    print("orbit self-check passed")

    win = orbit.horizon_window_s()

    print(f"  period          {orbit.period_s / 60:.1f} min")

    print(f"  speed           {orbit.speed_mps:.0f} m/s")

    print(f"  pass duration   {2 * win / 60:.1f} min horizon to horizon")

    for t in (0.0, win / 2, win):

        ls = orbit.link_to(ue, t)

        print(

            f"  t={t:6.1f}s  elev {ls.elevation_deg:5.1f} deg  "

            f"range {ls.range_m / 1e3:7.1f} km  delay {ls.delay_s * 1e3:5.2f} ms  "

            f"doppler {ls.doppler_hz(2.185e9) / 1e3:+6.1f} kHz"

        )

if __name__ == "__main__":

    _self_check()
