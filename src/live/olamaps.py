"""
Ola Maps REST client (place search, directions and distance matrix).

Endpoints (https://api.olamaps.io), authenticated with the `api_key` query parameter:
    GET  /places/v1/autocomplete     input, location, radius
    GET  /places/v1/details          place_id
    POST /routing/v1/directions      origin, destination  ("lat,lng")
    GET  /routing/v1/distanceMatrix  origins, destinations ("lat,lng|lat,lng")

Ola Maps returns distances in metres and durations in seconds, and its routing
considers traffic. The parsers below are written defensively because the
response format is not fully documented publicly.
"""

import uuid

import httpx

from .cache import TTLCache
from .settings import CITY_CENTRE

BASE_URL = "https://api.olamaps.io"


class OlaMapsError(RuntimeError):
    """Raised when Ola Maps cannot be reached or returns an unusable answer."""


def _number(value):
    """Accept 1234, "1234", or {"value": 1234} and return a float (or None)."""
    if isinstance(value, dict):
        value = value.get("value")
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _lat_lng(obj):
    """Find a (lat, lng) pair in a place object, whatever its exact shape."""
    if not isinstance(obj, dict):
        return None
    for path in (("geometry", "location"), ("location",), ()):
        node = obj
        for key in path:
            node = node.get(key, {}) if isinstance(node, dict) else {}
        lat = node.get("lat", node.get("latitude")) if isinstance(node, dict) else None
        lng = node.get("lng", node.get("longitude")) if isinstance(node, dict) else None
        if lat is not None and lng is not None:
            return float(lat), float(lng)
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

    # ------------------------------------------------------------------ http
    def _request(self, method, path, params):
        params = {**params, "api_key": self.api_key}
        try:
            response = self.http.request(
                method, path, params=params, headers={"X-Request-Id": str(uuid.uuid4())}
            )
        except httpx.HTTPError as error:
            raise OlaMapsError(f"Could not reach Ola Maps: {error.__class__.__name__}") from None
        if response.status_code in (401, 403):
            raise OlaMapsError("Ola Maps rejected the API key (check OLA_MAPS_API_KEY in .env)")
        if response.status_code == 429:
            raise OlaMapsError("Ola Maps rate limit reached, please wait a moment and try again")
        if response.status_code >= 400:
            raise OlaMapsError(f"Ola Maps error {response.status_code}")
        try:
            return response.json()
        except ValueError:
            raise OlaMapsError("Ola Maps returned a non-JSON response") from None

    # ---------------------------------------------------------------- places
    def autocomplete(self, text, near=CITY_CENTRE, radius_m=60000):
        """Place suggestions: [{name, address, place_id, lat, lng}] (lat/lng may be None)."""
        text = " ".join(text.split())
        key = ("ac", text.lower())
        cached = self.place_cache.get(key)
        if cached is not None:
            return cached

        data = self._request("GET", "/places/v1/autocomplete",
                             {"input": text, "location": _coord(near), "radius": radius_m})
        results = []
        for p in data.get("predictions", []) or []:
            fmt = p.get("structured_formatting") or {}
            point = _lat_lng(p)
            results.append({
                "name": fmt.get("main_text") or p.get("description", "").split(",")[0],
                "address": fmt.get("secondary_text") or p.get("description", ""),
                "place_id": p.get("place_id"),
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
        data = self._request("GET", "/places/v1/details", {"place_id": place_id})
        point = _lat_lng(data.get("result") or data)
        if point is None:
            raise OlaMapsError("Ola Maps did not return a location for this place")
        self.place_cache.set(key, point)
        return point

    # --------------------------------------------------------------- routing
    def directions(self, origin, destination):
        """Fastest road route with live traffic: {distance_km, duration_min, polyline}."""
        key = ("dir", _coord(origin), _coord(destination))
        cached = self.route_cache.get(key)
        if cached is not None:
            return cached

        data = self._request("POST", "/routing/v1/directions",
                             {"origin": _coord(origin), "destination": _coord(destination)})
        routes = data.get("routes") or []
        if not routes:
            raise OlaMapsError("No road route found between these places")
        route = routes[0]
        legs = route.get("legs") or []
        metres = sum(_number(leg.get("distance")) or 0 for leg in legs)
        seconds = sum(_number(leg.get("duration")) or 0 for leg in legs)
        if metres <= 0:
            raise OlaMapsError("Ola Maps returned a route without a distance")
        polyline = route.get("overview_polyline")
        if isinstance(polyline, dict):
            polyline = polyline.get("points")

        result = {
            "distance_km": round(metres / 1000, 2),
            "duration_min": round(seconds / 60, 1),
            "polyline": polyline if isinstance(polyline, str) else None,
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
        matrix = []
        for row in data.get("rows") or []:
            cells = []
            for element in row.get("elements") or []:
                metres = _number(element.get("distance"))
                seconds = _number(element.get("duration"))
                ok = metres is not None and seconds is not None and str(element.get("status", "OK")).upper() == "OK"
                cells.append({"distance_km": round(metres / 1000, 2), "duration_min": round(seconds / 60, 1)} if ok else None)
            matrix.append(cells)
        if len(matrix) != len(origins) or any(len(r) != len(destinations) for r in matrix):
            raise OlaMapsError("Ola Maps distance matrix had an unexpected size")
        self.route_cache.set(key, matrix)
        return matrix
