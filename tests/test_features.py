"""Tests for the audit fixes and the new features (v2)."""

import math
import random
import threading
from datetime import datetime

import httpx
import pytest

from src.graph import LOCATIONS, MODES, build_graph
from src.history import HistoryStore
from src.live.cache import TTLCache
from src.live.metro import IST, MetroNetwork, haversine_km
from src.live.olamaps import OlaMapsClient, OlaMapsError, OlaNotFound, clean_query
from src.live.planner import plan_trip
from src.live.ratelimit import SlidingWindowLimiter
from src.live.settings import METRO_GTFS_ZIP
from src.routing import (astar, compare_routes, dijkstra, k_shortest_paths, mixed_route,
                         MINUTE_VALUE)

needs_metro = pytest.mark.skipif(not METRO_GTFS_ZIP.exists(), reason="metro GTFS not downloaded")


@pytest.fixture(scope="module")
def city():
    return build_graph()


@pytest.fixture(scope="module")
def metro():
    if not METRO_GTFS_ZIP.exists():
        pytest.skip("metro GTFS not downloaded")
    return MetroNetwork(METRO_GTFS_ZIP)


def all_simple_paths(graph, src, dst, mode):
    out, stack = [], [(src, [src], [])]
    while stack:
        node, path, edges = stack.pop()
        if node == dst:
            out.append((path, edges))
            continue
        for e in graph[node]:
            if e.mode == mode and e.to not in path:
                stack.append((e.to, path + [e.to], edges + [e]))
    return out


# ------------------------------------------------------------ routing fixes

def test_equal_distance_routes_prefer_the_quicker_one(city):
    # Two bus routes Residential Area -> Market are both the shortest distance.
    for src, dst, mode in [("Residential Area", "Market", "bus"), ("Residential Area", "Metro Station", "bus")]:
        r = dijkstra(city, src, dst, mode)
        paths = all_simple_paths(city, src, dst, mode)
        best_km = min(sum(e.distance_km for e in edges) for _, edges in paths)
        best_time = min(sum(e.time_min for e in edges) for _, edges in paths
                        if sum(e.distance_km for e in edges) == best_km)
        assert r.distance_km == best_km and r.time_min == best_time


def test_exactly_one_fastest_and_one_cheapest_even_with_ties():
    # Two modes with identical time and fare: only one may be called fastest.
    g = build_graph([("A", "B", "bike_taxi", 5, 10), ("A", "B", "car_taxi", 5, 10)], ("A", "B"))
    c = compare_routes(g, "A", "B")
    assert c.fastest is not None and c.cheapest is not None
    assert len(c.tied_fastest) == 1
    assert c.fastest.mode == "bike_taxi"         # cheaper of the tied two wins


@pytest.mark.parametrize("priority", ["cheapest", "balanced", "fastest"])
def test_priority_orders_rows(city, priority):
    c = compare_routes(city, "College", "Airport", priority=priority)
    if priority == "cheapest":
        keys = [(o.fare, o.time_min) for o in c.options]
    elif priority == "fastest":
        keys = [(o.time_min, o.fare) for o in c.options]
    else:
        keys = [(o.fare + MINUTE_VALUE["balanced"] * o.time_min,) for o in c.options]
    assert keys == sorted(keys)
    assert c.options[0].id == {"cheapest": "bus", "fastest": "car_taxi_fastest", "balanced": "metro"}[priority]


# -------------------------------------------------------------------- A*

def test_astar_matches_dijkstra_and_settles_no_more(city):
    # The demo map has no real geography, so use the exact remaining distance
    # (from a reverse Dijkstra) halved: admissible and consistent, but informative.
    for src in LOCATIONS:
        for dst in LOCATIONS:
            if src == dst:
                continue
            d = dijkstra(city, src, dst, "bus")
            remaining = {n: (dijkstra(city, n, dst, "bus").distance_km if n != dst and dijkstra(city, n, dst, "bus") else 0.0)
                         for n in LOCATIONS}
            h = lambda n: remaining[n] / 2
            a = astar(city, src, dst, "bus", h)
            assert (d is None) == (a is None)
            if d:
                assert a.distance_km == d.distance_km and a.pops <= d.pops


@needs_metro
def test_metro_astar_same_arrival_fewer_pops(metro):
    when = datetime(2026, 10, 13, 9, 0, tzinfo=IST)
    o = next(k for k, s in metro.stations.items() if s["name"].startswith("Whitefield"))
    d = next(k for k, s in metro.stations.items() if s["name"] == "Silk Institute")
    dij, ast = metro.journey(o, d, when), metro.journey(o, d, when, use_astar=True)
    assert dij["arrive"] == ast["arrive"]
    assert ast["pops"] < dij["pops"]


# --------------------------------------------------------------- Yen's k paths

def test_k_shortest_paths_match_brute_force(city):
    for src, dst, mode in [("College", "Airport", "bus"), ("College", "Airport", "car_taxi"), ("Market", "Junction", "bike_taxi")]:
        got = k_shortest_paths(city, src, dst, mode, k=4)
        brute = sorted((sum(e.distance_km for e in edges), sum(e.time_min for e in edges))
                       for _, edges in all_simple_paths(city, src, dst, mode))[:4]
        assert [(r.distance_km, r.time_min) for r in got] == brute
        assert len({tuple(r.path) for r in got}) == len(got)          # all different
        assert all(len(set(r.path)) == len(r.path) for r in got)      # loopless


def test_k_shortest_paths_unreachable_is_empty(city):
    assert k_shortest_paths(city, "Hospital", "Airport", "metro", k=3) == []


# ------------------------------------------------------------- mixed routes

def test_mixed_route_can_change_modes_and_pays_each_boarding():
    # Bus reaches B only, metro goes B -> C only: a single mode cannot do A -> C.
    g = build_graph([("A", "B", "bus", 4, 10), ("B", "C", "metro", 5, 6)], ("A", "B", "C"))
    mix = mixed_route(g, "A", "C")
    assert [leg["mode"] for leg in mix.legs] == ["bus", "metro"]
    assert mix.fare == (5 + 2 * 4) + (10 + 3 * 5)     # base fare once per boarding
    assert mix.time_min == 10 + 6 + 5                 # rides + one transfer
    c = compare_routes(g, "A", "C")
    assert [o.id for o in c.options] == ["mixed"]
    assert set(c.unavailable) == set(MODES)


def test_mixed_route_respects_priority(city):
    cheap = mixed_route(city, "Residential Area", "Railway Station", minute_value=MINUTE_VALUE["cheapest"])
    fast = mixed_route(city, "Residential Area", "Railway Station", minute_value=MINUTE_VALUE["fastest"])
    assert cheap.fare <= fast.fare and fast.time_min <= cheap.time_min


# --------------------------------------------------------- concurrency safety

def test_ttl_cache_is_thread_safe():
    cache, errors = TTLCache(max_items=4, ttl_seconds=0.0005), []

    def work():
        for _ in range(5000):
            try:
                k = random.randint(0, 8)
                cache.set(k, 1) if random.random() < 0.5 else cache.get(k)
            except Exception as e:      # pragma: no cover - would be the bug
                errors.append(e)
    threads = [threading.Thread(target=work) for _ in range(8)]
    [t.start() for t in threads]
    [t.join() for t in threads]
    assert errors == []


def test_sliding_window_limiter():
    now = [0.0]
    lim = SlidingWindowLimiter(3, 60, clock=lambda: now[0])
    assert [lim.allow("ip") for _ in range(4)] == [True, True, True, False]
    assert lim.allow("other-ip")
    now[0] = 61
    assert lim.allow("ip")


# ------------------------------------------------------- Ola Maps robustness

def ola(handler):
    calls = []

    def wrapped(request):
        calls.append(request)
        return handler(request)
    client = OlaMapsClient("k", http_client=httpx.Client(base_url="https://api.olamaps.io",
                                                         transport=httpx.MockTransport(wrapped)))
    return client, calls


def test_clean_query_and_blank_search_spend_nothing():
    assert clean_query("  <script>  MG   Road ") == "script MG Road"
    client, calls = ola(lambda r: httpx.Response(200, json={"predictions": []}))
    assert client.autocomplete("   ") == [] and client.autocomplete("<>") == [] and client.autocomplete("a") == []
    assert calls == []


@pytest.mark.parametrize("body", [
    {"routes": [{"legs": [{"distance": "NaN", "duration": 10}]}]},
    {"routes": [{"legs": [{"distance": "inf", "duration": 10}]}]},
    {"routes": [{"legs": [{"distance": -5, "duration": 10}]}]},
    {"routes": "x"},
    {"routes": [None]},
    [],
])
def test_malformed_directions_become_errors_not_crashes(body):
    client, _ = ola(lambda r: httpx.Response(200, json=body))
    with pytest.raises(OlaMapsError):
        client.directions((12.9, 77.6), (12.95, 77.7))


def test_missing_place_is_not_found_and_cached():
    client, calls = ola(lambda r: httpx.Response(404, json={}))
    for _ in range(3):
        with pytest.raises(OlaNotFound):
            client.place_location("nope")
    assert len(calls) == 1                         # misses are remembered


def test_reverse_geocode_and_usage_counter():
    body = {"results": [{"name": "Lalbagh", "formatted_address": "Lalbagh, Bengaluru"}]}
    client, calls = ola(lambda r: httpx.Response(200, json=body))
    assert client.reverse_geocode((12.95, 77.58)) == {"name": "Lalbagh", "address": "Lalbagh, Bengaluru"}
    client.reverse_geocode((12.95, 77.58))         # cached
    assert len(calls) == 1 and client.usage()["calls_today"] == 1


def test_api_key_never_logged(caplog):
    import logging
    caplog.set_level(logging.DEBUG)
    client, _ = ola(lambda r: httpx.Response(200, json={"predictions": []}))
    client.autocomplete("MG Road")
    assert "api_key=k" not in caplog.text


# ---------------------------------------------------------------- metro data

@needs_metro
def test_holiday_calendar(metro):
    # 10 Oct 2026 is a Saturday listed as a holiday in calendar_dates.txt.
    assert metro.services_on(datetime(2026, 10, 10).date()) == {"holiday"}
    assert metro.services_on(datetime(2026, 10, 12).date()) == {"monday"}


@needs_metro
def test_after_midnight_trains_use_previous_day_service(metro):
    # Find a weekday departure written past 24:00 and ride it on the next morning.
    late = max(((dep, key) for key, lst in metro._departures.items() if key[0] == "weekday"
                for dep, _t, _i in lst), key=lambda x: x[0])
    dep, (_svc, line, direction, station) = late
    if dep < 86400:
        pytest.skip("no after-midnight departures in this feed")
    when = datetime(2026, 10, 14, 0, 0, tzinfo=IST)           # Wednesday 00:00, Tuesday was weekday
    when = when.replace(hour=(dep - 86400) // 3600, minute=((dep - 86400) % 3600) // 60)
    assert metro.next_departure(station, when) is not None


@needs_metro
def test_journey_uses_real_clock_times(metro):
    o = next(k for k, s in metro.stations.items() if s["name"] == "Indiranagar")
    d = next(k for k, s in metro.stations.items() if s["name"] == "Mahatma Gandhi Road")
    j = metro.journey(o, d, datetime(2026, 10, 13, 9, 0, tzinfo=IST))
    ride = j["rides"][0]
    assert "09:00" <= ride["depart"] < ride["arrive"]
    assert j["time_min"] == pytest.approx(j["first_wait_min"] + ride["time_min"], abs=0.2)


# ------------------------------------------------------------------- planner

class FakeMaps:
    def directions(self, a, b):
        km = round(haversine_km(a, b) * 1.3, 2)
        return {"distance_km": km, "duration_min": round(km / 20 * 60, 1), "polyline": None}

    def distance_matrix(self, origins, destinations):
        return [[{k: v for k, v in self.directions(o, d).items() if k != "polyline"} for d in destinations]
                for o in origins]


@needs_metro
def test_plan_has_walk_for_short_trips_and_single_fastest(metro):
    a = {"name": "Cubbon Park", "lat": 12.9763, "lng": 77.5929}
    b = {"name": "Vidhana Soudha", "lat": 12.9796, "lng": 77.5906}
    plan = plan_trip(FakeMaps(), metro, a, b, now=datetime(2026, 10, 13, 10, 0, tzinfo=IST))
    walk = next(o for o in plan["options"] if o["mode"] == "walk")
    assert walk["fare"] == 0 and walk["is_cheapest"]
    assert sum(o["is_fastest"] for o in plan["options"]) == 1
    assert sum(o["is_cheapest"] for o in plan["options"]) == 1


@needs_metro
def test_plan_late_night_metro_message(metro):
    a = {"name": "Vijayanagar", "lat": 12.9711, "lng": 77.5370}
    b = {"name": "Whitefield", "lat": 12.9698, "lng": 77.75}
    plan = plan_trip(FakeMaps(), metro, a, b, now=datetime(2026, 10, 13, 23, 50, tzinfo=IST))
    reason = next(u["reason"] for u in plan["unavailable"] if u["mode"] == "metro")
    assert "No metro train soon" in reason


@needs_metro
def test_plan_priority_changes_order(metro):
    a = {"name": "Koramangala", "lat": 12.9352, "lng": 77.6245}
    b = {"name": "Whitefield", "lat": 12.9698, "lng": 77.75}
    when = datetime(2026, 10, 13, 10, 0, tzinfo=IST)
    cheap = plan_trip(FakeMaps(), metro, a, b, now=when, priority="cheapest")
    fast = plan_trip(FakeMaps(), metro, a, b, now=when, priority="fastest")
    assert [o["fare"] for o in cheap["options"]] == sorted(o["fare"] for o in cheap["options"])
    assert [o["time_min"] for o in fast["options"]] == sorted(o["time_min"] for o in fast["options"])


# ------------------------------------------------------------- history store

def test_history_dedupes_repeats_and_caps_size(tmp_path, monkeypatch):
    import src.history as H
    h = HistoryStore(tmp_path / "h.db")
    assert h.add("A", "B") and not h.add("A", "B") and h.add("B", "C")
    monkeypatch.setattr(H, "MAX_ROWS", 3)
    for i in range(6):
        h.add(f"X{i}", "Y")
    assert [r["source"] for r in h.recent(50)] == ["X5", "X4", "X3"]


# ------------------------------------------------------------- API security

@pytest.fixture
def api(tmp_path):
    pytest.importorskip("httpx2")
    from fastapi.testclient import TestClient

    from src.server import create_app

    class Maps(FakeMaps):
        calls = 0

        def autocomplete(self, q):
            from src.live.olamaps import clean_query
            if len(clean_query(q)) < 2:
                return []
            Maps.calls += 1
            return [{"name": "MG Road", "address": "Bengaluru", "place_id": "p", "lat": 12.97, "lng": 77.6}]

        def reverse_geocode(self, point):
            return {"name": "Pinned", "address": "Bengaluru"}

        def usage(self):
            return {"calls_today": Maps.calls, "calls_total": Maps.calls}

    metro_net = MetroNetwork(METRO_GTFS_ZIP) if METRO_GTFS_ZIP.exists() else None
    return TestClient(create_app(db_path=tmp_path / "t.db", maps_client=Maps(), metro=metro_net)), Maps


def test_security_headers_and_csp(api):
    client, _ = api
    page = client.get("/")
    assert page.headers["x-content-type-options"] == "nosniff"
    assert "script-src 'self'" in page.headers["content-security-policy"]
    assert page.headers["cache-control"] == "no-cache"
    assert client.get("/api/health").headers["cache-control"] == "no-store"


def test_cross_site_requests_are_refused(api):
    client, _ = api
    assert client.get("/api/live/status", headers={"Sec-Fetch-Site": "cross-site"}).status_code == 403
    assert client.get("/api/live/status", headers={"Origin": "https://evil.example"}).status_code == 403
    assert client.get("/api/live/status", headers={"Origin": "http://localhost:8000"}).status_code == 200
    assert client.get("/api/health", headers={"Host": "evil.example"}).status_code == 400


def test_blank_place_search_costs_nothing(api):
    client, maps = api
    before = maps.calls
    assert client.get("/api/live/places", params={"q": "   "}).json() == []
    assert maps.calls == before


def test_rate_limit(api, monkeypatch):
    client, _ = api
    statuses = [client.get("/api/live/places", params={"q": "MG"}).status_code for _ in range(95)]
    assert statuses.count(429) >= 1 and statuses[0] == 200


@needs_metro
def test_plan_api_priority_and_departure(api):
    client, _ = api
    params = {"from_lat": 12.9352, "from_lng": 77.6245, "to_lat": 12.9698, "to_lng": 77.75,
              "from_name": "Koramangala", "to_name": "Whitefield", "priority": "fastest", "at": "09:30"}
    body = client.get("/api/live/plan", params=params).json()
    assert body["priority"] == "fastest" and "T09:30" in body["depart_at"]
    times = [o["time_min"] for o in body["options"]]
    assert times == sorted(times)
    bad = client.get("/api/live/plan", params={**params, "from_lat": 77.6245, "from_lng": 12.9352})
    assert bad.status_code == 422 and "swapped" in bad.json()["detail"]


@needs_metro
def test_trace_api_astar_and_lowercase_ids(api):
    client, _ = api
    d = client.get("/api/live/metro/trace", params={"from": "idn", "to": "magr", "at": "10:00"}).json()
    a = client.get("/api/live/metro/trace", params={"from": "IDN", "to": "MAGR", "at": "10:00",
                                                    "algorithm": "astar"}).json()
    assert d["timetable"] and a["algorithm"] == "astar"
    assert len(a["events"]) <= len(d["events"])
    assert a["route"]["arrive"] == d["route"]["arrive"]


def test_alternatives_and_reverse_endpoints(api):
    client, _ = api
    alts = client.get("/api/routes/alternatives",
                      params={"source": "College", "destination": "Airport", "mode": "bus", "k": 3}).json()
    assert [a["rank"] for a in alts] == [1, 2, 3]
    assert alts[0]["distance_km"] <= alts[1]["distance_km"] <= alts[2]["distance_km"]
    assert client.get("/api/live/reverse", params={"lat": 12.95, "lng": 77.58}).json()["name"] == "Pinned"
    assert client.get("/api/routes", params={"source": "   ", "destination": "Airport"}).status_code == 422
