"""
Optional network drawing with NetworkX + Matplotlib.

NetworkX is used ONLY to draw the map. All routes are computed by our own
Dijkstra implementation in routing.py.
"""

import matplotlib

matplotlib.use("Agg")  # render without a display window (needed by Streamlit)

import matplotlib.pyplot as plt  # noqa: E402
import networkx as nx  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402

from .graph import LOCATION_COORDS  # noqa: E402

ORANGE = "#F26B1D"
CHARCOAL = "#2B2D33"
GREY = "#C9CBCF"
METRO = "#7A5AF8"

POSITIONS = LOCATION_COORDS


def draw_network(graph, route=None):
    """
    Draw the whole network and, optionally, highlight one JourneyOption.

    Grey lines   = road connections (bus / bike taxi / car taxi)
    Purple dash  = metro-only connections
    Orange       = the highlighted route
    Returns a Matplotlib Figure.
    """
    # Collapse the per-mode directed edges into one undirected drawing edge
    # per pair of locations, remembering which modes serve it.
    drawing = nx.Graph()
    drawing.add_nodes_from(graph)
    for u, edges in graph.items():
        for e in edges:
            if drawing.has_edge(u, e.to):
                drawing[u][e.to]["modes"].add(e.mode)
            else:
                drawing.add_edge(u, e.to, modes={e.mode})

    pos = {name: POSITIONS.get(name, (0, 0)) for name in graph}
    if any(name not in POSITIONS for name in graph):
        pos = nx.spring_layout(drawing, seed=7)

    metro_only = [(u, v) for u, v, d in drawing.edges(data=True) if d["modes"] == {"metro"}]
    roads = [(u, v) for u, v in drawing.edges() if (u, v) not in metro_only]

    route_pairs = []
    route_nodes = set()
    if route is not None:
        route_nodes = set(route.path)
        route_pairs = list(zip(route.path, route.path[1:]))

    fig, ax = plt.subplots(figsize=(10, 5.6), dpi=110)
    fig.patch.set_facecolor("white")

    nx.draw_networkx_edges(drawing, pos, edgelist=roads, width=2, edge_color=GREY, ax=ax)
    nx.draw_networkx_edges(drawing, pos, edgelist=metro_only, width=2,
                           edge_color=METRO, style="dashed", alpha=0.7, ax=ax)
    if route_pairs:
        nx.draw_networkx_edges(drawing, pos, edgelist=route_pairs, width=6,
                               edge_color=ORANGE, ax=ax)
        labels = {
            (u, e.to): f"{e.distance_km:g} km"
            for u, e in zip(route.path, route.edges)
        }
        nx.draw_networkx_edge_labels(
            drawing, pos, edge_labels=labels, font_size=8, font_color=CHARCOAL, rotate=False,
            bbox={"boxstyle": "round,pad=0.2", "fc": "white", "ec": ORANGE, "lw": 1},
            ax=ax,
        )

    other_nodes = [n for n in graph if n not in route_nodes]
    nx.draw_networkx_nodes(drawing, pos, nodelist=other_nodes, node_size=520,
                           node_color="white", edgecolors=CHARCOAL, linewidths=1.6, ax=ax)
    if route_nodes:
        ends = [route.path[0], route.path[-1]]
        middle = [n for n in route.path[1:-1]]
        nx.draw_networkx_nodes(drawing, pos, nodelist=middle, node_size=560,
                               node_color=ORANGE, edgecolors=CHARCOAL, linewidths=1.6, ax=ax)
        nx.draw_networkx_nodes(drawing, pos, nodelist=ends, node_size=760,
                               node_color=CHARCOAL, edgecolors=ORANGE, linewidths=3, ax=ax)

    for name, (x, y) in pos.items():
        weight = "bold" if name in route_nodes else "normal"
        ax.text(x, y - 0.38, name, ha="center", va="top", fontsize=9,
                color=CHARCOAL, fontweight=weight,
                bbox={"boxstyle": "round,pad=0.15", "fc": "white", "ec": "none", "alpha": 0.85})

    legend = [
        Line2D([0], [0], color=GREY, lw=2, label="Road (bus / bike taxi / car taxi)"),
        Line2D([0], [0], color=METRO, lw=2, ls="--", label="Metro-only line"),
    ]
    if route_pairs:
        legend.append(Line2D([0], [0], color=ORANGE, lw=5, label=f"Selected route ({route.label})"))
    ax.legend(handles=legend, loc="lower right", fontsize=8, frameon=True)

    ax.set_axis_off()
    ax.margins(0.08, 0.14)
    fig.tight_layout()
    return fig
