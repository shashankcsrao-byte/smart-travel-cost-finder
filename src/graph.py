"""
Sample city transport network, stored as a weighted graph.

Vertices  = locations (e.g. "College", "Airport")
Edges     = transport connections between two locations for ONE mode,
            carrying a distance (km) and an estimated travel time (minutes).

The graph is an ADJACENCY LIST: a dictionary that maps every location to the
list of edges leaving it.

    graph["College"] -> [Edge(to="Market", mode="bus", distance_km=3, time_min=12), ...]

IMPORTANT: all distances, times and routes below are ILLUSTRATIVE SAMPLE DATA
for a college project. They are not real Bengaluru (or any city's) data.
"""

from dataclasses import dataclass

# The four supported transport modes. The order here is also used as the
# final tie-breaker when two journeys have identical fare and time.
MODES = ("bus", "metro", "bike_taxi", "car_taxi")


@dataclass(frozen=True)
class Edge:
    """One directed connection out of a location, for a single transport mode."""

    to: str
    mode: str
    distance_km: float
    time_min: float


# 12 locations (vertices) in the sample city.
LOCATIONS = (
    "College",
    "Residential Area",
    "Market",
    "Bus Stand",
    "Metro Station",
    "Railway Station",
    "Hospital",
    "City Centre",
    "Shopping Mall",
    "Tech Park",
    "Junction",
    "Airport",
)

# Hand-placed (x, y) map coordinates, used only for drawing the network map
# (College on the left, Airport on the right).
LOCATION_COORDS = {
    "Metro Station": (1.2, 4.6),
    "Railway Station": (2.8, 5.5),
    "Market": (1.6, 3.1),
    "College": (0.0, 2.3),
    "Residential Area": (0.5, 0.2),
    "Bus Stand": (2.8, 2.2),
    "Hospital": (3.9, 0.7),
    "City Centre": (4.3, 3.3),
    "Shopping Mall": (5.3, 4.6),
    "Junction": (5.7, 1.5),
    "Tech Park": (6.7, 3.6),
    "Airport": (8.2, 2.3),
}

# Each tuple is one BIDIRECTIONAL connection:
#   (location_a, location_b, mode, distance_km, time_min)
# build_graph() turns each tuple into two directed edges (a -> b and b -> a).
#
# Design notes:
#   * Bus has the widest network but is the slowest.
#   * Metro is fast but only serves a few locations, so some journeys
#     (e.g. Hospital -> Airport) are NOT possible by metro.
#   * Bike taxi has a Market <-> City Centre shortcut.
#   * Car taxi has a ring road (Residential Area <-> Tech Park) and an airport
#     expressway (Tech Park <-> Airport): longer in km but quicker in minutes.
CONNECTIONS = (
    # ---------------- Bus (14) ----------------
    ("College", "Market", "bus", 3, 12),
    ("College", "Residential Area", "bus", 4, 15),
    ("Market", "Bus Stand", "bus", 2, 8),
    ("Market", "Metro Station", "bus", 2, 8),
    ("Residential Area", "Bus Stand", "bus", 5, 18),
    ("Bus Stand", "Railway Station", "bus", 3, 12),
    ("Bus Stand", "Hospital", "bus", 4, 15),
    ("Bus Stand", "City Centre", "bus", 4, 16),
    ("Hospital", "Junction", "bus", 5, 18),
    ("City Centre", "Shopping Mall", "bus", 3, 12),
    ("City Centre", "Junction", "bus", 6, 22),
    ("Shopping Mall", "Tech Park", "bus", 4, 15),
    ("Junction", "Airport", "bus", 12, 35),
    ("Tech Park", "Airport", "bus", 14, 40),
    # ---------------- Metro (6) ----------------
    ("College", "Metro Station", "metro", 2, 5),
    ("Metro Station", "City Centre", "metro", 6, 10),
    ("Metro Station", "Railway Station", "metro", 4, 7),
    ("Railway Station", "City Centre", "metro", 3, 5),
    ("City Centre", "Tech Park", "metro", 7, 12),
    ("Tech Park", "Airport", "metro", 12, 18),
    # ---------------- Bike taxi (13) ----------------
    ("College", "Market", "bike_taxi", 3, 7),
    ("College", "Residential Area", "bike_taxi", 4, 9),
    ("Market", "Bus Stand", "bike_taxi", 2, 5),
    ("Market", "Metro Station", "bike_taxi", 2, 5),
    ("Market", "City Centre", "bike_taxi", 5, 11),
    ("Bus Stand", "Railway Station", "bike_taxi", 3, 7),
    ("Bus Stand", "Hospital", "bike_taxi", 4, 9),
    ("Hospital", "Junction", "bike_taxi", 5, 11),
    ("City Centre", "Junction", "bike_taxi", 5, 10),
    ("City Centre", "Shopping Mall", "bike_taxi", 3, 7),
    ("City Centre", "Tech Park", "bike_taxi", 7, 15),
    ("Junction", "Airport", "bike_taxi", 11, 22),
    ("Tech Park", "Airport", "bike_taxi", 13, 25),
    # ---------------- Car taxi (14) ----------------
    ("College", "Market", "car_taxi", 3, 8),
    ("College", "Residential Area", "car_taxi", 4, 10),
    ("Residential Area", "Tech Park", "car_taxi", 9, 15),
    ("Market", "Bus Stand", "car_taxi", 2, 6),
    ("Market", "Metro Station", "car_taxi", 2, 6),
    ("Market", "City Centre", "car_taxi", 5, 14),
    ("Bus Stand", "Railway Station", "car_taxi", 3, 8),
    ("Bus Stand", "Hospital", "car_taxi", 4, 10),
    ("Hospital", "Junction", "car_taxi", 5, 12),
    ("City Centre", "Junction", "car_taxi", 5, 12),
    ("City Centre", "Shopping Mall", "car_taxi", 3, 8),
    ("Shopping Mall", "Tech Park", "car_taxi", 4, 10),
    ("Junction", "Airport", "car_taxi", 12, 20),
    ("Tech Park", "Airport", "car_taxi", 15, 18),
)


def build_graph(connections=CONNECTIONS, locations=LOCATIONS, bidirectional=True):
    """
    Build the adjacency list {location: [Edge, ...]} from a list of connections.

    Every location gets a key, even if it has no edges, so that "unknown
    location" and "known but unreachable" can be told apart.

    Raises ValueError for unknown locations/modes or negative values, because
    Dijkstra's algorithm is only correct for non-negative edge weights.
    """
    graph = {name: [] for name in locations}

    for a, b, mode, distance_km, time_min in connections:
        if a not in graph or b not in graph:
            raise ValueError(f"Connection uses an unknown location: {a!r} - {b!r}")
        if mode not in MODES:
            raise ValueError(f"Unknown transport mode: {mode!r}")
        if distance_km < 0 or time_min < 0:
            raise ValueError(f"Negative distance/time on {a} - {b} ({mode})")

        graph[a].append(Edge(b, mode, distance_km, time_min))
        if bidirectional:
            graph[b].append(Edge(a, mode, distance_km, time_min))

    return graph


def list_connections(connections=CONNECTIONS, mode=None):
    """Return the connection tuples, optionally only those of one mode."""
    return [c for c in connections if mode is None or c[2] == mode]
