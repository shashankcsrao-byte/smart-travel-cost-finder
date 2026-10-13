"""
Real Namma Metro network built from GTFS data, routed with OUR Dijkstra.

Data: Namma Metro GTFS by Vonter (github.com/Vonter/bmrcl-gtfs), ODbL licence,
built from BMRCL timetables and OpenStreetMap. Includes official BMRCL fares.

Graph model (state graph, so line changes are not free):
    "IN:KGWA"         entering Majestic station (start of a metro journey)
    "KGWA@PURPLE"     standing on the Purple line platform at Majestic
    "OUT:KGWA"        leaving Majestic station (end of a metro journey)

    IN:A      -> A@LINE    wait for a train      (half the line's headway)
    A@LINE    -> B@LINE    ride to next station  (real km and minutes)
    A@LINE1   -> A@LINE2   change lines          (walk + wait for the other line)
    A@LINE    -> OUT:A     exit                  (0)

Dijkstra minimises travel TIME here, because metro fares are fixed per
origin-destination pair (they do not depend on the path taken).
"""

import csv
import io
import math
import zipfile
from collections import defaultdict
from datetime import datetime, timedelta, timezone

from ..graph import build_graph
from ..routing import dijkstra

IST = timezone(timedelta(hours=5, minutes=30))
DEFAULT_WAIT_MIN = 5.0          # used if headway cannot be derived
INTERCHANGE_WALK_MIN = 1.0      # GTFS transfers.txt: min_transfer_time = 60 s


def _seconds(hhmmss):
    h, m, s = (int(x) for x in hhmmss.split(":"))
    return h * 3600 + m * 60 + s


def haversine_km(a, b):
    """Straight-line distance between two (lat, lng) points on Earth."""
    lat1, lng1, lat2, lng2 = map(math.radians, (*a, *b))
    h = math.sin((lat2 - lat1) / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin((lng2 - lng1) / 2) ** 2
    return 2 * 6371.0 * math.asin(math.sqrt(h))


class MetroNetwork:
    def __init__(self, zip_path):
        self.zip_path = zip_path
        with zipfile.ZipFile(zip_path) as z:
            read = lambda name: list(csv.DictReader(io.TextIOWrapper(z.open(name), encoding="utf-8-sig")))
            stops = read("stops.txt")
            routes = read("routes.txt")
            trips = read("trips.txt")
            stop_times = read("stop_times.txt")
            fare_attributes = read("fare_attributes.txt")
            fare_rules = read("fare_rules.txt")
            fare_products = read("fare_products.txt")
            fare_leg_rules = read("fare_leg_rules.txt")
            timeframes = read("timeframes.txt")

        # ---- stations (parent stations only) ----
        by_id = {s["stop_id"]: s for s in stops}
        self.stations = {
            s["stop_id"]: {"id": s["stop_id"], "name": s["stop_name"],
                           "lat": float(s["stop_lat"]), "lng": float(s["stop_lon"]), "lines": []}
            for s in stops if s.get("location_type") == "1"
        }
        parent_of = lambda stop_id: by_id[stop_id].get("parent_station") or stop_id

        # ---- lines ----
        self.lines = {
            r["route_id"]: {"id": r["route_id"], "name": f"{r['route_short_name']} Line",
                            "color": "#" + (r.get("route_color") or "888888"), "stations": []}
            for r in routes
        }

        # ---- one full-length trip per line and direction gives the station order ----
        trip_info = {t["trip_id"]: t for t in trips}
        calls = defaultdict(list)
        for row in stop_times:
            calls[row["trip_id"]].append(row)
        longest = {}
        weekday_departures = defaultdict(int)
        service_window = {}     # service_id -> (first departure, last departure) in seconds
        for trip_id, rows in calls.items():
            t = trip_info[trip_id]
            key = (t["route_id"], t["direction_id"])
            if key not in longest or len(rows) > len(calls[longest[key]]):
                longest[key] = trip_id
            first = min(rows, key=lambda r: int(r["stop_sequence"]))
            departs = _seconds(first["departure_time"])
            start, end = service_window.get(t["service_id"], (departs, departs))
            service_window[t["service_id"]] = (min(start, departs), max(end, departs))
            if t["service_id"] == "weekday" and 8 * 3600 <= _seconds(first["departure_time"]) < 20 * 3600:
                weekday_departures[key] += 1

        # Average wait = half the weekday daytime headway of that line.
        self.wait_min = {}
        for line_id in self.lines:
            counts = [n for (line, _), n in weekday_departures.items() if line == line_id and n]
            headway = (12 * 60 / max(counts)) if counts else 2 * DEFAULT_WAIT_MIN
            self.wait_min[line_id] = round(headway / 2, 1)

        connections = []
        for (line_id, direction), trip_id in sorted(longest.items()):
            rows = sorted(calls[trip_id], key=lambda r: int(r["stop_sequence"]))
            sequence = [parent_of(r["stop_id"]) for r in rows]
            if direction == "0":
                self.lines[line_id]["stations"] = sequence
            for sid in sequence:
                if line_id not in self.stations[sid]["lines"]:
                    self.stations[sid]["lines"].append(line_id)
            for a, b in zip(rows, rows[1:]):
                km = round(float(b["shape_dist_traveled"]) - float(a["shape_dist_traveled"]), 3)
                minutes = round((_seconds(b["arrival_time"]) - _seconds(a["arrival_time"])) / 60, 2)
                connections.append((f"{parent_of(a['stop_id'])}@{line_id}",
                                    f"{parent_of(b['stop_id'])}@{line_id}", "metro", km, minutes))

        for sid, station in self.stations.items():
            for line_id in station["lines"]:
                state = f"{sid}@{line_id}"
                connections.append((f"IN:{sid}", state, "metro", 0, self.wait_min[line_id]))
                connections.append((state, f"OUT:{sid}", "metro", 0, 0))
                for other in station["lines"]:
                    if other != line_id:
                        connections.append((state, f"{sid}@{other}", "metro", 0,
                                            INTERCHANGE_WALK_MIN + self.wait_min[other]))

        nodes = sorted({c[0] for c in connections} | {c[1] for c in connections})
        self.graph = build_graph(connections, nodes, bidirectional=False)

        # ---- official fares ----
        price = {f["fare_id"]: float(f["price"]) for f in fare_attributes}
        self.token_fare = {(r["origin_id"], r["destination_id"]): price[r["fare_id"]] for r in fare_rules}
        product_amount = {p["fare_product_id"]: float(p["amount"]) for p in fare_products}
        self.smart_card_fare = {}
        for r in fare_leg_rules:
            if "smartcard" in r["fare_product_id"]:
                self.smart_card_fare[(r["from_area_id"], r["to_area_id"], r["from_timeframe_group_id"])] = \
                    product_amount[r["fare_product_id"]]
        self.timeframes = timeframes
        self.service_window = service_window

    # ------------------------------------------------------------------ queries
    def nearest_stations(self, point, k=3, max_km=6.0):
        """k closest stations by straight-line distance: [(station, km)]."""
        ranked = sorted(((haversine_km(point, (s["lat"], s["lng"])), s) for s in self.stations.values()),
                        key=lambda pair: pair[0])
        return [(s, round(d, 2)) for d, s in ranked[:k] if d <= max_km]

    @staticmethod
    def service_id(when):
        """GTFS calendar: Monday and Sunday have their own timetables (holidays ignored)."""
        return {0: "monday", 6: "sunday"}.get(when.astimezone(IST).weekday(), "weekday")

    def is_running(self, when):
        """True if trains depart at this time of day, per the timetable."""
        when = when.astimezone(IST)
        window = self.service_window.get(self.service_id(when))
        if window is None:
            return True
        seconds = when.hour * 3600 + when.minute * 60
        return window[0] <= seconds <= window[1]

    def hours(self, when):
        """First and last departure today as 'HH:MM' strings."""
        window = self.service_window.get(self.service_id(when))
        fmt = lambda sec: f"{sec // 3600:02d}:{sec % 3600 // 60:02d}"
        return (fmt(window[0]), fmt(window[1])) if window else None

    def timeframe(self, when):
        """'peak' or 'offpeak' for a datetime, using the GTFS timeframes table."""
        when = when.astimezone(IST)
        service = self.service_id(when)
        clock = when.strftime("%H:%M:%S")
        for row in self.timeframes:
            if row["service_id"] == service and row["start_time"] <= clock < row["end_time"]:
                return row["timeframe_group_id"]
        return "offpeak"

    def fare(self, origin_id, destination_id, when=None):
        """Official fare for a station pair: token/QR price and smart-card price."""
        when = when or datetime.now(IST)
        frame = self.timeframe(when)
        token = self.token_fare.get((origin_id, destination_id))
        card = self.smart_card_fare.get((origin_id, destination_id, frame))
        return {"token": token, "smart_card": card, "timeframe": frame}

    def trace(self, origin_id, destination_id, when=None):
        """Replay data for the website: every node our Dijkstra settled, in order.

        Each event is one pop from the min-heap, mapped from graph states to stations:
        {"station", "line", "t", "heap": [...], "frontier": [...]}.
        """
        if origin_id not in self.stations or destination_id not in self.stations:
            raise KeyError("Unknown station")
        raw = []
        dijkstra(self.graph, f"IN:{origin_id}", f"OUT:{destination_id}", "metro",
                 weight="time_min", trace=raw)

        def split(node):
            if "@" in node:
                station, line = node.split("@")
                return station, line
            return node.split(":", 1)[1], None

        events = []
        for ev in raw:
            station, line = split(ev["node"])
            if line is None:                      # IN:/OUT: bookkeeping states
                continue
            events.append({
                "station": station, "line": line, "t": round(ev["cost"], 1),
                "heap": [{"station": split(n)[0], "line": split(n)[1], "t": round(c, 1)}
                         for c, n in ev["heap"] if "@" in n],
                "frontier": [{"station": split(n)[0], "line": split(n)[1], "t": round(c, 1)}
                             for n, c in ev["relaxed"] if "@" in n and split(n)[0] != station],
            })
        return {
            "from": self.stations[origin_id],
            "to": self.stations[destination_id],
            "events": events,
            "route": self.route(origin_id, destination_id),
            "fare": self.fare(origin_id, destination_id, when),
            "states": len(self.graph),
        }

    def route(self, origin_id, destination_id):
        """Quickest metro journey between two stations (or None)."""
        if origin_id == destination_id:
            return None
        result = dijkstra(self.graph, f"IN:{origin_id}", f"OUT:{destination_id}", "metro", weight="time_min")
        if result is None:
            return None

        rides = []                      # consecutive stations on the same line
        for u, edge in zip(result.path, result.edges):
            if "@" not in u or "@" not in edge.to:
                continue
            (a, line_a), (b, line_b) = u.split("@"), edge.to.split("@")
            if line_a != line_b:        # a line change, not a ride
                continue
            if rides and rides[-1]["line"] == line_a and rides[-1]["stations"][-1] == a:
                ride = rides[-1]
            else:
                ride = {"line": line_a, "stations": [a], "distance_km": 0.0, "time_min": 0.0}
                rides.append(ride)
            ride["stations"].append(b)
            ride["distance_km"] += edge.distance_km
            ride["time_min"] += edge.time_min

        for ride in rides:
            line = self.lines[ride["line"]]
            ride.update({
                "line_name": line["name"], "color": line["color"],
                "from": self.stations[ride["stations"][0]]["name"],
                "to": self.stations[ride["stations"][-1]]["name"],
                "stops": len(ride["stations"]) - 1,
                "coords": [[self.stations[s]["lat"], self.stations[s]["lng"]] for s in ride["stations"]],
                "distance_km": round(ride["distance_km"], 2),
                "time_min": round(ride["time_min"], 1),
            })
        return {
            "origin": self.stations[origin_id],
            "destination": self.stations[destination_id],
            "rides": rides,
            "changes": max(len(rides) - 1, 0),
            "distance_km": round(result.distance_km, 2),
            "time_min": round(result.time_min, 1),     # includes waiting and line changes
        }
