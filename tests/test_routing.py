import math

import pytest

from src.fares import calculate_fare
from src.graph import CONNECTIONS, LOCATIONS, MODES, build_graph
from src.routing import (
    UnknownLocationError,
    compare_routes,
    dijkstra,
    find_location,
)

# ---------------------------------------------------------------------------
# Small hand-made graph used by most algorithm tests
#
#   A --1-- B --1-- C          bus:   A-B 1km, B-C 1km, A-C 5km (direct, longer)
#    \_____5_______/                  C-D 2km
#           |                  metro: A-D 10km only
#           D                  E is isolated (no edges)
# ---------------------------------------------------------------------------
TOY_LOCATIONS = ("A", "B", "C", "D", "E")
TOY_CONNECTIONS = [
    ("A", "B", "bus", 1, 10),
    ("B", "C", "bus", 1, 10),
    ("A", "C", "bus", 5, 4),
    ("C", "D", "bus", 2, 6),
    ("A", "D", "metro", 10, 8),
]


@pytest.fixture
def toy():
    return build_graph(TOY_CONNECTIONS, TOY_LOCATIONS)


@pytest.fixture
def city():
    return build_graph()


# ---------------------------------------------------------------------------
# Dijkstra basics
# ---------------------------------------------------------------------------

def test_known_shortest_path(toy):
    result = dijkstra(toy, "A", "D", "bus")
    assert result.path == ["A", "B", "C", "D"]
    assert result.distance_km == 4


def test_prefers_shorter_multi_hop_route_over_direct_edge(toy):
    # BFS (fewest edges) would choose A-C directly (5 km).
    # Dijkstra correctly chooses A-B-C (2 km).
    result = dijkstra(toy, "A", "C", "bus")
    assert result.path == ["A", "B", "C"]
    assert result.distance_km == 2


def test_path_reconstruction_is_consistent(city):
    for src in LOCATIONS:
        for dst in LOCATIONS:
            for mode in MODES:
                r = dijkstra(city, src, dst, mode)
                if r is None:
                    continue
                assert r.path[0] == src and r.path[-1] == dst
                # Every consecutive pair must be joined by an edge of this mode.
                km = 0
                for u, v in zip(r.path, r.path[1:]):
                    edges = [e for e in city[u] if e.to == v and e.mode == mode]
                    assert edges, f"{u}->{v} has no {mode} edge"
                    km += min(e.distance_km for e in edges)
                assert km == r.distance_km


def test_distance_and_time_totals(toy):
    r = dijkstra(toy, "A", "D", "bus")
    assert r.distance_km == 1 + 1 + 2
    assert r.time_min == 10 + 10 + 6


def test_stale_heap_entries_are_ignored():
    # C is first pushed with cost 10 (via A), then improved to 2 (via B).
    graph = build_graph(
        [("A", "C", "bus", 10, 1), ("A", "B", "bus", 1, 1), ("B", "C", "bus", 1, 1)],
        ("A", "B", "C"),
    )
    r = dijkstra(graph, "A", "C", "bus")
    assert r.distance_km == 2
    assert r.path == ["A", "B", "C"]


def test_parallel_edges_use_the_shorter_one_and_its_time():
    graph = build_graph(
        [("A", "B", "bus", 8, 10), ("A", "B", "bus", 3, 25)],
        ("A", "B"),
    )
    r = dijkstra(graph, "A", "B", "bus")
    assert r.distance_km == 3
    assert r.time_min == 25


# ---------------------------------------------------------------------------
# Mode filtering
# ---------------------------------------------------------------------------

def test_mode_filtering_never_mixes_modes(toy):
    # Bus route A->D is 4 km; metro must ignore it and use its own 10 km edge.
    r = dijkstra(toy, "A", "D", "metro")
    assert r.path == ["A", "D"]
    assert r.distance_km == 10


def test_mode_without_edges_is_unreachable(toy):
    assert dijkstra(toy, "A", "D", "car_taxi") is None


def test_metro_cannot_reach_hospital_in_sample_city(city):
    assert dijkstra(city, "Hospital", "Airport", "metro") is None


# ---------------------------------------------------------------------------
# Unreachable / invalid input / same location
# ---------------------------------------------------------------------------

def test_unreachable_destination_returns_none(toy):
    assert dijkstra(toy, "A", "E", "bus") is None


def test_unknown_location_raises(toy):
    with pytest.raises(UnknownLocationError):
        dijkstra(toy, "A", "Mars", "bus")
    with pytest.raises(UnknownLocationError):
        compare_routes(toy, "Nowhere", "A")


def test_unknown_mode_raises(toy):
    with pytest.raises(ValueError):
        dijkstra(toy, "A", "B", "rocket")


def test_source_equals_destination(toy):
    r = dijkstra(toy, "A", "A", "bus")
    assert r.path == ["A"] and r.distance_km == 0 and r.time_min == 0

    comparison = compare_routes(toy, "A", "A")
    assert comparison.same_location
    assert comparison.options == []
    assert comparison.cheapest is None


def test_find_location_is_case_and_space_insensitive(city):
    assert find_location(city, "  rAilway   station ") == "Railway Station"
    with pytest.raises(UnknownLocationError):
        find_location(city, "")
    with pytest.raises(UnknownLocationError):
        find_location(city, "Moon Base")


def test_directed_edges_are_supported():
    graph = build_graph([("A", "B", "bus", 1, 1)], ("A", "B"), bidirectional=False)
    assert dijkstra(graph, "A", "B", "bus") is not None
    assert dijkstra(graph, "B", "A", "bus") is None


def test_negative_distance_rejected_when_building_graph():
    with pytest.raises(ValueError):
        build_graph([("A", "B", "bus", -1, 1)], ("A", "B"))


# ---------------------------------------------------------------------------
# Comparison, sorting, cheapest vs fastest, ties
# ---------------------------------------------------------------------------

def test_options_sorted_by_fare(city):
    for src in LOCATIONS:
        for dst in LOCATIONS:
            fares = [o.fare for o in compare_routes(city, src, dst).options]
            assert fares == sorted(fares)


def test_fare_matches_route_distance(city):
    comparison = compare_routes(city, "College", "Airport")
    for o in comparison.options:
        assert o.fare == calculate_fare(o.mode, o.distance_km)


def test_college_to_airport_sample_result(city):
    # Values worked out by hand from CONNECTIONS (see docs/VIVA_NOTES.md).
    c = compare_routes(city, "College", "Airport")
    summary = [(o.mode, o.distance_km, o.time_min, o.fare) for o in c.options]
    assert summary == [
        ("bus", 26, 88, 57),
        ("metro", 27, 45, 91),
        ("bike_taxi", 24, 50, 212),
        ("car_taxi", 25, 54, 390),
    ]
    assert c.unavailable == []


def test_cheapest_and_fastest_can_differ(city):
    c = compare_routes(city, "College", "Airport")
    assert c.cheapest.mode == "bus"
    assert c.fastest.mode == "metro"


def test_cheapest_route_differs_from_fastest_route_within_a_mode(city):
    cheapest = dijkstra(city, "College", "Airport", "car_taxi", weight="distance_km")
    fastest = dijkstra(city, "College", "Airport", "car_taxi", weight="time_min")
    assert cheapest.path != fastest.path
    assert cheapest.distance_km < fastest.distance_km
    assert fastest.time_min < cheapest.time_min


def test_unavailable_modes_reported(city):
    c = compare_routes(city, "Hospital", "Airport")
    assert c.unavailable == ["metro"]
    assert "metro" not in [o.mode for o in c.options]


def test_equal_distance_routes_give_deterministic_result():
    # Two 2 km routes A-B-D and A-C-D: result must be the same every run.
    graph = build_graph(
        [("A", "B", "bus", 1, 1), ("B", "D", "bus", 1, 1),
         ("A", "C", "bus", 1, 1), ("C", "D", "bus", 1, 1)],
        ("A", "B", "C", "D"),
    )
    paths = {tuple(dijkstra(graph, "A", "D", "bus").path) for _ in range(20)}
    assert len(paths) == 1
    assert dijkstra(graph, "A", "D", "bus").distance_km == 2


def test_fare_ties_broken_by_time_and_reported():
    # bus: 5 + 2*10 = 25 ; metro: 10 + 3*5 = 25  -> equal fares.
    graph = build_graph(
        [("A", "B", "bus", 10, 40), ("A", "B", "metro", 5, 20)],
        ("A", "B"),
    )
    c = compare_routes(graph, "A", "B")
    assert [o.fare for o in c.options] == [25, 25]
    assert c.cheapest.mode == "metro"          # same fare, faster wins
    assert {o.mode for o in c.tied_cheapest} == {"bus", "metro"}


# ---------------------------------------------------------------------------
# Independent check: brute force over every simple path in the sample city
# ---------------------------------------------------------------------------

def _brute_force_shortest(graph, src, dst, mode):
    best = math.inf
    stack = [(src, 0, {src})]
    while stack:
        node, km, seen = stack.pop()
        if node == dst:
            best = min(best, km)
            continue
        for e in graph[node]:
            if e.mode == mode and e.to not in seen:
                stack.append((e.to, km + e.distance_km, seen | {e.to}))
    return best


def test_dijkstra_matches_brute_force_on_every_pair_and_mode(city):
    for mode in MODES:
        for src in LOCATIONS:
            for dst in LOCATIONS:
                expected = _brute_force_shortest(city, src, dst, mode)
                r = dijkstra(city, src, dst, mode)
                if expected == math.inf:
                    assert r is None
                else:
                    assert r.distance_km == expected, (mode, src, dst)


def test_sample_network_size():
    assert len(LOCATIONS) == 12
    assert len(CONNECTIONS) == 47
    graph = build_graph()
    assert sum(len(edges) for edges in graph.values()) == 94
    # Every location is served by at least one mode.
    assert all(graph[name] for name in LOCATIONS)


def test_trace_records_every_settled_node_without_changing_result(toy):
    trace = []
    with_trace = dijkstra(toy, "A", "D", "bus", trace=trace)
    assert with_trace == dijkstra(toy, "A", "D", "bus")
    settled = [event["node"] for event in trace]
    assert settled[0] == "A" and settled[-1] == "D"
    assert len(settled) == len(set(settled))            # each node settled once
    costs = [event["cost"] for event in trace]
    assert costs == sorted(costs)                         # min-heap pops in order
