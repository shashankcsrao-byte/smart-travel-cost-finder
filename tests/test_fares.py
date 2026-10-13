import pytest

from src.fares import FARE_RULES, calculate_fare
from src.graph import MODES, build_graph
from src.routing import compare_routes


def test_bus_fare_formula():
    # 5 base + 2/km * 10 km
    assert calculate_fare("bus", 10) == 25


@pytest.mark.parametrize("mode", MODES)
def test_every_mode_uses_base_plus_per_km(mode):
    rule = FARE_RULES[mode]
    assert calculate_fare(mode, 7) == rule["base"] + rule["per_km"] * 7


def test_zero_distance_costs_only_base_fare():
    assert calculate_fare("car_taxi", 0) == 40


def test_fractional_distance():
    assert calculate_fare("metro", 2.5) == 17.5


def test_unknown_mode_rejected():
    with pytest.raises(ValueError):
        calculate_fare("helicopter", 5)


def test_negative_distance_rejected():
    with pytest.raises(ValueError):
        calculate_fare("bus", -1)


def test_base_fare_charged_once_for_multi_edge_route():
    # Three bus edges of 2 km each: A-B-C-D (6 km total).
    locations = ("A", "B", "C", "D")
    connections = [
        ("A", "B", "bus", 2, 5),
        ("B", "C", "bus", 2, 5),
        ("C", "D", "bus", 2, 5),
    ]
    graph = build_graph(connections, locations)
    option = compare_routes(graph, "A", "D", modes=("bus",)).cheapest

    assert option.path == ["A", "B", "C", "D"]
    assert option.fare == 5 + 2 * 6          # 17: base once
    assert option.fare != 3 * 5 + 2 * 6      # 27: base per edge (wrong)
