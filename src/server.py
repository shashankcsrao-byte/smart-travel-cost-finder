"""
Web backend: a FastAPI REST API that also serves the web frontend.

Run from the project folder:
    python -m src.server
Then open http://localhost:8000  (interactive API docs: http://localhost:8000/docs)

Request flow:
    Browser (web/app.js)  --HTTP/JSON-->  this API  -->  routing.py (Dijkstra, A*, Yen, mixed)
                                                    -->  live/ (Ola Maps, metro timetable, fares)
                                                    -->  history.py (SQLite)
"""

import logging
import math
import os
from contextlib import asynccontextmanager
from datetime import datetime, timedelta
from pathlib import Path
from urllib.parse import urlparse

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from starlette.middleware.trustedhost import TrustedHostMiddleware

from .fares import FARE_RULES, mode_label
from .graph import CONNECTIONS, LOCATION_COORDS, LOCATIONS, MODES, build_graph
from .history import HistoryStore
from .live import fares as live_fares
from .live.metro import IST, MetroNetwork
from .live.olamaps import OlaMapsClient, OlaMapsError, OlaNotFound, OlaRejected
from .live.planner import plan_trip
from .live.ratelimit import SlidingWindowLimiter
from .live.settings import METRO_GTFS_ZIP, ola_maps_api_key
from .routing import (PRIORITIES, UnknownLocationError, compare_routes, find_location,
                      k_shortest_paths)

PROJECT_ROOT = Path(__file__).resolve().parent.parent
WEB_DIR = PROJECT_ROOT / "web"
DEFAULT_DB = PROJECT_ROOT / "travel_history.db"
log = logging.getLogger("smart_travel")

# Live mode only covers the Bengaluru region (metro data and fare rules are local).
BENGALURU_BOUNDS = {"lat": (12.70, 13.30), "lng": (77.30, 77.90)}

# Requests per minute per client IP for the endpoints that spend Ola Maps quota.
RATE_LIMITS = {"/api/live/places": 90, "/api/live/place": 60, "/api/live/plan": 30, "/api/live/reverse": 30}

CSP = ("default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
       "img-src 'self' data: https://tile.openstreetmap.org https://*.tile.openstreetmap.org; "
       "font-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'")
DOC_PATHS = ("/docs", "/redoc", "/openapi.json")


# --------------------------------------------------------------- JSON shapes

def _legs_of(path, edges):
    return [{"from": a, "to": e.to, "mode": e.mode, "distance_km": e.distance_km, "time_min": e.time_min}
            for a, e in zip(path, edges)]


def option_to_dict(option, rank, cheapest_id, fastest_id):
    """Convert a JourneyOption into JSON-friendly data for the frontend."""
    data = {
        "rank": rank,
        "id": option.id,
        "mode": option.mode,
        "variant": option.variant,
        "label": option.label,
        "path": option.path,
        "distance_km": option.distance_km,
        "time_min": option.time_min,
        "fare": option.fare,
        "is_cheapest": option.id == cheapest_id,
        "is_fastest": option.id == fastest_id,
        "legs": _legs_of(option.path, option.edges),
    }
    if option.mode in FARE_RULES:
        rule = FARE_RULES[option.mode]
        data["fare_breakdown"] = {"base": rule["base"], "per_km": rule["per_km"]}
    if option.legs:
        data["parts"] = [{"mode": leg["mode"], "label": leg["label"], "path": leg["path"],
                          "distance_km": leg["distance_km"], "time_min": leg["time_min"], "fare": leg["fare"]}
                         for leg in option.legs]
    return data


def comparison_to_dict(comparison):
    cheapest, fastest = comparison.cheapest, comparison.fastest
    return {
        "source": comparison.source,
        "destination": comparison.destination,
        "same_location": comparison.same_location,
        "priority": comparison.priority,
        "options": [
            option_to_dict(o, rank, cheapest.id if cheapest else None, fastest.id if fastest else None)
            for rank, o in enumerate(comparison.options, start=1)
        ],
        "unavailable": [{"mode": m, "label": mode_label(m)} for m in comparison.unavailable],
        "cheapest_mode": cheapest.id if cheapest else None,
        "fastest_mode": fastest.id if fastest else None,
        "notes": [
            "Sample city data and illustrative fares, not live or official values.",
            "Journey time is the sum of connection times; waiting and traffic are not included.",
        ],
    }


# ------------------------------------------------------------------- helpers

def _default_maps_client():
    key = ola_maps_api_key()
    return OlaMapsClient(key) if key else None


def _default_metro():
    return MetroNetwork(METRO_GTFS_ZIP) if METRO_GTFS_ZIP.exists() else None


def _ola_http_error(error):
    """Map map-service failures to honest HTTP status codes."""
    if isinstance(error, OlaNotFound):
        return HTTPException(status_code=404, detail=str(error))
    if isinstance(error, OlaRejected):
        return HTTPException(status_code=422, detail=str(error))
    return HTTPException(status_code=502, detail=str(error))


def parse_depart(text, now=None):
    """'HH:MM' (today, or tomorrow if that time has already passed by over an hour)
    or 'YYYY-MM-DDTHH:MM'. Returns an aware datetime in IST."""
    now = (now or datetime.now(IST)).astimezone(IST)
    if not text:
        return now
    text = text.strip()
    try:
        if "T" in text:
            when = datetime.fromisoformat(text)
            return when.replace(tzinfo=IST) if when.tzinfo is None else when.astimezone(IST)
        hh, mm = (int(x) for x in text.split(":"))
        when = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
    except (ValueError, TypeError):
        raise HTTPException(status_code=422, detail="Departure time must look like 18:30") from None
    if when < now - timedelta(hours=1):
        when += timedelta(days=1)
    return when


def create_app(db_path=None, maps_client="default", metro="default", allowed_hosts=None):
    """Build the FastAPI app. Tests pass a temporary db_path and fake live services."""
    graph = build_graph()
    history = HistoryStore(db_path or os.environ.get("TRAVEL_DB_PATH", DEFAULT_DB))
    maps = _default_maps_client() if maps_client == "default" else maps_client
    metro_net = _default_metro() if metro == "default" else metro
    hosts = allowed_hosts or [h.strip() for h in os.environ.get(
        "ALLOWED_HOSTS", "localhost,127.0.0.1,[::1],testserver").split(",") if h.strip()]
    if os.environ.get("RENDER_EXTERNAL_HOSTNAME"):          # e.g. smart-travel.onrender.com on Render
        hosts.append(os.environ["RENDER_EXTERNAL_HOSTNAME"])
    limiters = {path: SlidingWindowLimiter(n, 60) for path, n in RATE_LIMITS.items()}

    @asynccontextmanager
    async def lifespan(_app):
        yield
        if maps is not None and hasattr(maps, "close"):
            maps.close()                       # release the HTTP connection pool

    app = FastAPI(
        title="Smart Travel Cost Finder API",
        description="Route and fare comparison with hand-written Dijkstra, A*, Yen's k-shortest paths, "
                    "a mixed-mode state graph and a time-dependent metro timetable search.",
        version="2.0.0",
        lifespan=lifespan,
    )

    # ------------------------------------------------------------ security
    # Only answer to our own host names: blocks DNS-rebinding pages.
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=hosts)

    @app.middleware("http")
    async def guard(request: Request, call_next):
        path = request.url.path
        if path.startswith("/api/"):
            # Other websites must not be able to make this server spend quota or edit data.
            site = request.headers.get("sec-fetch-site", "")
            origin = request.headers.get("origin")
            if site == "cross-site" or (origin and urlparse(origin).hostname not in
                                        {h.strip("[]") for h in hosts}):
                return JSONResponse({"detail": "Cross-site requests are not allowed"}, status_code=403)
            if request.method not in ("GET", "HEAD", "OPTIONS") and \
                    request.headers.get("x-requested-with") != "smart-travel":
                return JSONResponse({"detail": "Missing X-Requested-With header"}, status_code=403)
            limiter = limiters.get(path)
            client = request.client.host if request.client else "unknown"
            if limiter and not limiter.allow(client):
                return JSONResponse({"detail": "Too many requests. Please wait a moment."}, status_code=429,
                                    headers={"Retry-After": str(limiter.retry_after(client))})
        response = await call_next(request)
        response.headers.setdefault("X-Content-Type-Options", "nosniff")
        response.headers.setdefault("Referrer-Policy", "strict-origin-when-cross-origin")
        response.headers.setdefault("X-Frame-Options", "DENY")
        response.headers.setdefault("Permissions-Policy", "geolocation=(self), camera=(), microphone=()")
        if path.startswith("/api/"):
            response.headers["Cache-Control"] = "no-store"
        elif not path.startswith(DOC_PATHS):
            response.headers["Cache-Control"] = "no-cache"      # always revalidate (ETag)
            response.headers.setdefault("Content-Security-Policy", CSP)
        return response

    # ---------------------------------------------------------------- demo
    @app.get("/api/health", tags=["system"])
    def health():
        return {"status": "ok", "locations": len(graph), "connections": len(CONNECTIONS),
                "maps_configured": maps is not None, "metro_loaded": metro_net is not None,
                "metro_feed_days_left": metro_net.feed_days_left() if metro_net else None}

    @app.get("/api/locations", tags=["network"])
    def get_locations():
        """All locations (graph vertices) with map coordinates."""
        return [{"name": name, "x": LOCATION_COORDS[name][0], "y": LOCATION_COORDS[name][1]}
                for name in LOCATIONS]

    @app.get("/api/fares", tags=["network"])
    def get_fares():
        """Fare rules per mode (illustrative values)."""
        return [{"mode": m, **FARE_RULES[m]} for m in MODES]

    @app.get("/api/network", tags=["network"])
    def get_network():
        """Every two-way connection in the sample city."""
        return [{"from": a, "to": b, "mode": mode, "label": mode_label(mode),
                 "distance_km": km, "time_min": minutes}
                for a, b, mode, km, minutes in CONNECTIONS]

    def resolve(name, label):
        if not name or not name.strip():
            raise HTTPException(status_code=422, detail=f"{label} cannot be empty")
        try:
            return find_location(graph, name)
        except UnknownLocationError as error:
            raise HTTPException(status_code=404, detail=str(error)) from None

    @app.get("/api/routes", tags=["routing"])
    def get_routes(
        source: str = Query(..., min_length=1, max_length=60, examples=["College"]),
        destination: str = Query(..., min_length=1, max_length=60, examples=["Airport"]),
        priority: str = Query("cheapest", pattern="^(cheapest|balanced|fastest)$"),
    ):
        """Every mode priced and ranked by priority, plus quicker routes and a Smart mix."""
        src, dst = resolve(source, "Source"), resolve(destination, "Destination")
        comparison = compare_routes(graph, src, dst, priority=priority)
        if not comparison.same_location:
            cheapest = comparison.cheapest
            history.add(src, dst, cheapest.id if cheapest else None, cheapest.fare if cheapest else None)
        return comparison_to_dict(comparison)

    @app.get("/api/routes/alternatives", tags=["routing"])
    def get_alternatives(
        source: str = Query(..., min_length=1, max_length=60),
        destination: str = Query(..., min_length=1, max_length=60),
        mode: str = Query(..., pattern="^(bus|metro|bike_taxi|car_taxi)$"),
        k: int = Query(3, ge=1, le=5),
    ):
        """The k best loopless routes for one mode (Yen's algorithm)."""
        src, dst = resolve(source, "Source"), resolve(destination, "Destination")
        if src == dst:
            return []
        from .fares import calculate_fare
        return [{"rank": i, "path": r.path, "distance_km": r.distance_km, "time_min": r.time_min,
                 "fare": calculate_fare(mode, r.distance_km)}
                for i, r in enumerate(k_shortest_paths(graph, src, dst, mode, k=k), start=1)]

    @app.get("/api/history", tags=["history"])
    def get_history(limit: int = Query(8, ge=1, le=50)):
        """Most recent searches and the most searched journeys."""
        return {"recent": history.recent(limit), "popular": history.popular(5)}

    @app.delete("/api/history", tags=["history"])
    def clear_history():
        """Clears all history. Needs the header X-Requested-With: smart-travel."""
        history.clear()
        return {"cleared": True}

    # ------------------------------------------------------------ live mode
    def require_maps():
        if maps is None:
            log.warning("Live request without OLA_MAPS_API_KEY configured")
            raise HTTPException(status_code=503, detail="Live mode is not set up on this server yet.")
        return maps

    def check_point(name, lat, lng):
        if not (math.isfinite(lat) and math.isfinite(lng)):
            raise HTTPException(status_code=422, detail=f"{name}: coordinates must be real numbers.")
        in_box = lambda a, b: (BENGALURU_BOUNDS["lat"][0] <= a <= BENGALURU_BOUNDS["lat"][1]
                               and BENGALURU_BOUNDS["lng"][0] <= b <= BENGALURU_BOUNDS["lng"][1])
        if not in_box(lat, lng):
            hint = " (latitude and longitude look swapped)" if in_box(lng, lat) else ""
            raise HTTPException(status_code=422,
                                detail=f"{name} is outside Bengaluru{hint}. Live mode covers Bengaluru only.")

    @app.get("/api/live/status", tags=["live"])
    def live_status():
        days_left = metro_net.feed_days_left() if metro_net else None
        warnings = []
        if days_left is not None and days_left < 30:
            warnings.append(f"Metro timetable data expires in {days_left} days. Download a newer feed.")
        return {
            "maps_configured": maps is not None,
            "metro_loaded": metro_net is not None,
            "metro_stations": len(metro_net.stations) if metro_net else 0,
            "metro_lines": len(metro_net.lines) if metro_net else 0,
            "metro_states": len(metro_net.graph) if metro_net else 0,
            "metro_feed_end": metro_net.feed_end.isoformat() if metro_net and metro_net.feed_end else None,
            "metro_feed_days_left": days_left,
            "demo_locations": len(graph),
            "demo_connections": len(CONNECTIONS),
            "map_calls": maps.usage() if maps is not None and hasattr(maps, "usage") else None,
            "rate_limits_per_minute": RATE_LIMITS,
            "warnings": warnings,
            "sources": [
                "Places, road distance and live traffic times: Ola Maps",
                "Namma Metro stations, timetable and official fares: BMRCL via github.com/Vonter/bmrcl-gtfs (ODbL)",
                "Auto and cab fares: Govt. of Karnataka notified rates",
                "BMTC fares: first stages of the Jan 2025 revision, longer trips estimated",
            ],
        }

    @app.get("/api/live/places", tags=["live"])
    def live_places(q: str = Query(..., min_length=1, max_length=100)):
        """Place suggestions as the user types (proxied so the API key stays secret)."""
        client = require_maps()
        try:
            return client.autocomplete(q)          # short or symbol-only text returns [] with no call
        except OlaMapsError as error:
            raise _ola_http_error(error) from None

    @app.get("/api/live/place", tags=["live"])
    def live_place(place_id: str = Query(..., min_length=1, max_length=300)):
        client = require_maps()
        try:
            lat, lng = client.place_location(place_id)
        except OlaMapsError as error:
            raise _ola_http_error(error) from None
        return {"lat": lat, "lng": lng}

    @app.get("/api/live/reverse", tags=["live"])
    def live_reverse(lat: float = Query(...), lng: float = Query(...)):
        """Name for a point picked on the map."""
        client = require_maps()
        check_point("That point", lat, lng)
        try:
            place = client.reverse_geocode((lat, lng))
        except OlaMapsError as error:
            raise _ola_http_error(error) from None
        return {**place, "lat": lat, "lng": lng}

    @app.get("/api/live/plan", tags=["live"])
    def live_plan(
        from_lat: float = Query(...), from_lng: float = Query(...),
        to_lat: float = Query(...), to_lng: float = Query(...),
        from_name: str = Query("Start", max_length=120), to_name: str = Query("Destination", max_length=120),
        priority: str = Query("cheapest", pattern="^(cheapest|balanced|fastest)$"),
        at: str = Query(None, max_length=20, description="Leave at: HH:MM (IST) or YYYY-MM-DDTHH:MM"),
    ):
        """Compare walk, auto, cab, bike taxi, bus and metro between two real places."""
        client = require_maps()
        check_point("The start", from_lat, from_lng)
        check_point("The destination", to_lat, to_lng)
        if abs(from_lat - to_lat) < 1e-4 and abs(from_lng - to_lng) < 1e-4:
            raise HTTPException(status_code=422, detail="The start and destination are the same place.")
        origin = {"name": " ".join(from_name.split()) or "Start", "lat": from_lat, "lng": from_lng}
        destination = {"name": " ".join(to_name.split()) or "Destination", "lat": to_lat, "lng": to_lng}
        depart = parse_depart(at)
        try:
            result = plan_trip(client, metro_net, origin, destination, now=depart, priority=priority)
        except OlaMapsError as error:
            raise _ola_http_error(error) from None
        cheapest = next(o for o in result["options"] if o["is_cheapest"])
        history.add_live(origin, destination, cheapest["mode"], cheapest["fare"])
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
        at: str = Query(None, max_length=20),
        algorithm: str = Query("dijkstra", pattern="^(dijkstra|astar)$"),
    ):
        """Every step of our search on the real metro timetable, for the website replay."""
        if metro_net is None:
            raise HTTPException(status_code=503, detail="Metro data is not loaded")
        a, b = from_station.strip().upper(), to_station.strip().upper()
        if a not in metro_net.stations or b not in metro_net.stations:
            raise HTTPException(status_code=404, detail="Unknown station")
        if a == b:
            raise HTTPException(status_code=422, detail="Pick two different stations")
        return metro_net.trace(a, b, when=parse_depart(at), algorithm=algorithm)

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
    host = os.environ.get("HOST", "127.0.0.1")        # 0.0.0.0 on a server
    print(f"Smart Travel Cost Finder running at http://localhost:{port}  (API docs: /docs)")
    uvicorn.run("src.server:create_app", factory=True, host=host, port=port,
                proxy_headers=True, forwarded_allow_ips=os.environ.get("FORWARDED_ALLOW_IPS", "127.0.0.1"))
