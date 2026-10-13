"""Smoke test: the optional map draws without errors (skipped if libs missing)."""

import pytest

pytest.importorskip("networkx")
pytest.importorskip("matplotlib")

import matplotlib.pyplot as plt  # noqa: E402

from src.graph import build_graph  # noqa: E402
from src.routing import compare_routes  # noqa: E402
from src.visualize import draw_network  # noqa: E402


def test_draw_network_with_and_without_route():
    graph = build_graph()
    route = compare_routes(graph, "College", "Airport").cheapest
    for fig in (draw_network(graph), draw_network(graph, route)):
        assert fig.axes
        plt.close(fig)
