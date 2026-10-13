"""
Ola Maps REST client (place search, reverse geocoding, directions, distance matrix).

Endpoints (https://api.olamaps.io), authenticated with the `api_key` query parameter:
    GET  /places/v1/autocomplete       input, location, radius
    GET  /places/v1/details            place_id
    GET  /places/v1/reverse-geocode    latlng
    POST /routing/v1/directions        origin, destination  ("lat,lng")
    GET  /routing/v1/distanceMatrix    origins, destinations ("lat,lng|lat,lng")

Ola Maps returns distances in metres and durations in seconds, and its routing
considers traffic. Every response is validated: anything malformed, negative,
NaN or infinite becomes an OlaMapsError instead of a server crash.
"""

import logging
import math
import re
import threading
import uuid
from datetime import date

import httpx

from .cache import TTLCache
from .settings import CITY_CENTRE

BASE_URL = "https://api.olamaps.io"
log = logging.getLogger(__name__)

# httpx logs every request URL at INFO level, and our key is in the URL.
# Keep that logger quiet so the key can never end up in a log file.
logging.getLogger("httpx").setLevel(logging.WARNING)
logging.getLogger("httpcore").setLevel(logging.WARNING)


class OlaMapsError(RuntimeError):
    """Ola Maps could not be reached or returned something unusable (HTTP 502)."""


class OlaNotFound(OlaMapsError):
    """The place or route does not exist (HTTP 404)."""


class OlaRejected(OlaMapsError):
    """Ola Maps refused this particular input (HTTP 422)."""


_ALLOWED = re.compile(r"[^\w\s.,'&/()#:-]", re.UNICODE)


def clean_query(text):
    """Place-search text with odd symbols removed and spaces collapsed.

    "  <script> MG   Road " -> "script MG Road". Firewalls in front of map APIs
    reject angle brackets and similar characters, and no place name needs them.
    """
    return " ".join(_ALLOWED.sub(" ", text or "").split())


def _number(value):
    """Accept 1234, "1234", or {"value": 1234}; return a finite, non-negative float or None."""
    if isinstance(value, dict):
        value = value.get("value")
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) and number >= 0 else None


def _lat_lng(obj):
    """Find a (lat, lng) pair in a place object, whatever its exact shape."""
    if not isinstance(obj, dict):
        return None
    for path in (("geometry", "location"), ("location",), ()):
        node = obj
        for key in path:
            node = node.get(key) if isinstance(node, dict) else None
        if not isinstance(node, dict):
            continue
        lat = node.get("lat", node.get("latitude"))
        lng = node.get("lng", node.get("longitude"))
        try:
            lat, lng = float(lat), float(lng)
        except (TypeError, ValueError):
            continue
        if math.isfinite(lat) and math.isfinite(lng) and -90 <= lat <= 90 and -180 <= lng <= 180:
            return lat, lng
    return None


def _coord(point):
    lat, lng = point
    return f"{lat:.6f},{lng:.6f}"


class OlaMapsClient:
    def __init__(self, api_key, http_client=None, timeout=10.0):
        if not api_key:
            raise OlaMapsError("Ola Maps API key is missing")
        self.api_key = api_key
        self.http = http_client or httpx.Client(base_url=BASE_URL, timeout=timeout)
        # Place search results change rarely; routes use live traffic, so cache briefly.
        self.place_cache = TTLCache(max_items=500, ttl_seconds=6 * 3600)
        self.route_cache = TTLCache(max_items=200, ttl_seconds=60)
        self.miss_cache = TTLCache(max_items=500, ttl_seconds=600)   # known "not found" answers
        self._count_lock = threading.Lock()
        self.calls_total = 0
        self.calls_today = 0
        self._count_day = date.today()

    def close(self):
        self.http.close()

    def usage(self):
        with self._count_lock:
            if self._count_day != date.today():
                self._count_day, self.calls_today = date.today(), 0
            return {"calls_today": self.calls_today, "calls_total": self.calls_total}

    # ------------------------------------------------------------------ http
    def _request(self, method, path, params):
        with self._count_lock:
            if self._count_day != date.today():
                self._count_day, self.calls_today = date.today(), 0
            self.calls_today += 1
            self.calls_total += 1
        try:
            response = self.http.request(
                method, path, params={**params, "api_key": self.api_key},
                headers={"X-Request-Id": str(uuid.uuid4())},
            )
        except httpx.HTTPError as error:
            raise OlaMapsError(f"Could not reach Ola Maps ({error.__class__.__name__})") from None
        status = response.status_code
        if status in (401, 403):
            log.warning("Ola Maps refused %s with HTTP %s (check OLA_MAPS_API_KEY)", path, status)
            raise OlaMapsError("The map service refused the request. The server's map key may be invalid.")
        if status == 404:
            raise OlaNotFound("Not found by the map service")
        if status == 429:
            raise OlaMapsError("The map service is busy. Please wait a moment and try again.")
        if status in (400, 422):
            raise OlaRejected("The map service could not use that input")
        if status >= 400:
            raise OlaMapsError(f"Map service error {status}")
        try:
            data = response.json()
        except ValueError:
            raise OlaMapsError("The map service returned an unreadable answer") from None
        if not isinstance(data, dict):
            raise OlaMapsError("The map service returned an unexpected answer")
        return data

    # ---------------------------------------------------------------- places
    def autocomplete(self, text, near=CITY_CENTRE, radius_m=60000):
        """Place suggestions: [{name, address, place_id, lat, lng}] (lat/lng may be None)."""
        text = clean_query(text)
        if len(text) < 2:
            return []                      # nothing worth spending quota on
        key = ("ac", text.lower())
        cached = self.place_cache.get(key)
        if cached is not None:
            return cached

        data = self._request("GET", "/places/v1/autocomplete",
                             {"input": text, "location": _coord(near), "radius": radius_m})
        results = []
        predictions = data.get("predictions") or []
        if not isinstance(predictions, list):
            raise OlaMapsError("The map service returned an unexpected answer")
        for p in predictions:
            if not isinstance(p, dict):
                continue
            fmt = p.get("structured_formatting") if isinstance(p.get("structured_formatting"), dict) else {}
            description = str(p.get("description") or "")
            point = _lat_lng(p)
            name = str(fmt.get("main_text") or description.split(",")[0]).strip()
            if not name:
                continue
            results.append({
                "name": name[:120],
                "address": str(fmt.get("secondary_text") or description)[:200],
                "place_id": str(p.get("place_id") or "")[:300] or None,
                "lat": point[0] if point else None,
                "lng": point[1] if point else None,
            })
        self.place_cache.set(key, results)
        return results

    def place_location(self, place_id):
        """(lat, lng) of a place_id, for suggestions that came without coordinates."""
        key = ("pd", place_id)
        cached = self.place_cache.get(key)
        if cached is not None:
            return cached
        if self.miss_cache.get(key):
            raise OlaNotFound("Place not found")
        try:
            data = self._request("GET", "/places/v1/details", {"place_id": place_id})
        except OlaRejected:
            self.miss_cache.set(key, True)
            raise OlaNotFound("Place not found") from None
        except OlaNotFound:
            self.miss_cache.set(key, True)
            raise
        point = _lat_lng(data.get("result") if isinstance(data.get("result"), dict) else data)
        if point is None:
            self.miss_cache.set(key, True)
            raise OlaNotFound("Place not found")
        self.place_cache.set(key, point)
        return point

    def reverse_geocode(self, point):
        """A readable name for a map position: {"name", "address"}."""
        key = ("rg", round(point[0], 4), round(point[1], 4))
        cached = self.place_cache.get(key)
        if cached is not None:
            return cached
        data = self._request("GET", "/places/v1/reverse-geocode", {"latlng": _coord(point)})
        results = data.get("results")
        first = results[0] if isinstance(results, list) and results and isinstance(results[0], dict) else {}
        address = str(first.get("formatted_address") or "").strip()
        name = str(first.get("name") or address.split(",")[0]).strip()
        place = {"name": (name or "Pinned place")[:120], "address": address[:200]}
        self.place_cache.set(key, place)
        return place

    # --------------------------------------------------------------- routing
    def directions(self, origin, destination):
        """Fastest road route with live traffic: {distance_km, duration_min, polyline}."""
        key = ("dir", _coord(origin), _coord(destination))
        cached = self.route_cache.get(key)
        if cached is not None:
            return cached

        try:
            data = self._request("POST", "/routing/v1/directions",
                                 {"origin": _coord(origin), "destination": _coord(destination)})
        except OlaRejected:
            raise OlaNotFound("No road route found between these places") from None
        routes = data.get("routes")
        if not isinstance(routes, list) or not routes or not isinstance(routes[0], dict):
            raise OlaNotFound("No road route found between these places")
        route = routes[0]
        legs = route.get("legs") if isinstance(route.get("legs"), list) else []
        legs = [leg for leg in legs if isinstance(leg, dict)]
        metres = [_number(leg.get("distance")) for leg in legs]
        seconds = [_number(leg.get("duration")) for leg in legs]
        if not legs or None in metres or None in seconds or sum(metres) <= 0:
            raise OlaMapsError("The map service returned a route without a usable distance")
        polyline = route.get("overview_polyline")
        if isinstance(polyline, dict):
            polyline = polyline.get("points")

        result = {
            "distance_km": round(sum(metres) / 1000, 2),
            "duration_min": round(sum(seconds) / 60, 1),
            "polyline": polyline if isinstance(polyline, str) and len(polyline) < 200_000 else None,
        }
        self.route_cache.set(key, result)
        return result

    def distance_matrix(self, origins, destinations):
        """Road distance/time for every origin-destination pair.

        Returns a list of rows; each row is a list of {distance_km, duration_min}
        (or None where no route exists).
        """
        key = ("dm", tuple(map(_coord, origins)), tuple(map(_coord, destinations)))
        cached = self.route_cache.get(key)
        if cached is not None:
            return cached

        data = self._request("GET", "/routing/v1/distanceMatrix", {
            "origins": "|".join(map(_coord, origins)),
            "destinations": "|".join(map(_coord, destinations)),
        })
        rows = data.get("rows")
        if not isinstance(rows, list):
            raise OlaMapsError("The map service returned an unexpected distance matrix")
        matrix = []
        for row in rows:
            elements = row.get("elements") if isinstance(row, dict) else None
            if not isinstance(elements, list):
                raise OlaMapsError("The map service returned an unexpected distance matrix")
            cells = []
            for element in elements:
                element = element if isinstance(element, dict) else {}
                metres, seconds = _number(element.get("distance")), _number(element.get("duration"))
                ok = metres is not None and seconds is not None and str(element.get("status", "OK")).upper() == "OK"
                cells.append({"distance_km": round(metres / 1000, 2), "duration_min": round(seconds / 60, 1)} if ok else None)
            matrix.append(cells)
        if len(matrix) != len(origins) or any(len(r) != len(destinations) for r in matrix):
            raise OlaMapsError("The map service returned a distance matrix of the wrong size")
        self.route_cache.set(key, matrix)
        return matrix
