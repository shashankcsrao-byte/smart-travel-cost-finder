"""
Web backend: a FastAPI REST API that also serves the web frontend.

Run from the project folder:
    python -m src.server
Then open http://localhost:8000  (interactive API docs: http://localhost:8000/docs)

Request flow:
    Browser (web/app.js)  --HTTP/JSON-->  this API  -->  routing.py (Dijkstra)
                                                    -->  history.py (SQLite)
"""

import os
from pathlib import Path

from fastapi import FastAPI, HTTPException, Query
from fastapi.staticfiles import StaticFiles

from .fares import FARE_RULES, mode_label
from .graph import CONNECTIONS, LOCATION_COORDS, LOCATIONS, MODES, build_graph
from .history import HistoryStore
from .live.metro import MetroNetwork
from .live.olamaps import OlaMapsClient, OlaMapsError
from .live import fares as live_fares
from .live.planner import plan_trip
from .live.settings import METRO_GTFS_ZIP, ola_maps_api_key
from .routing import UnknownLocationError, compare_routes, find_location

PROJECT_ROOT = Path(__file__).resolve().parent.parent
WEB_DIR = PROJECT_ROOT / "web"
DEFAULT_DB = PROJECT_ROOT / "travel_history.db"


def option_to_dict(option, rank, cheapest_modes, fastest_modes):
    """Convert a JourneyOption into JSON-friendly data for the frontend."""
    rule = FARE_RULES[option.mode]
    return {
        "rank": rank,
        "mode": option.mode,
        "label": option.label,
        "path": option.path,
        "distance_km": option.distance_km,
        "time_min": option.time_min,
        "fare": option.fare,
        "fare_breakdown": {"base": rule["base"], "per_km": rule["per_km"]},
        "is_cheapest": option.mode in cheapest_modes,
        "is_fastest": option.mode in fastest_modes,
        "legs": [
            {"from": start, "to": edge.to, "distance_km": edge.distance_km, "time_min": edge.time_min}
            for start, edge in zip(option.path, option.edges)
        ],
    }


def comparison_to_dict(comparison):
    cheapest_modes = {o.mode for o in comparison.tied_cheapest}
    fastest_modes = {o.mode for o in comparison.tied_fastest}
    return {
        "source": comparison.source,
        "destination": comparison.destination,
        "same_location": comparison.same_location,
        "options": [
            option_to_dict(o, rank, cheapest_modes, fastest_modes)
            for rank, o in enumerate(comparison.options, start=1)
        ],
        "unavailable": [{"mode": m, "label": mode_label(m)} for m in comparison.unavailable],
        "cheapest_mode": comparison.cheapest.mode if comparison.cheapest else None,
        "fastest_mode": comparison.fastest.mode if comparison.fastest else None,
        "notes": [
            "Sample city data and illustrative fares, not live or official values.",
            "Journey time is the sum of connection times; waiting, walking and traffic are not included.",
        ],
    }


# Live mode only covers the Bengaluru region (metro data and fare rules are local).
BENGALURU_BOUNDS = {"lat": (12.70, 13.30), "lng": (77.30, 77.90)}


def _default_maps_client():
    key = ola_maps_api_key()
    return OlaMapsClient(key) if key else None


def _default_metro():
    return MetroNetwork(METRO_GTFS_ZIP) if METRO_GTFS_ZIP.exists() else None


def create_app(db_path=None, maps_client="default", metro="default"):
    """Build the FastAPI app. Tests pass a temporary db_path and fake live services."""
    graph = build_graph()
    history = HistoryStore(db_path or os.environ.get("TRAVEL_DB_PATH", DEFAULT_DB))
    maps = _default_maps_client() if maps_client == "default" else maps_client
    metro_net = _default_metro() if metro == "default" else metro

    app = FastAPI(
        title="Smart Travel Cost Finder API",
        description="Cheapest route per transport mode using a hand-written Dijkstra's algorithm. "
                    "All data is illustrative sample data.",
        version="1.0.0",
    )

    @app.get("/api/health", tags=["system"])
    def health():
        return {"status": "ok", "locations": len(graph), "connections": len(CONNECTIONS)}

    @app.get("/api/locations", tags=["network"])
    def get_locations():
        """All locations (graph vertices) with map coordinates."""
        return [
            {"name": name, "x": LOCATION_COORDS[name][0], "y": LOCATION_COORDS[name][1]}
            for name in LOCATIONS
        ]

    @app.get("/api/fares", tags=["network"])
    def get_fares():
        """Fare rules per mode (illustrative values)."""
        return [{"mode": m, **FARE_RULES[m]} for m in MODES]

    @app.get("/api/network", tags=["network"])
    def get_network():
        """Every two-way connection in the sample city."""
        return [
            {"from": a, "to": b, "mode": mode, "label": mode_label(mode),
             "distance_km": km, "time_min": minutes}
            for a, b, mode, km, minutes in CONNECTIONS
        ]

    @app.get("/api/routes", tags=["routing"])
    def get_routes(
        source: str = Query(..., min_length=1, max_length=60, examples=["College"]),
        destination: str = Query(..., min_length=1, max_length=60, examples=["Airport"]),
    ):
        """Cheapest route for each mode, ranked by fare."""
        try:
            src = find_location(graph, source)
            dst = find_location(graph, destination)
        except UnknownLocationError as error:
            raise HTTPException(status_code=404, detail=str(error)) from None

        comparison = compare_routes(graph, src, dst)
        if not comparison.same_location:
            cheapest = comparison.cheapest
            history.add(src, dst,
                        cheapest.mode if cheapest else None,
                        cheapest.fare if cheapest else None)
        return comparison_to_dict(comparison)

    @app.get("/api/history", tags=["history"])
    def get_history(limit: int = Query(8, ge=1, le=50)):
        """Most recent searches and the most searched journeys."""
        return {"recent": history.recent(limit), "popular": history.popular(5)}

    @app.delete("/api/history", tags=["history"])
    def clear_history():
        history.clear()
        return {"cleared": True}

    # ------------------------------------------------------------ live mode
    def require_maps():
        if maps is None:
            raise HTTPException(
                status_code=503,
                detail="Live mode needs an Ola Maps API key. Add OLA_MAPS_API_KEY to the .env file "
                       "in the project folder and restart the server.",
            )
        return maps

    def check_point(name, lat, lng):
        if not (BENGALURU_BOUNDS["lat"][0] <= lat <= BENGALURU_BOUNDS["lat"][1]
                and BENGALURU_BOUNDS["lng"][0] <= lng <= BENGALURU_BOUNDS["lng"][1]):
            raise HTTPException(status_code=422, detail=f"{name} is outside Bengaluru. Live mode covers Bengaluru only.")

    @app.get("/api/live/status", tags=["live"])
    def live_status():
        return {
            "maps_configured": maps is not None,
            "metro_loaded": metro_net is not None,
            "metro_stations": len(metro_net.stations) if metro_net else 0,
            "metro_lines": len(metro_net.lines) if metro_net else 0,
            "metro_states": len(metro_net.graph) if metro_net else 0,
            "demo_locations": len(graph),
            "demo_connections": len(CONNECTIONS),
            "sources": [
                "Places, road distance and live traffic times: Ola Maps",
                "Namma Metro stations, timetable and official fares: BMRCL via github.com/Vonter/bmrcl-gtfs (ODbL)",
                "Auto and cab fares: Govt. of Karnataka notified rates",
                "BMTC fares: first stages of the Jan 2025 revision",
            ],
        }

    @app.get("/api/live/places", tags=["live"])
    def live_places(q: str = Query(..., min_length=2, max_length=100)):
        """Place suggestions as the user types (proxied so the API key stays secret)."""
        try:
            return require_maps().autocomplete(q)
        except OlaMapsError as error:
            raise HTTPException(status_code=502, detail=str(error)) from None

    @app.get("/api/live/place", tags=["live"])
    def live_place(place_id: str = Query(..., min_length=1, max_length=300)):
        try:
            lat, lng = require_maps().place_location(place_id)
        except OlaMapsError as error:
            raise HTTPException(status_code=502, detail=str(error)) from None
        return {"lat": lat, "lng": lng}

    @app.get("/api/live/plan", tags=["live"])
    def live_plan(
        from_lat: float = Query(...), from_lng: float = Query(...),
        to_lat: float = Query(...), to_lng: float = Query(...),
        from_name: str = Query("Start", max_length=120), to_name: str = Query("Destination", max_length=120),
    ):
        """Compare auto, cab, bike taxi, bus and metro between two real places."""
        client = require_maps()
        check_point("The start", from_lat, from_lng)
        check_point("The destination", to_lat, to_lng)
        origin = {"name": from_name.strip() or "Start", "lat": from_lat, "lng": from_lng}
        destination = {"name": to_name.strip() or "Destination", "lat": to_lat, "lng": to_lng}
        if abs(from_lat - to_lat) < 1e-4 and abs(from_lng - to_lng) < 1e-4:
            raise HTTPException(status_code=422, detail="The start and destination are the same place.")
        try:
            result = plan_trip(client, metro_net, origin, destination)
        except OlaMapsError as error:
            raise HTTPException(status_code=502, detail=str(error)) from None
        history.add_live(origin, destination, result["cheapest_mode"], result["options"][0]["fare"])
        return result

    @app.get("/api/live/metro", tags=["live"])
    def live_metro():
        """Metro stations and lines, for drawing on the map."""
        if metro_net is None:
            return {"stations": [], "lines": []}
        return {
            "stations": list(metro_net.stations.values()),
            "lines": [
                {**line, "coords": [[metro_net.stations[s]["lat"], metro_net.stations[s]["lng"]] for s in line["stations"]]}
                for line in metro_net.lines.values()
            ],
        }

    @app.get("/api/live/metro/trace", tags=["live"])
    def live_metro_trace(
        from_station: str = Query(..., alias="from", min_length=1, max_length=12),
        to_station: str = Query(..., alias="to", min_length=1, max_length=12),
    ):
        """Every step of our Dijkstra on the real metro graph, for the website replay."""
        if metro_net is None:
            raise HTTPException(status_code=503, detail="Metro data is not loaded")
        if from_station == to_station:
            raise HTTPException(status_code=422, detail="Pick two different stations")
        try:
            return metro_net.trace(from_station, to_station)
        except KeyError:
            raise HTTPException(status_code=404, detail="Unknown station") from None

    @app.get("/api/live/fares", tags=["live"])
    def live_fare_rules():
        """Live-mode fare rules with their sources, plus a worked 10 km example."""
        bus10, _ = live_fares.bmtc_fare(10)
        return [
            {"mode": "metro", "label": "Namma Metro", "kind": "official",
             "rule": "Fixed fare for each pair of stations (BMRCL table)",
             "example": "Indiranagar to MG Road: " + (
                 f"₹{metro_net.token_fare.get(('IDN', 'MAGR'), 0):g}" if metro_net else "n/a"),
             "source": "Official BMRCL fare table in the Namma Metro GTFS feed"},
            {"mode": "auto", "label": "Auto", "kind": "regulated",
             "rule": "₹36 for the first 2 km, then ₹18 per km. +50% from 10 PM to 5 AM",
             "example": f"10 km by day: ₹{live_fares.auto_fare(10, 12)[0]}",
             "source": live_fares.AUTO_METER["source"]},
            {"mode": "cab", "label": "Cab", "kind": "regulated",
             "rule": "₹100 for the first 4 km, then ₹24 per km",
             "example": f"10 km: ₹{live_fares.cab_fare(10)}",
             "source": live_fares.CAB["source"]},
            {"mode": "bus", "label": "BMTC bus", "kind": "estimated",
             "rule": "₹6, ₹12, ₹18 up to 6 km. Longer trips estimated (2020 slabs × 1.2)",
             "example": f"10 km: about ₹{bus10}",
             "source": live_fares.BMTC_ORDINARY["source"]},
            {"mode": "bike_taxi", "label": "Bike taxi", "kind": "indicative",
             "rule": "₹20 + ₹8 per km (no official rate card)",
             "example": f"10 km: ₹{live_fares.bike_fare(10)}",
             "source": live_fares.BIKE_TAXI["source"]},
        ]

    @app.get("/api/live/history", tags=["live"])
    def live_history(limit: int = Query(6, ge=1, le=30)):
        return history.recent_live(limit)

    # The frontend (HTML/CSS/JS). Mounted last so /api/* routes match first.
    app.mount("/", StaticFiles(directory=WEB_DIR, html=True), name="web")
    return app


if __name__ == "__main__":
    import uvicorn

    port = int(os.environ.get("PORT", 8000))
    print(f"Smart Travel Cost Finder running at http://localhost:{port}  (API docs: /docs)")
    uvicorn.run("src.server:create_app", factory=True, host="127.0.0.1", port=port)
