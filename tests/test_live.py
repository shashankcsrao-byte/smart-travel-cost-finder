"""
Tests for live mode. They run offline: Ola Maps is replaced by a fake HTTP
transport or a fake client, and the metro tests use the downloaded GTFS file.
"""

from datetime import datetime

import httpx
import pytest

from src.live import fares as F
from src.live.cache import TTLCache
from src.live.metro import IST, MetroNetwork, haversine_km
from src.live.olamaps import OlaMapsClient, OlaMapsError
from src.live.planner import plan_trip, uber_link
from src.live.settings import METRO_GTFS_ZIP

needs_metro = pytest.mark.skipif(not METRO_GTFS_ZIP.exists(), reason="metro GTFS not downloaded")

KORAMANGALA = {"name": "Koramangala", "lat": 12.9352, "lng": 77.6245}
WHITEFIELD = {"name": "Whitefield", "lat": 12.9698, "lng": 77.7500}
MG_ROAD = {"name": "MG Road", "lat": 12.9756, "lng": 77.6066}
INDIRANAGAR = {"name": "Indiranagar", "lat": 12.9784, "lng": 77.6408}


@pytest.fixture(scope="module")
def metro():
    if not METRO_GTFS_ZIP.exists():
        pytest.skip("metro GTFS not downloaded")
    return MetroNetwork(METRO_GTFS_ZIP)


def station_id(metro, text):
    return next(sid for sid, s in metro.stations.items() if text.lower() in s["name"].lower())


# ------------------------------------------------------------------ fares

def test_auto_meter_fare_minimum_and_per_km():
    assert F.auto_fare(1.5, hour=12) == (36, False)          # within first 2 km
    assert F.auto_fare(10, hour=12) == (36 + 18 * 8, False)  # 180
    assert F.auto_fare(10, hour=23) == (270, True)           # night +50%
    assert F.auto_fare(10, hour=4)[1] is True
    assert F.auto_fare(10, hour=5)[1] is False


def test_cab_and_bike_fares():
    assert F.cab_fare(3) == 100
    assert F.cab_fare(10) == 100 + 24 * 6
    assert F.bike_fare(5) == 20 + 8 * 5


def test_bmtc_stage_fares():
    assert F.bmtc_fare(1.2) == (6, True)
    assert F.bmtc_fare(3.9) == (12, True)
    assert F.bmtc_fare(6.0) == (18, True)
    assert F.bmtc_fare(10) == (24, False)    # estimated: 2020 slab 20 x 1.2
    assert F.bmtc_fare(15) == (30, False)    # estimated: 2020 slab 25 x 1.2
    assert F.bmtc_fare(55) == (36, False)    # estimated: 2020 slab 30 x 1.2


def test_bmtc_estimate_uses_verified_ratio():
    # The verified 2025 stages are exactly 1.2 x the 2020 stages, the basis of the estimate.
    old_2020 = [5, 10, 15, 20, 25, 30]
    new = [fare for _, fare in F.BMTC_ORDINARY["verified_stages"] + F.BMTC_ORDINARY["estimated_stages"]]
    assert new == [round(f * 1.2) for f in old_2020]


# ------------------------------------------------------------------ cache

def test_ttl_cache_expiry_and_lru_eviction():
    now = [0.0]
    cache = TTLCache(max_items=2, ttl_seconds=10, clock=lambda: now[0])
    cache.set("a", 1)
    cache.set("b", 2)
    assert cache.get("a") == 1          # "a" is now most recently used
    cache.set("c", 3)                   # evicts least recently used: "b"
    assert cache.get("b") is None and cache.get("c") == 3
    now[0] = 11
    assert cache.get("a") is None       # expired


# ------------------------------------------------------- Ola Maps client

def fake_ola(handler):
    return OlaMapsClient("test-key", http_client=httpx.Client(
        base_url="https://api.olamaps.io", transport=httpx.MockTransport(handler)))


def test_directions_parsing_and_api_key():
    seen = {}

    def handler(request):
        seen["method"] = request.method
        seen["path"] = request.url.path
        seen["params"] = dict(request.url.params)
        return httpx.Response(200, json={"routes": [{
            "legs": [{"distance": 8200, "duration": 1500}, {"distance": {"value": 800}, "duration": {"value": 300}}],
            "overview_polyline": "abc"}]})

    route = fake_ola(handler).directions((12.9, 77.6), (12.95, 77.7))
    assert route == {"distance_km": 9.0, "duration_min": 30.0, "polyline": "abc"}
    assert seen["method"] == "POST" and seen["path"] == "/routing/v1/directions"
    assert seen["params"]["api_key"] == "test-key"
    assert seen["params"]["origin"] == "12.900000,77.600000"


def test_directions_are_cached():
    calls = []

    def handler(request):
        calls.append(1)
        return httpx.Response(200, json={"routes": [{"legs": [{"distance": 1000, "duration": 120}]}]})

    client = fake_ola(handler)
    client.directions((12.9, 77.6), (12.95, 77.7))
    client.directions((12.9, 77.6), (12.95, 77.7))
    assert len(calls) == 1


def test_autocomplete_parsing():
    def handler(request):
        assert request.url.params["input"] == "mg road"
        return httpx.Response(200, json={"predictions": [{
            "description": "MG Road, Bengaluru, Karnataka", "place_id": "p1",
            "structured_formatting": {"main_text": "MG Road", "secondary_text": "Bengaluru, Karnataka"},
            "geometry": {"location": {"lat": 12.97, "lng": 77.6}}}]})

    [place] = fake_ola(handler).autocomplete("  mg   road ")
    assert place == {"name": "MG Road", "address": "Bengaluru, Karnataka", "place_id": "p1", "lat": 12.97, "lng": 77.6}


def test_distance_matrix_parsing():
    def handler(request):
        assert request.url.params["destinations"].count("|") == 1
        return httpx.Response(200, json={"rows": [{"elements": [
            {"distance": 1500, "duration": 300, "status": "OK"},
            {"status": "ZERO_RESULTS"}]}]})

    [row] = fake_ola(handler).distance_matrix([(12.9, 77.6)], [(12.91, 77.61), (12.92, 77.62)])
    assert row == [{"distance_km": 1.5, "duration_min": 5.0}, None]


@pytest.mark.parametrize("status, message", [(401, "API key"), (429, "rate limit"), (500, "error 500")])
def test_errors_become_friendly_messages(status, message):
    client = fake_ola(lambda request: httpx.Response(status, json={}))
    with pytest.raises(OlaMapsError, match=message):
        client.directions((12.9, 77.6), (12.95, 77.7))


def test_missing_key_rejected():
    with pytest.raises(OlaMapsError):
        OlaMapsClient("")


# ------------------------------------------------------------------ metro

@needs_metro
def test_metro_network_loaded(metro):
    assert len(metro.stations) == 83
    assert set(metro.lines) == {"PURPLE", "GREEN", "YELLOW"}
    majestic = metro.stations[station_id(metro, "Majestic")]
    assert sorted(majestic["lines"]) == ["GREEN", "PURPLE"]


@needs_metro
def test_metro_same_line_route(metro):
    r = metro.route(station_id(metro, "Indiranagar"), station_id(metro, "Mahatma Gandhi"))
    assert r["changes"] == 0
    assert [ride["line"] for ride in r["rides"]] == ["PURPLE"]
    assert r["rides"][0]["stations"][0] == station_id(metro, "Indiranagar")


@needs_metro
def test_metro_interchange_at_majestic(metro):
    r = metro.route(station_id(metro, "Whitefield"), station_id(metro, "Silk Institute"))
    assert [ride["line"] for ride in r["rides"]] == ["PURPLE", "GREEN"]
    assert r["rides"][0]["stations"][-1] == station_id(metro, "Majestic")
    riding = sum(ride["time_min"] for ride in r["rides"])
    assert r["time_min"] > riding        # waiting + line change are counted


@needs_metro
def test_metro_fare_matches_official_table(metro):
    o, d = station_id(metro, "Indiranagar"), station_id(metro, "Mahatma Gandhi")
    assert metro.fare(o, d)["token"] == metro.token_fare[(o, d)]
    peak = metro.fare(o, d, datetime(2026, 10, 13, 9, 0, tzinfo=IST))     # Tuesday 9 AM
    offpeak = metro.fare(o, d, datetime(2026, 10, 13, 14, 0, tzinfo=IST))
    assert peak["timeframe"] == "peak" and offpeak["timeframe"] == "offpeak"
    assert offpeak["smart_card"] <= peak["smart_card"] <= peak["token"]


@needs_metro
def test_metro_operating_hours(metro):
    assert metro.is_running(datetime(2026, 10, 13, 9, 0, tzinfo=IST))
    assert not metro.is_running(datetime(2026, 10, 13, 2, 0, tzinfo=IST))


@needs_metro
def test_nearest_stations_sorted(metro):
    near = metro.nearest_stations((KORAMANGALA["lat"], KORAMANGALA["lng"]), k=3)
    distances = [km for _, km in near]
    assert distances == sorted(distances) and len(near) == 3


# ---------------------------------------------------------------- planner

class FakeMaps:
    """Stands in for Ola Maps: straight-line distance x 1.3 at 20 km/h."""

    def directions(self, a, b):
        km = round(haversine_km(a, b) * 1.3, 2)
        return {"distance_km": km, "duration_min": round(km / 20 * 60, 1), "polyline": None}

    def distance_matrix(self, origins, destinations):
        return [[{k: v for k, v in self.directions(o, d).items() if k != "polyline"} for d in destinations]
                for o in origins]


@needs_metro
def test_plan_trip_ranks_all_modes(metro):
    now = datetime(2026, 10, 13, 10, 0, tzinfo=IST)
    plan = plan_trip(FakeMaps(), metro, KORAMANGALA, WHITEFIELD, now=now)
    modes = [o["mode"] for o in plan["options"]]
    assert set(modes) == {"auto", "cab", "bike_taxi", "bus", "metro"}
    fares = [o["fare"] for o in plan["options"]]
    assert fares == sorted(fares)
    assert plan["options"][0]["is_cheapest"]
    assert sum(o["is_fastest"] for o in plan["options"]) >= 1

    metro_option = next(o for o in plan["options"] if o["mode"] == "metro")
    kinds = [leg["kind"] for leg in metro_option["legs"]]
    assert kinds[0] in ("walk", "auto") and kinds[-1] in ("walk", "auto") and "metro" in kinds
    assert metro_option["fare_kind"] == "official"


@needs_metro
def test_plan_trip_metro_closed_at_night(metro):
    plan = plan_trip(FakeMaps(), metro, KORAMANGALA, WHITEFIELD, now=datetime(2026, 10, 13, 2, 0, tzinfo=IST))
    assert [u["mode"] for u in plan["unavailable"]] == ["metro"]
    auto = next(o for o in plan["options"] if o["mode"] == "auto")
    assert "night" in auto["fare_text"]


@needs_metro
def test_plan_trip_same_station_area_has_no_metro(metro):
    near_indiranagar = {"name": "Nearby", "lat": 12.9790, "lng": 77.6420}
    plan = plan_trip(FakeMaps(), metro, INDIRANAGAR, near_indiranagar, now=datetime(2026, 10, 13, 10, 0, tzinfo=IST))
    assert "metro" in [u["mode"] for u in plan["unavailable"]]


def test_uber_link_has_pickup_and_drop():
    link = uber_link(MG_ROAD, INDIRANAGAR)
    assert link.startswith("https://m.uber.com/looking?")
    assert "pickup=" in link and "drop%5B0%5D=" in link


# -------------------------------------------------------------------- API

@needs_metro
def test_live_api_end_to_end(tmp_path, metro):
    pytest.importorskip("httpx2")
    from fastapi.testclient import TestClient

    from src.server import create_app

    client = TestClient(create_app(db_path=tmp_path / "t.db", maps_client=FakeMaps(), metro=metro))
    status = client.get("/api/live/status").json()
    assert status["maps_configured"] and status["metro_stations"] == 83

    params = {"from_lat": KORAMANGALA["lat"], "from_lng": KORAMANGALA["lng"], "from_name": "Koramangala",
              "to_lat": WHITEFIELD["lat"], "to_lng": WHITEFIELD["lng"], "to_name": "Whitefield"}
    body = client.get("/api/live/plan", params=params).json()
    assert body["origin"]["name"] == "Koramangala" and body["options"]

    history = client.get("/api/live/history").json()
    assert history[0]["destination"]["name"] == "Whitefield"

    outside = {**params, "to_lat": 19.07, "to_lng": 72.87}       # Mumbai
    assert client.get("/api/live/plan", params=outside).status_code == 422

    lines = client.get("/api/live/metro").json()["lines"]
    assert {line["id"] for line in lines} == {"PURPLE", "GREEN", "YELLOW"}


def test_live_api_without_key_explains_setup(tmp_path):
    pytest.importorskip("httpx2")
    from fastapi.testclient import TestClient

    from src.server import create_app

    client = TestClient(create_app(db_path=tmp_path / "t.db", maps_client=None, metro=None))
    response = client.get("/api/live/places", params={"q": "MG Road"})
    assert response.status_code == 503
    assert "OLA_MAPS_API_KEY" in response.json()["detail"]


@needs_metro
def test_metro_trace_replays_the_real_run(metro):
    t = metro.trace(station_id(metro, "Whitefield"), station_id(metro, "Silk Institute"))
    times = [e["t"] for e in t["events"]]
    assert times == sorted(times)                         # pops come out in time order
    assert t["events"][0]["station"] == station_id(metro, "Whitefield")
    assert t["events"][-1]["station"] == station_id(metro, "Silk Institute")
    assert t["route"]["time_min"] == pytest.approx(t["events"][-1]["t"], abs=0.11)
    assert t["fare"]["token"] == metro.token_fare[(t["from"]["id"], t["to"]["id"])]


def test_live_fares_and_trace_endpoints(tmp_path, metro):
    pytest.importorskip("httpx2")
    from fastapi.testclient import TestClient

    from src.server import create_app

    client = TestClient(create_app(db_path=tmp_path / "t.db", maps_client=None, metro=metro))
    rules = client.get("/api/live/fares").json()
    assert {r["mode"] for r in rules} == {"metro", "auto", "cab", "bus", "bike_taxi"}
    assert all(r["source"] for r in rules)
    trace = client.get("/api/live/metro/trace", params={"from": "IDN", "to": "MAGR"}).json()
    assert trace["events"][-1]["station"] == "MAGR"
    assert client.get("/api/live/metro/trace", params={"from": "IDN", "to": "IDN"}).status_code == 422
    assert client.get("/api/live/metro/trace", params={"from": "IDN", "to": "NOPE"}).status_code == 404
