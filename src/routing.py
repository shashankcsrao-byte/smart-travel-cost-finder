"""
Route finding and multi-mode comparison.

Everything here is implemented by hand on Python's `heapq` (a binary min-heap).
No graph library is used to compute routes.

    dijkstra()           lowest-cost path for ONE mode (cost = distance or time)
    astar()              same answer as Dijkstra, guided by a heuristic, fewer pops
    k_shortest_paths()   Yen's algorithm: the k best loopless paths for one mode
    mixed_route()        Dijkstra on (location, mode) states: may change mode,
                         pays a boarding fare each time, values time at ₹/min
    compare_routes()     everything above combined into a ranked comparison
"""

import heapq
import math
from dataclasses import dataclass, field

from .fares import FARE_RULES, calculate_fare, mode_label
from .graph import MODES

# Edge attributes the search is allowed to minimise.
WEIGHTS = ("distance_km", "time_min")

# How the visitor's priority turns time into money (₹ per minute) for ranking and
# for the mixed-mode search. "cheapest" ignores time; "fastest" makes time dominate.
PRIORITIES = ("cheapest", "balanced", "fastest")
MINUTE_VALUE = {"cheapest": 0.0, "balanced": 2.0, "fastest": 50.0}
TRANSFER_MIN = 5.0        # walking + waiting when changing mode in the mixed search
INF = (math.inf, math.inf)


class UnknownLocationError(ValueError):
    """Raised when a location name is not a vertex of the graph."""


@dataclass
class PathResult:
    """Output of one search: the route and its totals."""

    mode: str
    path: list            # [source, ..., destination]
    distance_km: float
    time_min: float
    edges: list = field(default_factory=list)   # Edge objects used, in order
    pops: int = 0                               # nodes settled (for Dijkstra vs A*)


@dataclass
class JourneyOption:
    """A complete, priced journey. One row in the comparison."""

    mode: str
    label: str
    path: list
    distance_km: float
    time_min: float
    fare: float
    edges: list = field(default_factory=list)
    variant: str = "cheapest"      # "cheapest" | "fastest" | "mixed"
    legs: list = field(default_factory=list)  # mixed only: one entry per mode used

    @property
    def id(self):
        if self.variant == "mixed":
            return "mixed"
        return self.mode if self.variant == "cheapest" else f"{self.mode}_{self.variant}"


def _order(option):
    """Fixed tie-break order: the order of MODES, mixed journeys last."""
    return MODES.index(option.mode) if option.mode in MODES else len(MODES)


def rank_key(option, priority="cheapest"):
    """Sort key for a priority. Ties fall back to the other measure, then mode order."""
    if priority == "fastest":
        return (option.time_min, option.fare, _order(option))
    if priority == "balanced":
        score = option.fare + MINUTE_VALUE["balanced"] * option.time_min
        return (round(score, 6), option.fare, _order(option))
    return (option.fare, option.time_min, _order(option))


@dataclass
class Comparison:
    """Result of comparing all modes for one source -> destination query."""

    source: str
    destination: str
    options: list = field(default_factory=list)       # available, ranked by `priority`
    unavailable: list = field(default_factory=list)   # modes with no route
    priority: str = "cheapest"

    @property
    def same_location(self):
        return self.source == self.destination

    @property
    def cheapest(self):
        """Exactly one option: lowest fare, then quicker, then mode order."""
        return min(self.options, key=lambda o: rank_key(o, "cheapest")) if self.options else None

    @property
    def fastest(self):
        """Exactly one option: quickest, then cheaper, then mode order."""
        return min(self.options, key=lambda o: rank_key(o, "fastest")) if self.options else None

    @property
    def tied_cheapest(self):
        """Every option with the cheapest fare (to say "same fare also by ...")."""
        if not self.options:
            return []
        best = self.cheapest.fare
        return [o for o in self.options if math.isclose(o.fare, best)]

    @property
    def tied_fastest(self):
        """Kept for compatibility. The badge itself goes to `fastest` only."""
        return [self.fastest] if self.options else []


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
    shown = "".join(ch for ch in name.strip() if ch.isprintable())[:60]
    raise UnknownLocationError(f"Unknown location: {shown}")


def validate_location(graph, name):
    """Raise UnknownLocationError unless `name` is exactly a vertex of the graph."""
    if name not in graph:
        raise UnknownLocationError(f"Unknown location: {name}")


# ---------------------------------------------------------------------------
# Dijkstra and A* (one shared search)
# ---------------------------------------------------------------------------

def _edge_cost(edge, weight):
    """Cost as a pair: the measure we minimise, then the other one as tie-break.

    Minimising (distance, time) means: among equally short routes, take the
    quicker one. Tuples compare element by element, so the heap needs no change.
    """
    if weight == "time_min":
        return (edge.time_min, edge.distance_km)
    return (edge.distance_km, edge.time_min)


def _search(graph, source, destination, mode, weight, trace=None, heuristic=None,
            banned_edges=frozenset(), banned_nodes=frozenset()):
    validate_location(graph, source)
    validate_location(graph, destination)
    if mode not in MODES:
        raise ValueError(f"Unknown transport mode: {mode!r}")
    if weight not in WEIGHTS:
        raise ValueError(f"weight must be one of {WEIGHTS}")

    h = heuristic or (lambda node: 0.0)

    def priority(node, cost):
        # Dijkstra: h = 0, so priority == cost. A*: cost so far + estimate to go.
        return (cost[0] + h(node), cost[1])

    # 1. Initialise: source = 0, every other location = infinity.
    dist = {location: INF for location in graph}
    dist[source] = (0.0, 0.0)
    parent = {}            # parent[v] = (u, edge): best known way to reach v
    visited = set()
    heap = [(priority(source, dist[source]), source)]
    pops = 0

    while heap:
        # 2. Take the unvisited location with the smallest priority.
        prio, u = heapq.heappop(heap)

        # 3. Skip stale entries (heapq cannot "decrease a key").
        if u in visited or prio > priority(u, dist[u]):
            continue
        visited.add(u)
        pops += 1
        if trace is not None:
            event = {"node": u, "cost": dist[u][0], "heap": [], "relaxed": []}
            trace.append(event)

        if u == destination:          # final once popped -> stop early
            break

        # 4. Relax every edge of the requested mode leaving u.
        for edge in graph[u]:
            if edge.mode != mode or edge.to in banned_nodes or (u, edge.to, id(edge)) in banned_edges:
                continue
            step = _edge_cost(edge, weight)
            new = (dist[u][0] + step[0], dist[u][1] + step[1])
            if new < dist[edge.to]:
                dist[edge.to] = new
                parent[edge.to] = (u, edge)
                heapq.heappush(heap, (priority(edge.to, new), edge.to))
                if trace is not None:
                    event["relaxed"].append((edge.to, new[0]))
        if trace is not None:
            # Heap after this node's neighbours were pushed, without stale entries.
            live = [(dist[n][0], n) for p, n in heapq.nsmallest(12, heap)
                    if n not in visited and p <= priority(n, dist[n])]
            event["heap"] = live[:4]

    # 5. Unreachable destination.
    if dist[destination] == INF:
        return None

    # 6. Rebuild the path from the parent links, adding up the real totals.
    path, used = [destination], []
    node = destination
    while node != source:
        prev, edge = parent[node]
        used.append(edge)
        path.append(prev)
        node = prev
    path.reverse()
    used.reverse()
    return PathResult(mode=mode, path=path,
                      distance_km=round(sum(e.distance_km for e in used), 3),
                      time_min=round(sum(e.time_min for e in used), 3),
                      edges=used, pops=pops)


def dijkstra(graph, source, destination, mode, weight="distance_km", trace=None):
    """
    Lowest-`weight` route from `source` to `destination` using ONLY `mode` edges.

    weight = "distance_km" (default) -> shortest route, which is also the cheapest
             because fare = base + per_km * distance. Ties go to the quicker route.
    weight = "time_min"              -> quickest route for that mode.

    Returns a PathResult, or None if the destination is unreachable by `mode`.
    Time: O((V + E) log V) with a binary heap.  Space: O(V + E).

    trace: optional list. One event per settled node is appended:
           {"node", "cost", "heap": [(cost, node), ...up to 4], "relaxed": [(node, cost), ...]}
           Used by the website to replay the run. It does not change the result.
    """
    return _search(graph, source, destination, mode, weight, trace=trace)


def astar(graph, source, destination, mode, heuristic, weight="distance_km", trace=None):
    """A* search: Dijkstra plus an estimate of the remaining cost.

    `heuristic(node)` must never overestimate the real remaining cost (admissible)
    and must be consistent, otherwise A* can return a worse path. With h = 0 it is
    exactly Dijkstra. A good h settles fewer nodes (see PathResult.pops).
    """
    return _search(graph, source, destination, mode, weight, trace=trace, heuristic=heuristic)


# ---------------------------------------------------------------------------
# Yen's k-shortest loopless paths
# ---------------------------------------------------------------------------

def k_shortest_paths(graph, source, destination, mode, k=3, weight="distance_km"):
    """The k best loopless routes for one mode (Yen's algorithm).

    For each route already found, every node on it is tried as a "spur": the part
    before it is kept, the edges that earlier routes used next are banned, and a
    fresh Dijkstra finds the rest. The best candidate becomes the next route.
    """
    first = dijkstra(graph, source, destination, mode, weight)
    if first is None:
        return []
    found = [first]
    candidates = []                     # min-heap of (cost, tie, PathResult)
    seen = {tuple(first.path)}
    tie = 0

    def cost_of(result):
        return (result.distance_km, result.time_min) if weight == "distance_km" else (result.time_min, result.distance_km)

    while len(found) < k:
        last = found[-1]
        for i in range(len(last.path) - 1):
            spur, root_path, root_edges = last.path[i], last.path[: i + 1], last.edges[:i]
            banned_edges = set()
            for p in found:
                if p.path[: i + 1] == root_path and len(p.edges) > i:
                    e = p.edges[i]
                    banned_edges.add((spur, e.to, id(e)))
            banned_nodes = frozenset(root_path[:-1])     # keep the route loopless
            spur_result = _search(graph, spur, destination, mode, weight,
                                  banned_edges=frozenset(banned_edges), banned_nodes=banned_nodes)
            if spur_result is None:
                continue
            edges = root_edges + spur_result.edges
            path = root_path[:-1] + spur_result.path
            if tuple(path) in seen:
                continue
            seen.add(tuple(path))
            total = PathResult(mode=mode, path=path, edges=edges,
                               distance_km=round(sum(e.distance_km for e in edges), 3),
                               time_min=round(sum(e.time_min for e in edges), 3))
            tie += 1
            heapq.heappush(candidates, (cost_of(total), tie, total))
        if not candidates:
            break
        found.append(heapq.heappop(candidates)[2])
    return found


# ---------------------------------------------------------------------------
# Mixed-mode journeys: Dijkstra on (location, mode) states
# ---------------------------------------------------------------------------

def mixed_route(graph, source, destination, fare_rules=FARE_RULES, minute_value=0.0,
                transfer_min=TRANSFER_MIN, modes=MODES):
    """Best journey that may switch modes, e.g. Bus -> Metro -> Bus.

    State = (location, mode you are currently riding). Moves:
      * ride an edge of the current mode: + per_km x km, + time
      * board a different mode at the same place: + that mode's base fare,
        + transfer_min minutes (not for the very first boarding)
    Cost minimised = fare + minute_value x minutes, so the visitor's priority
    decides how much a saved minute is worth. All costs are non-negative, so
    Dijkstra is still correct on this bigger state graph.
    """
    validate_location(graph, source)
    validate_location(graph, destination)
    if source == destination:
        return None

    start = (source, None)
    best = {start: (0.0, 0.0)}           # (generalised cost, minutes)
    parent = {}
    done = set()
    heap = [((0.0, 0.0), source, "")]    # mode stored as "" for None so tuples compare

    goal = None
    while heap:
        cost, node, mode = heapq.heappop(heap)
        state = (node, mode or None)
        if state in done or cost > best[state]:
            continue
        done.add(state)
        if node == destination and mode:
            goal = state
            break

        # Board (or change to) another mode here.
        for new_mode in modes:
            if new_mode == state[1]:
                continue
            extra_min = transfer_min if state[1] else 0.0
            step = fare_rules[new_mode]["base"] + minute_value * extra_min
            nxt, new_cost = (node, new_mode), (cost[0] + step, cost[1] + extra_min)
            if new_cost < best.get(nxt, INF):
                best[nxt], parent[nxt] = new_cost, (state, None)
                heapq.heappush(heap, (new_cost, node, new_mode))

        # Ride the current mode.
        if state[1]:
            for edge in graph[node]:
                if edge.mode != state[1]:
                    continue
                step = fare_rules[edge.mode]["per_km"] * edge.distance_km + minute_value * edge.time_min
                nxt = (edge.to, edge.mode)
                new_cost = (cost[0] + step, cost[1] + edge.time_min)
                if new_cost < best.get(nxt, INF):
                    best[nxt], parent[nxt] = new_cost, (state, edge)
                    heapq.heappush(heap, (new_cost, edge.to, edge.mode))

    if goal is None:
        return None

    # Rebuild: walk back through states, grouping rides of the same mode into legs.
    steps = []
    state = goal
    while state != start:
        prev, edge = parent[state]
        steps.append((prev, state, edge))
        state = prev
    steps.reverse()

    legs, path, edges = [], [source], []
    for prev, state, edge in steps:
        if edge is None:                       # boarding a new mode
            legs.append({"mode": state[1], "path": [state[0]], "edges": []})
            continue
        legs[-1]["path"].append(edge.to)
        legs[-1]["edges"].append(edge)
        path.append(edge.to)
        edges.append(edge)
    legs = [leg for leg in legs if leg["edges"]]   # drop a boarding that was never used
    out_legs = []
    for leg in legs:
        km = round(sum(e.distance_km for e in leg["edges"]), 3)
        out_legs.append({
            "mode": leg["mode"], "label": mode_label(leg["mode"]), "path": leg["path"],
            "distance_km": km, "time_min": round(sum(e.time_min for e in leg["edges"]), 3),
            "fare": calculate_fare(leg["mode"], km, fare_rules), "edges": leg["edges"],
        })
    transfers = max(len(out_legs) - 1, 0)
    return JourneyOption(
        mode="mixed",
        label="Smart mix: " + " → ".join(leg["label"] for leg in out_legs),
        path=path,
        distance_km=round(sum(leg["distance_km"] for leg in out_legs), 3),
        time_min=round(sum(leg["time_min"] for leg in out_legs) + transfers * transfer_min, 3),
        fare=round(sum(leg["fare"] for leg in out_legs), 2),
        edges=edges, variant="mixed", legs=out_legs,
    )


# ---------------------------------------------------------------------------
# Comparison across modes
# ---------------------------------------------------------------------------

def _option(route, mode, fare_rules, variant):
    label = mode_label(mode) + (" (quicker route)" if variant == "fastest" else "")
    return JourneyOption(mode=mode, label=label, path=route.path,
                         distance_km=route.distance_km, time_min=route.time_min,
                         fare=calculate_fare(mode, route.distance_km, fare_rules),
                         edges=route.edges, variant=variant)


def compare_routes(graph, source, destination, modes=MODES, fare_rules=FARE_RULES,
                   priority="cheapest", include_extras=True):
    """
    Price every mode and rank the journeys.

    For each mode: its cheapest route (shortest distance) and, if a different route
    is quicker, that quicker route too. Plus one "Smart mix" journey when changing
    modes beats every single-mode trip for the chosen priority.

    * Rows are ranked by `priority` ("cheapest", "balanced" or "fastest").
    * Exactly one row is the cheapest and exactly one is the fastest.
    * Modes that cannot reach the destination go into `unavailable`.
    * If source == destination, no journey is needed: options stay empty.
    """
    validate_location(graph, source)
    validate_location(graph, destination)
    if priority not in PRIORITIES:
        raise ValueError(f"priority must be one of {PRIORITIES}")

    result = Comparison(source=source, destination=destination, priority=priority)
    if source == destination:
        return result

    for mode in modes:
        cheapest = dijkstra(graph, source, destination, mode, weight="distance_km")
        if cheapest is None:
            result.unavailable.append(mode)
            continue
        result.options.append(_option(cheapest, mode, fare_rules, "cheapest"))
        if include_extras:
            quickest = dijkstra(graph, source, destination, mode, weight="time_min")
            if quickest.time_min < cheapest.time_min - 1e-9:
                result.options.append(_option(quickest, mode, fare_rules, "fastest"))

    if include_extras:
        mix = mixed_route(graph, source, destination, fare_rules,
                          minute_value=MINUTE_VALUE[priority], modes=modes)
        # Show the mix when it changes mode AND either no single mode can make the
        # trip at all, or it beats every single-mode journey for this priority.
        if mix is not None and len(mix.legs) > 1:
            if not result.options or rank_key(mix, priority) < min(rank_key(o, priority) for o in result.options):
                result.options.append(mix)

    result.options.sort(key=lambda o: rank_key(o, priority))
    return result
