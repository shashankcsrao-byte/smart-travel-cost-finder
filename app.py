"""
Streamlit dashboard for Smart Travel Cost Finder.

Run from the project folder:
    streamlit run app.py
"""

from html import escape

import matplotlib.pyplot as plt
import streamlit as st

from src.fares import FARE_RULES, mode_label
from src.graph import CONNECTIONS, LOCATIONS, MODES, build_graph
from src.main import format_km, format_minutes, format_rupees
from src.routing import compare_routes
from src.visualize import draw_network

st.set_page_config(
    page_title="Smart Travel Cost Finder",
    page_icon="🧭",
    layout="wide",
)

MODE_ICONS = {"bus": "🚌", "metro": "🚇", "bike_taxi": "🏍️", "car_taxi": "🚕"}

# ---------------------------------------------------------------------------
# Styling: white, orange and charcoal
# ---------------------------------------------------------------------------
st.markdown(
    """
<style>
:root { --orange:#F26B1D; --orange-soft:#FFF1E8; --charcoal:#2B2D33; --muted:#6B6E76; --line:#E6E3DF; }
.block-container { padding-top: 2rem; max-width: 1200px; }
.hero { background: var(--charcoal); color: #fff; border-radius: 18px; padding: 28px 32px;
        border-bottom: 6px solid var(--orange); margin-bottom: 1.4rem; }
.hero h1, .hero h1 .plain { color:#fff !important; } .hero h1 { font-size: 2.1rem; margin: 0; letter-spacing: .5px; }
.hero h1 .accent { color: var(--orange) !important; }
.hero p { color:#D9DADD; margin: .4rem 0 0; font-size: 1.02rem; }
.hero .tag { display:inline-block; margin-top: .9rem; font-size:.78rem; padding: 3px 10px;
             border:1px solid #5A5D66; border-radius: 999px; color:#C7C9CE; }
.card { background:#fff; border:1px solid var(--line); border-radius:16px; padding:18px 18px 14px;
        box-shadow: 0 2px 10px rgba(43,45,51,.06); height:100%; }
.card.best { border: 2px solid var(--orange); box-shadow: 0 6px 18px rgba(242,107,29,.18); }
.card.off { background:#FAFAF9; color: var(--muted); }
.card .rank { font-size:.78rem; color: var(--muted); font-weight:600; letter-spacing:.6px; }
.card .mode { font-size:1.2rem; font-weight:700; color: var(--charcoal); margin:.15rem 0 .4rem; }
.card .fare { font-size:2rem; font-weight:800; color: var(--orange); line-height:1.1; }
.card.off .fare { color:#A8AAB0; font-size:1.2rem; }
.card .metrics span { white-space:nowrap; }
.card .metrics { display:flex; flex-wrap:wrap; gap:6px 14px; margin:.5rem 0 .7rem; color: var(--charcoal); font-size:.92rem; }
.card .path { font-size:.82rem; color: var(--muted); line-height:1.55; }
.card .path b { color: var(--charcoal); font-weight:600; }
.badge { display:inline-block; font-size:.68rem; font-weight:700; letter-spacing:.6px;
         padding:3px 8px; border-radius:999px; margin-right:4px; vertical-align:middle; }
.badge.cheap { background: var(--orange); color:#fff; }
.badge.fast { background: var(--charcoal); color:#fff; }
.badge.na { background:#ECEBE9; color: var(--muted); }
.summary { background: var(--orange-soft); border-left: 5px solid var(--orange); border-radius: 12px;
           padding: 14px 18px; margin: .4rem 0 1.2rem; color: var(--charcoal); }
.empty { border: 2px dashed var(--line); border-radius: 16px; padding: 30px; text-align:center; color: var(--muted); }
.note { font-size:.8rem; color: var(--muted); }
</style>
""",
    unsafe_allow_html=True,
)


@st.cache_resource
def get_graph():
    return build_graph()


graph = get_graph()

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------
st.markdown(
    """
<div class="hero">
  <h1><span class="plain">SMART TRAVEL</span> <span class="accent">COST FINDER</span></h1>
  <p>A graph-based multi-modal transport cost optimization system. Compare the cheapest
     route by bus, metro, bike taxi and car taxi, computed with a hand-written Dijkstra's algorithm.</p>
  <span class="tag">Sample city network &middot; illustrative fares &middot; not live data</span>
</div>
""",
    unsafe_allow_html=True,
)

# ---------------------------------------------------------------------------
# Journey input
# ---------------------------------------------------------------------------
col_src, col_dst, col_btn = st.columns([2, 2, 1], vertical_alignment="bottom")
with col_src:
    source = st.selectbox("Source", LOCATIONS, index=LOCATIONS.index("College"))
with col_dst:
    destination = st.selectbox("Destination", LOCATIONS, index=LOCATIONS.index("Airport"))
with col_btn:
    if st.button("Find Routes", type="primary", width="stretch"):
        st.session_state.query = (source, destination)

query = st.session_state.get("query")


def card_html(option, rank, is_cheapest, is_fastest):
    badges = ""
    if is_cheapest:
        badges += '<span class="badge cheap">CHEAPEST</span>'
    if is_fastest:
        badges += '<span class="badge fast">FASTEST</span>'
    stops = " &rarr; ".join(f"<b>{escape(p)}</b>" for p in option.path)
    return f"""
<div class="card {'best' if is_cheapest else ''}">
  <div class="rank">#{rank} &middot; SORTED BY FARE</div>
  <div class="mode">{MODE_ICONS[option.mode]} {escape(option.label)} {badges}</div>
  <div class="fare">{format_rupees(option.fare)}</div>
  <div class="metrics"><span>📏 {format_km(option.distance_km)}</span>
       <span>⏱️ {format_minutes(option.time_min)}</span>
       <span>🔁 {len(option.path) - 1} legs</span></div>
  <div class="path">{stops}</div>
</div>"""


def unavailable_html(mode):
    return f"""
<div class="card off">
  <div class="rank">NOT AVAILABLE</div>
  <div class="mode" style="color:#8A8D94">{MODE_ICONS[mode]} {escape(mode_label(mode))}
       <span class="badge na">NO ROUTE</span></div>
  <div class="fare">&mdash;</div>
  <div class="path">This mode does not connect these two locations in the sample
       network, so it cannot complete the journey on its own.</div>
</div>"""


# ---------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------
if query is None:
    st.markdown(
        """
<div class="empty">
  <div style="font-size:2rem">🧭</div>
  <b>Choose a source and destination, then press <span style="color:#F26B1D">Find Routes</span>.</b><br>
  Try <i>College &rarr; Airport</i> (cheapest and fastest differ) or
  <i>Hospital &rarr; Airport</i> (metro not available).
</div>""",
        unsafe_allow_html=True,
    )
else:
    src, dst = query
    comparison = compare_routes(graph, src, dst)
    st.subheader(f"{src}  →  {dst}")

    if comparison.same_location:
        st.warning("Source and destination are the same, so no journey is needed. "
                   "Please pick two different locations.")
    elif not comparison.options:
        st.error("No transport mode can reach this destination in the sample network.")
    else:
        cheapest_modes = {o.mode for o in comparison.tied_cheapest}
        fastest_modes = {o.mode for o in comparison.tied_fastest}
        cheapest, fastest = comparison.cheapest, comparison.fastest

        tie_note = ""
        if len(comparison.tied_cheapest) > 1:
            tie_note = " (tie: " + ", ".join(o.label for o in comparison.tied_cheapest) + ")"
        st.markdown(
            f"""<div class="summary">💡 <b>Recommendation:</b> the cheapest option is
            <b>{escape(cheapest.label)}</b> at <b>{format_rupees(cheapest.fare)}</b>{escape(tie_note)}.
            The fastest option is <b>{escape(fastest.label)}</b> at
            <b>{format_minutes(fastest.time_min)}</b>.
            {"Saving money costs " + format_minutes(cheapest.time_min - fastest.time_min) + " extra travel time."
             if cheapest.mode != fastest.mode and cheapest.time_min > fastest.time_min else ""}
            </div>""",
            unsafe_allow_html=True,
        )

        m1, m2, m3, m4 = st.columns(4)
        m1.metric(f"Cheapest fare · {cheapest.label}", format_rupees(cheapest.fare))
        m2.metric(f"Fastest time · {fastest.label}", format_minutes(fastest.time_min))
        m3.metric("Shortest distance", format_km(min(o.distance_km for o in comparison.options)))
        m4.metric("Modes available", f"{len(comparison.options)} / {len(MODES)}")

        cols = st.columns(len(MODES))
        cards = [card_html(o, i, o.mode in cheapest_modes, o.mode in fastest_modes)
                 for i, o in enumerate(comparison.options, start=1)]
        cards += [unavailable_html(m) for m in comparison.unavailable]
        for col, html in zip(cols, cards):
            col.markdown(html, unsafe_allow_html=True)

        st.markdown("#### Comparison table")
        st.dataframe(
            [
                {
                    "Rank": i,
                    "Mode": o.label,
                    "Fare (₹)": o.fare,
                    "Distance (km)": o.distance_km,
                    "Time (min)": o.time_min,
                    "Route": " → ".join(o.path),
                }
                for i, o in enumerate(comparison.options, start=1)
            ],
            hide_index=True,
            width="stretch",
        )

        st.markdown("#### Route details and map")
        labels = [o.label for o in comparison.options]
        chosen_label = st.radio("Show route for", labels, horizontal=True)
        chosen = comparison.options[labels.index(chosen_label)]

        map_col, legs_col = st.columns([3, 2])
        with map_col:
            fig = draw_network(graph, chosen)
            st.pyplot(fig)
            plt.close(fig)
        with legs_col:
            st.markdown(f"**{MODE_ICONS[chosen.mode]} {chosen.label}: leg-by-leg**")
            st.dataframe(
                [
                    {"From": a, "To": e.to, "km": e.distance_km, "min": e.time_min}
                    for a, e in zip(chosen.path, chosen.edges)
                ],
                hide_index=True,
                width="stretch",
            )
            rule = FARE_RULES[chosen.mode]
            st.markdown(
                f"""**Fare calculation** (base fare charged once)

`base + per_km × distance` = ₹{rule['base']} + ₹{rule['per_km']} × {chosen.distance_km:g} km
= **{format_rupees(chosen.fare)}**

**Total time** = sum of leg times = **{format_minutes(chosen.time_min)}**"""
            )

    st.markdown(
        '<p class="note">Journey times are the sum of connection times only. Waiting, walking, '
        "transfers and live traffic are not included. All data is illustrative sample data.</p>",
        unsafe_allow_html=True,
    )

# ---------------------------------------------------------------------------
# Explanations
# ---------------------------------------------------------------------------
st.divider()
st.markdown("### How it works")
tab_algo, tab_ds, tab_fare, tab_limits, tab_data = st.tabs(
    ["Algorithm", "Data structures", "Fare rules", "Assumptions & limits", "Network data"]
)

with tab_algo:
    st.markdown(
        """
**Dijkstra's algorithm, run once per transport mode**

1. Set the source's distance to 0 and every other location's distance to ∞.
2. Put `(0, source)` in a **min-heap** (priority queue).
3. Repeatedly pop the location with the **smallest tentative distance**. Skip it if it is a
   *stale* entry (already finalised, or an older, larger distance).
4. **Relax** each edge of the *current mode only*: if `dist[u] + edge.km < dist[v]`,
   update `dist[v]`, record `parent[v] = (u, edge)`, and push `(new distance, v)`.
5. Stop when the destination is popped; its distance is now final.
6. **Rebuild the path** by following `parent` back from the destination, then reverse it.

**Why it is correct:** every edge weight is non-negative, so when a location is popped with
the smallest tentative distance, no other path can later reach it more cheaply.
Ordinary BFS would minimise the *number of legs*, not the total distance, which is wrong
for weighted graphs.

**Complexity:** `O((V + E) log V)` time per run using a binary heap, `O(V + E)` space.
The app runs it 4 times (once per mode). The sample city has V = 12 locations and
E = 94 directed edges.
"""
    )

with tab_ds:
    st.markdown(
        """
| Data structure | Where it is used |
|---|---|
| **Graph (adjacency list)**: `dict[location] → list[Edge]` | Stores locations and their outgoing connections |
| **Edge dataclass**: `to, mode, distance_km, time_min` | One connection for one transport mode |
| **Min-heap** (`heapq`) | Always picks the closest unfinished location |
| **Dictionary** `dist` | Best known distance to each location |
| **Dictionary** `parent` | How each location was reached, for path reconstruction |
| **Set** `visited` | Locations whose distance is final |
| **List + reverse** | Building the route from destination back to source |
| **Sorting** with key `(fare, time, mode)` | Ranking the journey options |
"""
    )

with tab_fare:
    st.markdown("**Illustrative sample fares, not official fares**")
    st.dataframe(
        [
            {"Mode": FARE_RULES[m]["label"], "Base fare (₹)": FARE_RULES[m]["base"],
             "Per km (₹)": FARE_RULES[m]["per_km"]}
            for m in MODES
        ],
        hide_index=True,
    )
    st.markdown(
        """
**Total fare = base fare + per-km rate × total route distance**

* The base fare is charged **once per journey**, not once per road segment.
* Within a mode the per-km rate is constant, so **shortest distance = lowest fare**.
  That is why Dijkstra uses distance as the edge weight and the base fare is added afterwards.
* If tolls, slab fares or per-boarding charges were added, the edge weight would have to
  become the real cost of each edge instead.
"""
    )

with tab_limits:
    st.markdown(
        """
* The city map, distances, times and fares are **sample data**, not live or official values.
* Journey time = sum of connection times. **Waiting, walking, transfers and traffic are not modelled.**
* Each journey uses **one transport mode** from start to finish (no bus → metro changes).
* All connections are **bidirectional** with the same distance and time both ways.
* The recommendation is the cheapest fare. The fastest option is highlighted separately.
"""
    )

with tab_data:
    mode_filter = st.selectbox("Mode", ["All"] + [mode_label(m) for m in MODES])
    st.dataframe(
        [
            {"From": a, "To": b, "Mode": mode_label(m), "km": km, "min": t}
            for a, b, m, km, t in CONNECTIONS
            if mode_filter == "All" or mode_label(m) == mode_filter
        ],
        hide_index=True,
        width="stretch",
    )
    st.caption("Each row is a two-way connection.")
