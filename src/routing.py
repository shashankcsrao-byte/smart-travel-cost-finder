"""
Route finding (Dijkstra's algorithm) and multi-mode comparison.

The core algorithm is implemented by hand with Python's `heapq` (a binary
min-heap). No graph library is used to compute routes.
"""

import heapq
import math
from dataclasses import dataclass, field

from .fares import FARE_RULES, calculate_fare, mode_label
from .graph import MODES

# Edge attributes Dijkstra is allowed to minimise.
WEIGHTS = ("distance_km", "time_min")


class UnknownLocationError(ValueError):
    """Raised when a location name is not a vertex of the graph."""


@dataclass
class PathResult:
    """Output of one Dijkstra run: the route and its totals."""

    mode: str
    path: list            # [source, ..., destination]
    distance_km: float
    time_min: float
    edges: list = field(default_factory=list)   # Edge objects used, in order


@dataclass
class JourneyOption:
    """A complete, priced journey for one transport mode."""

    mode: str
    label: str
    path: list
    distance_km: float
    time_min: float
    fare: float
    edges: list = field(default_factory=list)


@dataclass
class Comparison:
    """Result of comparing all modes for one source -> destination query."""

    source: str
    destination: str
    options: list = field(default_factory=list)       # available, sorted by fare
    unavailable: list = field(default_factory=list)   # modes with no route

    @property
    def same_location(self):
        return self.source == self.destination

    @property
    def cheapest(self):
        return self.options[0] if self.options else None

    @property
    def fastest(self):
        if not self.options:
            return None
        return min(self.options, key=lambda o: (o.time_min, o.fare, MODES.index(o.mode)))

    @property
    def tied_cheapest(self):
        """All options whose fare equals the cheapest fare (handles ties)."""
        if not self.options:
            return []
        best = self.options[0].fare
        return [o for o in self.options if math.isclose(o.fare, best)]

    @property
    def tied_fastest(self):
        """All options whose time equals the fastest time (handles ties)."""
        if not self.options:
            return []
        best = self.fastest.time_min
        return [o for o in self.options if math.isclose(o.time_min, best)]


# ---------------------------------------------------------------------------
# Input validation
# ---------------------------------------------------------------------------

def find_location(graph, name):
    """
    Return the exact location name matching `name` (case/space-insensitive).

    "  college " -> "College".  Raises UnknownLocationError if not found.
    """
    if not isinstance(name, str) or not name.strip():
        raise UnknownLocationError("Location name cannot be empty")

    wanted = " ".join(name.split()).lower()
    for location in graph:
        if location.lower() == wanted:
            return location
    raise UnknownLocationError(f"Unknown location: {name.strip()!r}")


def validate_location(graph, name):
    """Raise UnknownLocationError unless `name` is exactly a vertex of the graph."""
    if name not in graph:
        raise UnknownLocationError(f"Unknown location: {name!r}")


# ---------------------------------------------------------------------------
# Dijkstra's algorithm
# ---------------------------------------------------------------------------

def dijkstra(graph, source, destination, mode, weight="distance_km", trace=None):
    """
    Lowest-`weight` route from `source` to `destination` using ONLY `mode` edges.

    weight = "distance_km" (default) -> shortest route, which is also the
             cheapest because fare = base + per_km * distance.
    weight = "time_min"              -> quickest route for that mode.

    Returns a PathResult, or None if the destination is unreachable by `mode`.
    Time: O((V + E) log V) with a binary heap.  Space: O(V + E).

    trace: optional list. If given, one event per settled node is appended:
           {"node", "cost", "heap": [(cost, node), ...4 smallest], "relaxed": [(node, cost), ...]}
           Used by the website to replay the run; it does not change the result.
    """
    validate_location(graph, source)
    validate_location(graph, destination)
    if mode not in MODES:
        raise ValueError(f"Unknown transport mode: {mode!r}")
    if weight not in WEIGHTS:
        raise ValueError(f"weight must be one of {WEIGHTS}")

    # 1. Initialise: source = 0, every other location = infinity.
    dist = {location: math.inf for location in graph}
    dist[source] = 0
    # parent[v] = (u, edge) means "we reached v from u using this edge".
    parent = {}
    visited = set()
    # Min-heap of (tentative_cost, location). Equal costs are ordered by name,
    # which makes results deterministic.
    heap = [(0, source)]

    while heap:
        # 2. Take the unvisited location with the smallest tentative cost.
        cost, u = heapq.heappop(heap)

        # 3. Skip stale entries. heapq cannot "decrease a key", so when we find
        #    a better cost we push a new entry and ignore the old one here.
        if u in visited or cost > dist[u]:
            continue
        visited.add(u)
        if trace is not None:
            event = {"node": u, "cost": cost, "heap": [], "relaxed": []}
            trace.append(event)

        # Once the destination is popped its cost is final -> stop early.
        if u == destination:
            break

        # 4. Relax every edge of the requested mode leaving u.
        for edge in graph[u]:
            if edge.mode != mode:
                continue          # mode filtering: never mix transport modes
            new_cost = cost + getattr(edge, weight)
            if new_cost < dist[edge.to]:
                dist[edge.to] = new_cost
                parent[edge.to] = (u, edge)
                heapq.heappush(heap, (new_cost, edge.to))
                if trace is not None:
                    event["relaxed"].append((edge.to, new_cost))
        if trace is not None:
            # Snapshot of the heap after this node's neighbours were pushed,
            # leaving out stale entries that will be skipped later.
            live = [(c, n) for c, n in heapq.nsmallest(12, heap) if n not in visited and c <= dist[n]]
            event["heap"] = live[:4]

    # 5. Unreachable destination.
    if dist[destination] == math.inf:
        return None

    # 6. Reconstruct the path by walking parents back from the destination,
    #    adding up distance and time of the edges actually used.
    path = [destination]
    used_edges = []
    total_km = 0
    total_min = 0
    node = destination
    while node != source:
        prev, edge = parent[node]
        total_km += edge.distance_km
        total_min += edge.time_min
        used_edges.append(edge)
        path.append(prev)
        node = prev
    path.reverse()
    used_edges.reverse()

    return PathResult(mode=mode, path=path, distance_km=total_km,
                      time_min=total_min, edges=used_edges)


# ---------------------------------------------------------------------------
# Comparison across modes
# ---------------------------------------------------------------------------

def compare_routes(graph, source, destination, modes=MODES, fare_rules=FARE_RULES):
    """
    Find the cheapest route for every mode separately, price it, and rank.

    * Options are sorted by fare, then time, then the order in MODES.
    * Modes that cannot reach the destination go into `unavailable`.
    * If source == destination, no journey is needed: options stay empty.
    """
    validate_location(graph, source)
    validate_location(graph, destination)

    result = Comparison(source=source, destination=destination)
    if source == destination:
        return result

    for mode in modes:
        route = dijkstra(graph, source, destination, mode, weight="distance_km")
        if route is None:
            result.unavailable.append(mode)
            continue
        result.options.append(
            JourneyOption(
                mode=mode,
                label=mode_label(mode),
                path=route.path,
                distance_km=route.distance_km,
                time_min=route.time_min,
                fare=calculate_fare(mode, route.distance_km, fare_rules),
                edges=route.edges,
            )
        )

    result.options.sort(key=lambda o: (o.fare, o.time_min, MODES.index(o.mode)))
    return result
