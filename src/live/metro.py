"""
Real Namma Metro network built from GTFS data, routed with OUR algorithms.

Data: Namma Metro GTFS by Vonter (github.com/Vonter/bmrcl-gtfs), ODbL licence,
built from BMRCL timetables and OpenStreetMap. Includes official BMRCL fares.

Two ways to route, both written by hand:

1. Timetable search (`journey`, `trace`): a time-dependent Dijkstra.
   A state is "standing at station S on line L at time t". From there you can
     * take the NEXT train of line L in either direction (found with binary search
       over that station's sorted departure times) to the following station, or
     * walk to another line's platform at the same station (+1 minute).
   The cost of a ride depends on WHEN you arrive (you may wait for the train), but
   arriving earlier never makes you arrive later ("FIFO"), so Dijkstra stays
   correct. It uses the real calendar: weekday/Monday/Sunday/holiday timetables,
   and trains that run after midnight belong to the previous day's service.

2. Static average-wait graph (`route`): used when no trains run (late night), and
   as the classic DSA example:
       IN:A -> A@LINE (half the headway), A@LINE -> B@LINE (ride),
       A@LINE1 -> A@LINE2 (change lines), A@LINE -> OUT:A
"""

import bisect
import csv
import heapq
import io
import math
import zipfile
from collections import defaultdict
from datetime import datetime, timedelta, timezone

from ..graph import build_graph
from ..routing import astar, dijkstra

IST = timezone(timedelta(hours=5, minutes=30))
DEFAULT_WAIT_MIN = 5.0          # used if headway cannot be derived
INTERCHANGE_WALK_MIN = 1.0      # GTFS transfers.txt: min_transfer_time = 60 s
DAY = 86400


def _seconds(hhmmss):
    h, m, s = (int(x) for x in hhmmss.split(":"))
    return h * 3600 + m * 60 + s


def _clock(seconds):
    seconds = int(round(seconds)) % DAY
    return f"{seconds // 3600:02d}:{seconds % 3600 // 60:02d}"


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
            calendar = read("calendar.txt")
            calendar_dates = read("calendar_dates.txt")
            fare_attributes = read("fare_attributes.txt")
            fare_rules = read("fare_rules.txt")
            fare_products = read("fare_products.txt")
            fare_leg_rules = read("fare_leg_rules.txt")
            timeframes = read("timeframes.txt")
            feed_info = read("feed_info.txt")

        self.feed_end = datetime.strptime(feed_info[0]["feed_end_date"], "%Y%m%d").date() if feed_info else None

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

        # ---- calendar: which services run on which date ----
        self._calendar = calendar
        self._exceptions = defaultdict(list)          # "YYYYMMDD" -> [(service, type)]
        for row in calendar_dates:
            self._exceptions[row["date"]].append((row["service_id"], row["exception_type"]))

        # ---- trips: ordered stops with arrival/departure seconds ----
        trip_info = {t["trip_id"]: t for t in trips}
        calls = defaultdict(list)
        for row in stop_times:
            calls[row["trip_id"]].append(row)
        self.trip_stops = {}                          # trip -> [(station, arr, dep), ...]
        departures = defaultdict(list)                # (service, line, dir, station) -> [(dep, trip, idx)]
        longest = {}
        weekday_departures = defaultdict(int)
        for trip_id, rows in calls.items():
            rows.sort(key=lambda r: int(r["stop_sequence"]))
            t = trip_info[trip_id]
            line, direction, service = t["route_id"], t["direction_id"], t["service_id"]
            stops_list = [(parent_of(r["stop_id"]), _seconds(r["arrival_time"]), _seconds(r["departure_time"]))
                          for r in rows]
            self.trip_stops[trip_id] = stops_list
            for idx, (station, _arr, dep) in enumerate(stops_list[:-1]):
                departures[(service, line, direction, station)].append((dep, trip_id, idx))
            key = (line, direction)
            if key not in longest or len(rows) > len(calls[longest[key]]):
                longest[key] = trip_id
            if service == "weekday" and 8 * 3600 <= stops_list[0][2] < 20 * 3600:
                weekday_departures[key] += 1
        for lst in departures.values():
            lst.sort()                                # sorted -> binary search works
        self._departures = dict(departures)

        # Average wait = half the weekday daytime headway of that line (static graph only).
        self.wait_min = {}
        for line_id in self.lines:
            counts = [n for (line, _), n in weekday_departures.items() if line == line_id and n]
            headway = (12 * 60 / max(counts)) if counts else 2 * DEFAULT_WAIT_MIN
            self.wait_min[line_id] = round(headway / 2, 1)

        # ---- static graph from one full-length trip per line and direction ----
        connections = []
        self.hop_km = {}                              # (a, b, line) -> km
        for (line_id, direction), trip_id in sorted(longest.items()):
            rows = calls[trip_id]
            sequence = [parent_of(r["stop_id"]) for r in rows]
            if direction == "0":
                self.lines[line_id]["stations"] = sequence
            for sid in sequence:
                if line_id not in self.stations[sid]["lines"]:
                    self.stations[sid]["lines"].append(line_id)
            for a, b in zip(rows, rows[1:]):
                km = round(float(b["shape_dist_traveled"]) - float(a["shape_dist_traveled"]), 3)
                minutes = round((_seconds(b["arrival_time"]) - _seconds(a["arrival_time"])) / 60, 2)
                sa, sb = parent_of(a["stop_id"]), parent_of(b["stop_id"])
                self.hop_km[(sa, sb, line_id)] = km
                connections.append((f"{sa}@{line_id}", f"{sb}@{line_id}", "metro", km, minutes))

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

        # Fastest possible speed (km per second) on any hop: makes the A* estimate
        # "straight-line distance / top speed" a guaranteed underestimate.
        speeds = [km / (m * 60) for _a, _b, mode, km, m in connections if km > 0 and m > 0]
        self.top_speed_km_s = max(speeds) if speeds else 0.02

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

    # ------------------------------------------------------------------ calendar
    def services_on(self, day):
        """Service ids running on a calendar date (calendar.txt + calendar_dates.txt)."""
        key = day.strftime("%Y%m%d")
        weekday = ("monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday")[day.weekday()]
        active = {row["service_id"] for row in self._calendar
                  if row[weekday] == "1" and row["start_date"] <= key <= row["end_date"]}
        for service, kind in self._exceptions.get(key, []):
            if kind == "1":
                active.add(service)
            elif kind == "2":
                active.discard(service)
        return active

    def timeframe(self, when):
        """'peak' or 'offpeak' for a datetime, using the GTFS timeframes table."""
        when = when.astimezone(IST)
        services = self.services_on(when.date())
        clock = when.strftime("%H:%M:%S")
        for row in self.timeframes:
            if row["service_id"] in services and row["start_time"] <= clock < row["end_time"]:
                return row["timeframe_group_id"]
        return "offpeak"

    def feed_days_left(self, today=None):
        today = today or datetime.now(IST).date()
        return (self.feed_end - today).days if self.feed_end else None

    # ------------------------------------------------------------------- queries
    def nearest_stations(self, point, k=3, max_km=6.0):
        """k closest stations by straight-line distance: [(station, km)]."""
        ranked = sorted(((haversine_km(point, (s["lat"], s["lng"])), s) for s in self.stations.values()),
                        key=lambda pair: pair[0])
        return [(s, round(d, 2)) for d, s in ranked[:k] if d <= max_km]

    def fare(self, origin_id, destination_id, when=None):
        """Official fare for a station pair: token/QR price and smart-card price."""
        when = when or datetime.now(IST)
        frame = self.timeframe(when)
        token = self.token_fare.get((origin_id, destination_id))
        card = self.smart_card_fare.get((origin_id, destination_id, frame))
        return {"token": token, "smart_card": card, "timeframe": frame}

    def _ride(self, ride):
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
        return ride

    # -------------------------------------------------------------- static graph
    def route(self, origin_id, destination_id):
        """Quickest journey on the static average-wait graph (or None)."""
        if origin_id == destination_id:
            return None
        result = dijkstra(self.graph, f"IN:{origin_id}", f"OUT:{destination_id}", "metro", weight="time_min")
        if result is None:
            return None
        rides = []
        for u, edge in zip(result.path, result.edges):
            if "@" not in u or "@" not in edge.to:
                continue
            (a, line_a), (b, line_b) = u.split("@"), edge.to.split("@")
            if line_a != line_b:
                continue
            if rides and rides[-1]["line"] == line_a and rides[-1]["stations"][-1] == a:
                ride = rides[-1]
            else:
                ride = {"line": line_a, "stations": [a], "distance_km": 0.0, "time_min": 0.0}
                rides.append(ride)
            ride["stations"].append(b)
            ride["distance_km"] += edge.distance_km
            ride["time_min"] += edge.time_min
        rides = [self._ride(r) for r in rides]
        return {
            "origin": self.stations[origin_id], "destination": self.stations[destination_id],
            "rides": rides, "changes": max(len(rides) - 1, 0),
            "distance_km": round(result.distance_km, 2),
            "time_min": round(result.time_min, 1),     # includes average waiting and line changes
            "timetable": False,
        }

    # ------------------------------------------------------------ timetable search
    def _service_offsets(self, day):
        """Services to search: today's, plus yesterday's shifted back 24 h
        (GTFS writes a 00:20 train of yesterday's service as 24:20:00)."""
        return ([(s, 0) for s in self.services_on(day)] +
                [(s, -DAY) for s in self.services_on(day - timedelta(days=1))])

    def _next_train(self, offsets, line, direction, station, t):
        """Earliest departure of `line` in `direction` from `station` at or after t.
        Binary search on the sorted departure list: O(log n)."""
        best = None
        for service, off in offsets:
            lst = self._departures.get((service, line, direction, station))
            if not lst:
                continue
            i = bisect.bisect_left(lst, (t - off,))
            if i < len(lst):
                dep, trip, idx = lst[i]
                cand = (dep + off, trip, idx, off)
                if best is None or cand < best:
                    best = cand
        return best

    def _search_timetable(self, origin_id, destination_id, depart, trace=None, use_astar=False):
        depart = depart.astimezone(IST)
        t0 = depart.hour * 3600 + depart.minute * 60 + depart.second
        offsets = self._service_offsets(depart.date())
        goal = (self.stations[destination_id]["lat"], self.stations[destination_id]["lng"])

        def h(station):
            if not use_astar:
                return 0.0
            s = self.stations[station]
            return haversine_km((s["lat"], s["lng"]), goal) / self.top_speed_km_s

        best, parent, done = {}, {}, set()
        heap = []
        for line in self.stations[origin_id]["lines"]:
            best[(origin_id, line)] = t0
            heapq.heappush(heap, (t0 + h(origin_id), origin_id, line))
        pops = 0
        found = None
        while heap:
            prio, station, line = heapq.heappop(heap)
            node = (station, line)
            t = best[node]
            if node in done or prio > t + h(station) + 1e-9:
                continue                               # stale entry
            done.add(node)
            pops += 1
            event = None
            if trace is not None:
                event = {"node": node, "cost": (t - t0) / 60, "heap": [], "relaxed": []}
                trace.append(event)
            if station == destination_id:
                found = node
                break

            def relax(nxt, arrive, how):
                if arrive < best.get(nxt, math.inf):
                    best[nxt] = arrive
                    parent[nxt] = (node, how)
                    heapq.heappush(heap, (arrive + h(nxt[0]), nxt[0], nxt[1]))
                    if event is not None:
                        event["relaxed"].append((nxt, (arrive - t0) / 60))

            for direction in ("0", "1"):
                train = self._next_train(offsets, line, direction, station, t)
                if train is None:
                    continue
                dep, trip, idx, off = train
                nxt_station, arr, _dep = self.trip_stops[trip][idx + 1]
                relax((nxt_station, line), arr + off, ("ride", trip, dep))
            for other in self.stations[station]["lines"]:
                if other != line:
                    relax((station, other), t + INTERCHANGE_WALK_MIN * 60, ("change",))

            if event is not None:
                live = [((best[(s, l)] - t0) / 60, (s, l)) for p, s, l in heapq.nsmallest(12, heap)
                        if (s, l) not in done and p <= best[(s, l)] + h(s) + 1e-9]
                event["heap"] = live[:4]
        return found, best, parent, t0, pops

    def journey(self, origin_id, destination_id, depart, use_astar=False):
        """Earliest arrival using the real timetable, leaving the origin at `depart`.

        Returns rides with real board/alight clock times, or None if no train
        can make the trip from that time (for example after the last train)."""
        if origin_id == destination_id:
            return None
        found, best, parent, t0, pops = self._search_timetable(origin_id, destination_id, depart,
                                                               use_astar=use_astar)
        if found is None:
            return None
        hops = []
        node = found
        while node in parent:
            prev, how = parent[node]
            hops.append((prev, node, how))
            node = prev
        hops.reverse()
        rides = []
        for prev, nxt, how in hops:
            if how[0] != "ride":
                continue
            _kind, _trip, dep = how
            a, b, line = prev[0], nxt[0], prev[1]
            if rides and rides[-1]["line"] == line and rides[-1]["stations"][-1] == a:
                ride = rides[-1]
            else:
                ride = {"line": line, "stations": [a], "distance_km": 0.0, "board_s": dep}
                rides.append(ride)
            ride["stations"].append(b)
            ride["distance_km"] += self.hop_km.get((a, b, line)) or haversine_km(
                (self.stations[a]["lat"], self.stations[a]["lng"]),
                (self.stations[b]["lat"], self.stations[b]["lng"]))
            ride["alight_s"] = best[nxt]
        first_board = rides[0]["board_s"] if rides else t0
        for ride in rides:
            ride["time_min"] = (ride["alight_s"] - ride["board_s"]) / 60
            ride["depart"], ride["arrive"] = _clock(ride.pop("board_s")), _clock(ride.pop("alight_s"))
            self._ride(ride)
        arrive_s = best[found]
        return {
            "origin": self.stations[origin_id], "destination": self.stations[destination_id],
            "rides": rides, "changes": max(len(rides) - 1, 0),
            "distance_km": round(sum(r["distance_km"] for r in rides), 2),
            "time_min": round((arrive_s - t0) / 60, 1),        # includes the real wait for trains
            "first_wait_min": round((first_board - t0) / 60, 1),
            "depart": _clock(t0), "arrive": _clock(arrive_s),
            "pops": pops, "timetable": True,
        }

    def next_departure(self, station_id, depart):
        """Next departure from a station on any line, as 'HH:MM', or None."""
        depart = depart.astimezone(IST)
        t = depart.hour * 3600 + depart.minute * 60
        offsets = self._service_offsets(depart.date())
        times = [self._next_train(offsets, line, d, station_id, t)
                 for line in self.stations[station_id]["lines"] for d in ("0", "1")]
        times = [x[0] for x in times if x]
        return _clock(min(times)) if times else None

    # -------------------------------------------------------------------- replay
    def trace(self, origin_id, destination_id, when=None, algorithm="dijkstra"):
        """Replay data for the website: every state the search settled, in order.

        Uses the real timetable at `when`. If no train runs then (late night), it
        falls back to the static average-wait graph and says so."""
        if origin_id not in self.stations or destination_id not in self.stations:
            raise KeyError("Unknown station")
        when = (when or datetime.now(IST)).astimezone(IST)
        use_astar = algorithm == "astar"
        raw = []
        found, *_ = self._search_timetable(origin_id, destination_id, when, trace=raw, use_astar=use_astar)
        if found is not None:
            events = [{
                "station": ev["node"][0], "line": ev["node"][1], "t": round(ev["cost"], 1),
                "heap": [{"station": n[0], "line": n[1], "t": round(c, 1)} for c, n in ev["heap"]],
                "frontier": [{"station": n[0], "line": n[1], "t": round(c, 1)}
                             for n, c in ev["relaxed"] if n[0] != ev["node"][0]],
            } for ev in raw]
            route = self.journey(origin_id, destination_id, when, use_astar=use_astar)
        else:
            events, route = self._static_trace(origin_id, destination_id, use_astar)
        return {
            "from": self.stations[origin_id], "to": self.stations[destination_id],
            "events": events, "route": route, "timetable": found is not None,
            "algorithm": "astar" if use_astar else "dijkstra",
            "at": when.strftime("%H:%M"),
            "fare": self.fare(origin_id, destination_id, when),
            "states": len(self.graph),
        }

    def _static_trace(self, origin_id, destination_id, use_astar):
        raw = []
        goal = self.stations[destination_id]

        def station_of(node):
            return node.split("@")[0] if "@" in node else node.split(":", 1)[1]

        if use_astar:
            def heuristic(node):
                s = self.stations[station_of(node)]
                km = haversine_km((s["lat"], s["lng"]), (goal["lat"], goal["lng"]))
                return km / (self.top_speed_km_s * 60)
            astar(self.graph, f"IN:{origin_id}", f"OUT:{destination_id}", "metro", heuristic,
                  weight="time_min", trace=raw)
        else:
            dijkstra(self.graph, f"IN:{origin_id}", f"OUT:{destination_id}", "metro",
                     weight="time_min", trace=raw)
        events = []
        for ev in raw:
            if "@" not in ev["node"]:
                continue
            station, line = ev["node"].split("@")
            events.append({
                "station": station, "line": line, "t": round(ev["cost"], 1),
                "heap": [{"station": n.split("@")[0], "line": n.split("@")[1], "t": round(c, 1)}
                         for c, n in ev["heap"] if "@" in n],
                "frontier": [{"station": n.split("@")[0], "line": n.split("@")[1], "t": round(c, 1)}
                             for n, c in ev["relaxed"] if "@" in n and n.split("@")[0] != station],
            })
        return events, self.route(origin_id, destination_id)
