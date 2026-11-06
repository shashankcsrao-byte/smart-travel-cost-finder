#!/usr/bin/env python3
"""Add the Namma Metro Pink Line (elevated section) to the bundled GTFS feed.

The upstream feed (github.com/Vonter/bmrcl-gtfs, version 2026-08-17) has no Pink Line yet.
This adds its first section, Kalena Agrahara - Tavarekere (6 stations, about 7.5 km), which
opens on 13 Nov 2026, so the C backend can route on it from that date:

  stations      OpenStreetMap coordinates (ODbL); Jayadeva Hospital is the existing Yellow
                Line station, so it becomes an interchange
  timetable     ESTIMATED until BMRCL publishes one: every 10 minutes, 06:00 to 23:00,
                about 34 km/h between stations with a 30 s stop
  service dates its own services (PINK_weekday ...) that start on 20261113, with the same
                weekdays and holiday exceptions as the other lines, so trips before the
                opening never use the line
  fares         BMRCL's distance slabs. Every Pink Line trip to the rest of the network passes
                Jayadeva Hospital, so a fare is the official Jayadeva Hospital fare's distance
                plus the Pink Line distance: the official table stays the anchor. (A plain
                slab-of-track-distance rule reproduces only about 80% of the official table,
                so it is not used on its own.)

Run from the project root. It is idempotent: an earlier Pink Line is removed first.
    python3 tools/add_pink_line.py [path/to/bmrcl.zip]
"""
import csv
import heapq
import io
import math
import sys
import zipfile
from collections import defaultdict

FEED = sys.argv[1] if len(sys.argv) > 1 else "data/bmrcl-gtfs/bmrcl.zip"
ROUTE_ID = "PINK"
SERVICE_PREFIX = "PINK_"
OPENING = "20261113"
LINE_KM = 7.5                      # reported length; straight-line hops are scaled up to it
FIRST, LAST, HEADWAY_MIN = 6 * 60, 23 * 60, 10
SPEED_KMPH, DWELL_S = 34, 30
TRANSFER_S = 120
COLOR = "FF49A7"                   # the line's colour on OpenStreetMap

# South to north (direction 0). code, name, lat, lng: OpenStreetMap, Nov 2026.
STATIONS = [
    ("KLAG", "Kalena Agrahara", 12.8715777, 77.5937292),
    ("HLMV", "Hulimavu", 12.8808327, 77.5958602),
    ("IIMB", "IIMB", 12.8957244, 77.5995939),
    ("JPN4", "JP Nagar 4th Phase", 12.9097707, 77.6001389),
    ("JDHP", "Jayadeva Hospital", 12.916731, 77.5999663),   # existing Yellow Line station
    ("TVKR", "Tavarekere", 12.9294984, 77.6005376),
]
NEW = {code for code, *_ in STATIONS if code != "JDHP"}
PINK_PLATFORM = {code: (f"{code}_PF1", f"{code}_PF2") for code in NEW}
PINK_PLATFORM["JDHP"] = ("JDHP_PF3", "JDHP_PF4")
ALL_PINK_PLATFORMS = {p for pair in PINK_PLATFORM.values() for p in pair}

# BMRCL distance slabs (km upper bound, token fare). Smart card: 5% off peak, 10% off-peak.
SLABS = [(2, 10), (4, 20), (6, 30), (8, 40), (10, 50), (15, 60), (20, 70), (25, 80), (math.inf, 90)]
FARE_KM_SCALE = 0.88               # BMRCL fare km per km of track, best fit on the official table
HUB = "JDHP"                       # where the Pink Line meets the rest of the network


def slab(km):
    return next(fare for limit, fare in SLABS if km <= limit + 1e-9)


def slab_range(fare):
    """The fare-distance range (km) that a token fare stands for."""
    lower = 0.0
    for limit, f in SLABS:
        if f == fare:
            return lower, limit
        lower = limit
    raise ValueError(fare)


def haversine(a, b):
    lat1, lng1, lat2, lng2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((lat2 - lat1) / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin((lng2 - lng1) / 2) ** 2
    return 2 * 6371.0088 * math.asin(math.sqrt(h))


def read(z, name):
    text = z.read(name).decode("utf-8-sig")
    rows = list(csv.DictReader(io.StringIO(text)))
    header = next(csv.reader(io.StringIO(text)))
    return header, rows


def write(header, rows):
    out = io.StringIO()
    w = csv.DictWriter(out, fieldnames=header, lineterminator="\n", extrasaction="ignore")
    w.writeheader()
    w.writerows(rows)
    return out.getvalue().encode("utf-8")


def hms(minutes_float):
    s = round(minutes_float * 60)
    return f"{s // 3600:02d}:{s % 3600 // 60:02d}:{s % 60:02d}"


def network_km(stop_times, trips, parent_of, extra_hops):
    """Shortest track distance between every pair of stations, from one trip per line and direction."""
    route_of = {t["trip_id"]: (t["route_id"], t["direction_id"]) for t in trips}
    calls = defaultdict(list)
    for r in stop_times:
        calls[r["trip_id"]].append(r)
    longest = {}
    for trip_id, rows in calls.items():
        key = route_of.get(trip_id)
        if key and (key not in longest or len(rows) > len(calls[longest[key]])):
            longest[key] = trip_id
    adj = defaultdict(dict)

    def link(a, b, km):
        if a != b and (b not in adj[a] or km < adj[a][b]):
            adj[a][b] = adj[b][a] = km

    for trip_id in longest.values():
        rows = sorted(calls[trip_id], key=lambda r: int(r["stop_sequence"]))
        for x, y in zip(rows, rows[1:]):
            link(parent_of[x["stop_id"]], parent_of[y["stop_id"]],
                 abs(float(y["shape_dist_traveled"]) - float(x["shape_dist_traveled"])))
    for a, b, km in extra_hops:
        link(a, b, km)
    dist = {}
    for src in adj:
        best, heap = {src: 0.0}, [(0.0, src)]
        while heap:
            d, u = heapq.heappop(heap)
            if d > best[u]:
                continue
            for v, w in adj[u].items():
                if d + w < best.get(v, math.inf):
                    best[v] = d + w
                    heapq.heappush(heap, (d + w, v))
        dist[src] = best
    return dist


def main():
    with zipfile.ZipFile(FEED) as z:
        names = z.namelist()
        raw = {n: z.read(n) for n in names}
        t = {n: read(z, n) for n in names if n.endswith(".txt")}

    def rows(name):
        return t[name][1]

    # ---- remove an earlier Pink Line, so the script can run again ----
    pink_trips = {r["trip_id"] for r in rows("trips.txt") if r["route_id"] == ROUTE_ID}
    keep = {
        "routes.txt": lambda r: r["route_id"] != ROUTE_ID,
        "trips.txt": lambda r: r["route_id"] != ROUTE_ID,
        "stop_times.txt": lambda r: r["trip_id"] not in pink_trips,
        "stops.txt": lambda r: (r["stop_id"] not in NEW and r["parent_station"] not in NEW
                                and r["stop_id"] not in ALL_PINK_PLATFORMS),
        "areas.txt": lambda r: r["area_id"] not in NEW,
        "stop_areas.txt": lambda r: r["area_id"] not in NEW and r["stop_id"] not in ALL_PINK_PLATFORMS,
        "calendar.txt": lambda r: not r["service_id"].startswith(SERVICE_PREFIX),
        "calendar_dates.txt": lambda r: not r["service_id"].startswith(SERVICE_PREFIX),
        "transfers.txt": lambda r: ROUTE_ID not in (r["from_route_id"], r["to_route_id"]),
        "shapes.txt": lambda r: not r["shape_id"].startswith("s_pink"),
        "fare_rules.txt": lambda r: r["origin_id"] not in NEW and r["destination_id"] not in NEW,
        "fare_leg_rules.txt": lambda r: r["from_area_id"] not in NEW and r["to_area_id"] not in NEW,
    }
    for name, ok in keep.items():
        t[name] = (t[name][0], [r for r in rows(name) if ok(r)])

    # ---- check the fare rule against the official table before using it ----
    parent_of = {r["stop_id"]: (r["parent_station"] or r["stop_id"]) for r in rows("stops.txt")}
    price = {a["fare_id"]: int(float(a["price"])) for a in rows("fare_attributes.txt")}
    official = {(r["origin_id"], r["destination_id"]): price[r["fare_id"]] for r in rows("fare_rules.txt")}
    before = network_km(rows("stop_times.txt"), rows("trips.txt"), parent_of, [])
    pairs = [(o, d) for (o, d) in official if o in before and d in before[o]]
    agree = sum(slab(before[o][d] * FARE_KM_SCALE) == official[(o, d)] for o, d in pairs)
    print(f"plain slab rule: matches {agree}/{len(pairs)} official fares ({agree / max(1, len(pairs)):.1%}); "
          f"Pink fares are anchored on the official {HUB} fares instead")
    if not any(o == HUB for o, _ in official):
        sys.exit(f"the official table has no fares from {HUB}; not writing")

    # ---- stations and platforms ----
    new_stops = []
    for code, name, lat, lng in STATIONS:
        if code in NEW:
            new_stops.append({"stop_name": name, "parent_station": "", "stop_code": code, "zone_id": "", "stop_id": code,
                              "stop_lat": f"{lat:.7f}", "stop_lon": f"{lng:.7f}", "location_type": "1",
                              "wheelchair_boarding": "1"})
        for pf in PINK_PLATFORM[code]:
            new_stops.append({"stop_name": name, "parent_station": code, "stop_code": code, "zone_id": code,
                              "stop_id": pf, "stop_lat": f"{lat:.7f}", "stop_lon": f"{lng:.7f}", "location_type": "",
                              "wheelchair_boarding": "1", "platform_code": pf.rsplit("PF", 1)[1]})
    t["stops.txt"] = (t["stops.txt"][0], rows("stops.txt") + new_stops)
    for s in new_stops:
        parent_of[s["stop_id"]] = s["parent_station"] or s["stop_id"]
    t["areas.txt"] = (t["areas.txt"][0],
                      rows("areas.txt") + [{"area_id": c, "area_name": n} for c, n, *_ in STATIONS if c in NEW])
    t["stop_areas.txt"] = (t["stop_areas.txt"][0], rows("stop_areas.txt") +
                           [{"area_id": c, "stop_id": pf} for c in PINK_PLATFORM for pf in PINK_PLATFORM[c]])

    # ---- the line ----
    t["routes.txt"] = (t["routes.txt"][0], rows("routes.txt") + [{
        "route_long_name": "Kalena Agrahara - Tavarekere", "route_short_name": "Pink", "agency_id": "BMRCL",
        "route_desc": "Pink Line passes through Kalena Agrahara, Jayadeva Hospital and Tavarekere",
        "route_type": "1", "route_id": ROUTE_ID, "route_color": COLOR, "route_text_color": "FFFFFF"}])

    straight = [haversine(STATIONS[i][2:], STATIONS[i + 1][2:]) for i in range(len(STATIONS) - 1)]
    scale = LINE_KM / sum(straight)
    hops = [round(km * scale, 3) for km in straight]
    cum = [0.0]
    for km in hops:
        cum.append(round(cum[-1] + km, 3))

    def along(i, direction):
        return cum[i] if direction == 0 else round(cum[-1] - cum[i], 3)

    shapes = []
    for direction in (0, 1):
        order = list(range(len(STATIONS))) if direction == 0 else list(reversed(range(len(STATIONS))))
        for seq, i in enumerate(order):
            shapes.append({"shape_id": f"s_pink{direction}", "shape_pt_lat": f"{STATIONS[i][2]:.7f}",
                           "shape_pt_lon": f"{STATIONS[i][3]:.7f}", "shape_pt_sequence": str(seq),
                           "shape_dist_traveled": f"{along(i, direction):.3f}"})
    t["shapes.txt"] = (t["shapes.txt"][0], rows("shapes.txt") + shapes)

    # ---- services: same days and holidays as the rest of the network, from the opening date ----
    base = list(rows("calendar.txt"))
    t["calendar.txt"] = (t["calendar.txt"][0], base + [
        {**r, "service_id": SERVICE_PREFIX + r["service_id"], "start_date": max(OPENING, r["start_date"])} for r in base])
    t["calendar_dates.txt"] = (t["calendar_dates.txt"][0], rows("calendar_dates.txt") + [
        {**r, "service_id": SERVICE_PREFIX + r["service_id"]} for r in rows("calendar_dates.txt") if r["date"] >= OPENING])

    # ---- trips and stop times (estimated timetable) ----
    trips, times = [], []
    for service in (r["service_id"] for r in base):
        for direction in (0, 1):
            order = list(range(len(STATIONS))) if direction == 0 else list(reversed(range(len(STATIONS))))
            headsign = STATIONS[order[-1]][1]
            for start in range(FIRST, LAST + 1, HEADWAY_MIN):
                trip_id = f"pink_{service}_{direction}_{start // 60:02d}{start % 60:02d}"
                trips.append({"route_id": ROUTE_ID, "service_id": SERVICE_PREFIX + service, "trip_headsign": headsign,
                              "direction_id": str(direction), "shape_id": f"s_pink{direction}", "trip_id": trip_id,
                              "wheelchair_accessible": "1"})
                clock = float(start)
                for seq, i in enumerate(order):
                    if seq:
                        clock += abs(cum[i] - cum[order[seq - 1]]) / SPEED_KMPH * 60
                    leave = clock + (DWELL_S / 60 if 0 < seq < len(order) - 1 else 0)
                    times.append({"trip_id": trip_id, "arrival_time": hms(clock), "departure_time": hms(leave),
                                  "stop_id": PINK_PLATFORM[STATIONS[i][0]][direction], "stop_sequence": str(seq + 1),
                                  "stop_headsign": headsign, "shape_dist_traveled": f"{along(i, direction):.3f}",
                                  "timepoint": "0"})
                    clock = leave
    t["trips.txt"] = (t["trips.txt"][0], rows("trips.txt") + trips)
    t["stop_times.txt"] = (t["stop_times.txt"][0], rows("stop_times.txt") + times)

    # ---- interchange at Jayadeva Hospital ----
    yellow = [r["stop_id"] for r in rows("stops.txt")
              if r["parent_station"] == "JDHP" and "_PF" in r["stop_id"] and r["stop_id"] not in ALL_PINK_PLATFORMS]
    transfers = []
    for y in yellow:
        for p in PINK_PLATFORM["JDHP"]:
            transfers.append({"from_stop_id": y, "to_stop_id": p, "from_route_id": "YELLOW", "to_route_id": ROUTE_ID,
                              "transfer_type": "2", "min_transfer_time": str(TRANSFER_S)})
            transfers.append({"from_stop_id": p, "to_stop_id": y, "from_route_id": ROUTE_ID, "to_route_id": "YELLOW",
                              "transfer_type": "2", "min_transfer_time": str(TRANSFER_S)})
    t["transfers.txt"] = (t["transfers.txt"][0], rows("transfers.txt") + transfers)

    # ---- fares: every pair that touches a new station ----
    pink_at = {code: cum[i] for i, (code, *_) in enumerate(STATIONS)}

    def fare_km_from_hub(station, towards_hub):
        """Fare distance between the hub and a station off the Pink Line, kept inside the slab
        of its official fare."""
        key = (station, HUB) if towards_hub else (HUB, station)
        lo, hi = slab_range(official[key])
        return min(max(before[HUB][station] * FARE_KM_SCALE, lo + 0.01), hi)

    stations = sorted({r["stop_id"] for r in rows("stops.txt") if r["location_type"] == "1"})
    fare_rules, legs = [], []
    for o in stations:
        for d in stations:
            if o == d or (o not in NEW and d not in NEW):
                continue
            if o in pink_at and d in pink_at:                               # both on the Pink Line
                km = abs(pink_at[o] - pink_at[d])
            elif o in pink_at:                                              # Pink -> rest, via the hub
                if (HUB, d) not in official:
                    continue
                km = abs(pink_at[o] - pink_at[HUB]) + fare_km_from_hub(d, towards_hub=False)
            else:                                                           # rest -> Pink, via the hub
                if (o, HUB) not in official:
                    continue
                km = fare_km_from_hub(o, towards_hub=True) + abs(pink_at[d] - pink_at[HUB])
            fare = slab(km)
            fare_rules.append({"fare_id": f"token{fare}", "origin_id": o, "destination_id": d})
            for frame, card in (("peak", "smartcard_peak"), ("offpeak", "smartcard_offpeak")):
                for product in ("token", "qr", card):
                    legs.append({"leg_group_id": "metro", "network_id": "", "from_area_id": o, "to_area_id": d,
                                 "from_timeframe_group_id": frame, "fare_product_id": f"fare_{fare}_{product}"})
    t["fare_rules.txt"] = (t["fare_rules.txt"][0], rows("fare_rules.txt") + fare_rules)
    t["fare_leg_rules.txt"] = (t["fare_leg_rules.txt"][0], rows("fare_leg_rules.txt") + legs)

    # ---- write the feed back; files this script does not touch are copied byte for byte ----
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as out:
        for n in names:
            out.writestr(n, write(*t[n]) if n in keep else raw[n])
    with open(FEED, "wb") as f:
        f.write(buffer.getvalue())
    print(f"added the Pink Line ({STATIONS[0][1]} - {STATIONS[-1][1]}, {cum[-1]:.2f} km, {len(STATIONS)} stations): "
          f"{len(trips)} trips, {len(fare_rules)} fare pairs, {len(transfers)} transfers, from {OPENING}")


if __name__ == "__main__":
    main()
