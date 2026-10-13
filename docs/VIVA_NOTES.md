# Viva Notes: Smart Travel Cost Finder

## 1. The problem in one paragraph
A traveller can choose between several transport modes, and each mode has its own
routes, distances, fares and times. We model the city as a **weighted graph**. For
**each mode separately** we run **Dijkstra's algorithm** to find the minimum-distance
route, which is also the minimum-fare route for that mode. We then price each route,
add up its travel time, **sort the options by fare**, and highlight the cheapest and
fastest options.

## 2. Key terms

| Term | Meaning in this project |
|---|---|
| **Vertex (node)** | A location, e.g. `"College"`. There are 12. |
| **Edge** | A connection from one location to another for one mode, with `distance_km` and `time_min`. |
| **Weighted graph** | Each edge has a cost (here: distance). |
| **Adjacency list** | `graph = {location: [Edge, Edge, ...]}`, a list of outgoing edges per vertex. |
| **Bidirectional** | Each connection in `CONNECTIONS` becomes two directed edges, A→B and B→A. |
| **Min-heap** | A binary tree stored in a list, where the smallest item is always at the top. `heapq.heappush` and `heappop` are O(log n). |
| **`dist` dictionary** | The best distance found so far to each location. It starts at ∞, and the source starts at 0. |
| **`parent` dictionary** | `parent[v] = (u, edge)` means "the best known way to reach v is from u using this edge". |
| **Relaxation** | If `dist[u] + w(u,v) < dist[v]`, then update `dist[v]` and `parent[v]`, and push v. |
| **Stale entry** | An old, larger `(cost, node)` left in the heap after a better cost was found. It is skipped when popped. |
| **Path reconstruction** | Follow `parent` from the destination back to the source, then reverse the list. |

### Why an adjacency list and not an adjacency matrix?
A matrix needs V × V cells (144 here) even though most pairs are not connected. It also
cannot easily store several modes between the same pair. An adjacency list stores only
the edges that exist, in O(V + E) space, and lets us loop over the neighbours of a node
directly.

## 3. Dijkstra, step by step (code in `src/routing.py`)

```
dist = {every location: ∞};  dist[source] = 0
parent = {};  visited = set();  heap = [(0, source)]
while heap:
    cost, u = heappop(heap)
    if u in visited or cost > dist[u]: continue      # stale entry
    visited.add(u)
    if u == destination: break                       # distance is now final
    for edge in graph[u]:
        if edge.mode != mode: continue               # mode filtering
        new = cost + edge.distance_km
        if new < dist[edge.to]:                      # relaxation
            dist[edge.to] = new
            parent[edge.to] = (u, edge)
            heappush(heap, (new, edge.to))
if dist[destination] == ∞: return None               # unreachable
walk parent back from destination, then reverse       # path + totals
```

### Hand trace: College → Airport by **metro**
Metro edges: College–Metro Station 2 · Metro Station–City Centre 6 · Metro Station–Railway 4 ·
Railway–City Centre 3 · City Centre–Tech Park 7 · Tech Park–Airport 12

| Pop | Relaxations | Heap afterwards |
|---|---|---|
| (0, College) | Metro Station = 2 | (2, Metro Station) |
| (2, Metro Station) | City Centre = 8, Railway Station = 6 (College 4 > 0, so no change) | (6, Railway), (8, City Centre) |
| (6, Railway Station) | City Centre: 6 + 3 = 9, which is not < 8, so no change | (8, City Centre) |
| (8, City Centre) | Tech Park = 15 | (15, Tech Park) |
| (15, Tech Park) | Airport = 27 | (27, Airport) |
| (27, Airport) | destination popped, so stop | |

Parents: Airport ← Tech Park ← City Centre ← Metro Station ← College.
After reversing: **College → Metro Station → City Centre → Tech Park → Airport**, 27 km.
Time = 5 + 10 + 12 + 18 = **45 min**. Fare = 10 + 3 × 27 = **₹91**.

### Stale entry example (from the tests)
Edges: A–C 10 km, A–B 1 km, B–C 1 km. From A, C is first pushed as `(10, C)`. Later,
via B, it is improved and `(2, C)` is pushed. `(2, C)` is popped first and finalises C.
When `(10, C)` is popped later, `10 > dist[C] = 2`, so it is skipped.

## 4. Why Dijkstra is correct, and why BFS is not enough

**Key idea:** when we pop the node `u` with the smallest tentative distance, any other
path to `u` must leave the set of finished nodes through some node `x` whose distance is
**≥ dist[u]**. All edge weights are **non-negative**, so continuing from `x` can only add
distance. That means no other path to `u` can be shorter, and `dist[u]` is final.

**Negative weights break this:** a negative edge found later could make an already
finished node cheaper. Bellman-Ford handles negative weights in O(V·E).
`build_graph()` rejects negative distances for this reason.

**BFS** finds the path with the **fewest edges**. That is only the shortest path when all
edges have equal weight. Example from the tests: the direct A–C edge is 5 km, while
A–B–C is 1 + 1 = 2 km. BFS picks A–C (1 edge), but Dijkstra correctly picks A–B–C.

## 5. Complexity

Let V = number of locations and E = number of directed edges.

| | Cost |
|---|---|
| Building the adjacency list | O(V + E) |
| One Dijkstra run | **O((V + E) log V)** time, **O(V + E)** space |
| One query (4 modes) | 4 Dijkstra runs, plus sorting 4 options |

**Explanation:** each edge is relaxed at most once per pop of its source node, giving
O(E) pushes. Each push or pop costs O(log(heap size)). The heap can hold up to E entries
because of lazy deletion, so each operation is O(log E), and log E ≤ 2 log V. In this
simple version every run scans all edges of a node and skips the other modes, so E here
means all edges, not just one mode's edges.

Sample city: V = 12 and E = 94 directed edges (47 two-way connections).

## 6. The fare model and why distance can be the weight
`fare = base + per_km × distance`, where base and per_km are constant for a mode and
per_km > 0. Fare rises whenever distance rises, so **the minimum-distance route is the
minimum-fare route**. The base fare is added **once**, after Dijkstra finishes.

If the base fare were added on every edge, a 5-leg bus trip would pay ₹25 in base fares
instead of ₹5. That would also bias the search towards routes with fewer, longer legs.
This is tested in `test_base_fare_charged_once_for_multi_edge_route`.

**When this assumption breaks:** tolls on particular roads, slab fares (e.g. ₹10 for the
first 2 km), or a charge per boarding. In those cases the edge weight must be the real
cost of each edge, or the state must be extended (see section 8).

## 7. Cheapest vs fastest
- Ranking uses `(fare, time, mode order)`: fare first, the faster option on equal fare,
  and a fixed mode order as the final tie-breaker.
- **Fastest** is chosen separately by `(time, fare, mode order)`.
- College → Airport: **Bus is cheapest (₹57, 88 min)** and **Metro is fastest (45 min, ₹91)**.
- Within one mode, cheapest and fastest can also differ. By car, the cheapest route is
  25 km / 54 min, but `dijkstra(..., weight="time_min")` finds the ring road plus
  expressway route at 28 km / 43 min.

## 8. Extension: mixed-mode journeys (design)
Per-mode routing answers: *"What is the cheapest way to go **only by bus**?"*
Mixed-mode routing answers a different question: *"What is the cheapest journey if I
may **change modes**?"* (e.g. bus → metro → bus).

- **State** = `(location, current_mode)` instead of just `location`. The same place
  reached by bus and reached by metro are different states, because the next fare
  depends on whether you are already on board.
- **Moves:**
  - Continue on the same mode: cost = `per_km × edge distance`.
  - Board a different mode at the current location: cost = that mode's **base fare**,
    plus an optional transfer or waiting penalty if optimising time.
  - Start: from `(source, None)`, boarding any mode costs its base fare.
- **Goal:** any state `(destination, m)`.
- The state graph has V × (modes + 1) states, so complexity becomes
  O((V·M + E·M) log(V·M)). All costs are still non-negative, so Dijkstra is still correct.
- The fare rules must be stated explicitly, for example "the base fare is paid again
  every time you board".

## 9. Likely viva questions

1. **Why Dijkstra and not BFS?** BFS minimises the number of edges, not the total
   weight. Our edges have different distances. (Section 4.)
2. **Why does Dijkstra need non-negative weights?** A finished node's distance must never
   decrease later. (Section 4.)
3. **What is the time complexity?** O((V + E) log V) per run with a binary heap.
4. **Why a heap?** Finding the minimum by scanning a list each time costs O(V), giving
   O(V²) overall. A heap gives each minimum in O(log V).
5. **How do you handle the heap not supporting decrease-key?** Lazy deletion: push a new
   entry and skip stale ones when they are popped.
6. **How do you reconstruct the path?** Use the `parent` dictionary, walking backwards
   from the destination, then reverse the list.
7. **Why store the edge in `parent` and not just the previous node?** So the time of the
   exact edge used can be added up, even if two edges join the same pair of locations.
8. **How do you stop modes from mixing?** Dijkstra skips every edge where
   `edge.mode != mode`.
9. **What if the destination is unreachable?** `dist[destination]` stays ∞, Dijkstra
   returns `None`, and the mode is listed as unavailable.
10. **Why is the base fare added only once?** Because a passenger pays it once per
    journey. Adding it per edge would overcharge and bias the route choice.
11. **Why can you minimise distance instead of fare?** Fare grows monotonically with
    distance within a mode. (Section 6.)
12. **Is the graph directed?** It is stored as directed edges, and each sample
    connection is added in both directions. `build_graph(bidirectional=False)` supports
    one-way roads.
13. **How did you test correctness?** 40+ pytest tests: hand-checked cases, edge cases,
    and a brute-force search over every simple path for every location pair and every
    mode, compared with Dijkstra's answer.
14. **What if two routes cost the same?** The heap breaks ties by location name, so
    results are deterministic. Fare ties are ranked by time, and all tied options are
    reported.
15. **Did you use NetworkX for the routes?** No. NetworkX and Matplotlib only draw the
    map in the dashboard. Routes come from our own `dijkstra()`.
16. **What are the limitations?** Sample data, no waiting or traffic, and single-mode
    journeys only. (README.)
17. **How would you use real data?** Load stops and routes from open transit data
    (GTFS) and road distances from map data into the same adjacency-list format.
18. **What is the space complexity?** O(V + E) for the graph, plus O(V) for `dist`,
    `parent` and `visited`, plus up to O(E) heap entries.

## 10. Full-stack questions
19. **What is the architecture?** The browser frontend (HTML/CSS/JS) calls the REST API
    (FastAPI) over HTTP and gets JSON back. The API calls the routing engine (Dijkstra) and
    saves each search in SQLite. Each layer has one job.
20. **Why REST/JSON?** Any client can use the same API: the web page, a mobile app, or
    the tests. The algorithm stays on the server and is written once.
21. **What happens with an invalid location?** The API returns HTTP 404 with a message,
    and the frontend shows it in a red alert. A missing parameter gives HTTP 422.
22. **How do you prevent SQL injection?** Every query uses `?` placeholders, so user text
    is never pasted into SQL. A test inserts `x'); DROP TABLE searches; --` and checks
    that it is stored as plain text.
23. **How do you prevent HTML injection in the frontend?** All text goes through `esc()`
    before it is inserted into HTML.
24. **Why SQLite?** It is built into Python, stored in a single file and needs no server.
    That is enough for search history in a local app.
25. **Why does the frontend not run Dijkstra itself?** The algorithm, data and fare rules
    stay in one place (the backend). The browser only presents the results.

## 11. Demo script (about 4 minutes)
0. Web app: `python -m src.server`, then open http://localhost:8000. Click *College →
   Airport*, point out the cheapest and fastest badges, click the Metro card to redraw the
   map, and show the history. Open `/docs` and run `GET /api/routes` live.
1. Terminal: `python -m src.main`, then option 3 (fare rules), then option 2, College →
   Airport. Point out that the bus is cheapest and the metro is fastest.
2. Option 2 again, Hospital → Airport. Point out that the metro is not available.
3. Option 2 with an invalid name (e.g. "Mars"). Point out the friendly error.
4. Dashboard: `streamlit run app.py`. Show the cards, the comparison table, the route map
   and the leg-by-leg fare calculation.
5. Tests: `python -m pytest -v`. All pass, including the brute-force check.
6. Open `src/routing.py` and walk through `dijkstra()` using the hand trace in section 3.

## 12. Live Bengaluru mode

### How the real metro is modelled (state graph)
A plain station graph would make line changes free. Instead each station has one node
per line, plus entry and exit nodes:

```
IN:KGWA ──wait 2.6──► KGWA@PURPLE ──ride──► MGRD@PURPLE ──► OUT:MGRD
                        │ change: walk 1 + wait 3.1
                        ▼
                      KGWA@GREEN ──ride──► ...
```
- Ride edges: real km and minutes between stations, from the timetable (`stop_times.txt`).
- Waiting time: half the line's weekday daytime headway, computed from the timetable.
- Line change: 1 min walk (GTFS `transfers.txt`) plus waiting for the other line.
- 83 stations become 251 states. The same `dijkstra()` function runs with `weight="time_min"`.

**Why minimise time, not distance?** BMRCL fares depend only on the start and end
stations (a fixed table), not on the path. So within the metro the best path is the
quickest one.

### First and last mile
The 3 nearest stations to each end are found with the **haversine** formula, sorted:
O(S log S) for S = 83. One Ola Maps distance-matrix call gets real road distances to
those stations. A leg of 1 km or less is walked (4.5 km/h); a longer one takes an auto at
the meter rate. All 3 × 3 station pairs are tried (9 Dijkstra runs), and the cheapest
combination is kept, with time as the tie-break.

### Viva questions
26. **Is this real-time?** Road times come from Ola Maps with current traffic, at the
    moment you search, and can auto-refresh every 2 minutes. Metro times come from the
    official timetable. Fares are official or published rates, not live app prices.
27. **Why not real Uber or Ola prices?** Uber no longer issues the server tokens its
    price-estimate API needs. Ola, Rapido and Namma Yatri have no public fare API, and
    scraping would break their terms. We estimate from published rates, label each
    estimate, and link to the apps.
28. **Where is the API key kept?** In `.env` on the server. The browser calls our own
    `/api/live/...` endpoints, and the backend adds the key. If the key were in
    JavaScript, anyone could copy it.
29. **What is the TTL + LRU cache?** An `OrderedDict` used as a hash map plus a linked
    list: O(1) get and set, the least recently used entry is evicted when full, and
    entries expire after 60 s for routes (so traffic stays current) or 6 h for place
    searches. It saves API quota and makes repeat searches fast.
30. **What is debouncing?** The place-search box waits 300 ms after the last keystroke
    before calling the API, so typing "Koramangala" makes 1 request instead of 11. A
    request counter discards out-of-order responses.
31. **How do you know the metro route is correct?** The tests check that Whitefield →
    Silk Institute changes from Purple to Green at Majestic, that the total time is more
    than the riding time (so waiting is counted), that fares match the official table,
    and that the metro is closed at 2 AM.
32. **What does each fare label mean?** Official: the operator's own table (metro).
    Govt rate: a state-notified rate (auto, cab). Estimated: derived from older official
    data (BMTC beyond 6 km = 2020 slab × 1.2, because the verified 2025 stages
    ₹6/₹12/₹18 are exactly 1.2 × the 2020 stages ₹5/₹10/₹15). Indicative: no official rate exists (bike taxi).

### Demo script for live mode (2 minutes)
1. Switch to **Live Bengaluru**, then search *Koramangala → Whitefield*. Point out the
   live drive time and the five ranked options with their fare labels.
2. Click the **Namma Metro** card. The map draws the real Yellow → Green → Purple route,
   and the steps show the auto first mile, line changes and the official fare with the
   smart-card price.
3. Click **Uber ↗** to show the app opening with the trip pre-filled, for the exact price.
4. Open the *Live data* tab to explain what is live, what is official and what is estimated.


## 13. The website's Dijkstra replay
33. **Is the animation fake?** No. The page calls `/api/live/metro/trace`, which runs the same
    `dijkstra()` with an optional `trace` list. Each time a node is settled, the function
    records the node, its cost, the neighbours it relaxed, and the 4 smallest live heap entries
    (stale entries are left out). The result is identical with or without tracing, and a test
    checks this. The browser only maps scroll position to an event number and draws it.
34. **What do you see on screen?** White stations are settled (final distance known). Ringed
    stations are on the frontier (in the heap). The pulsing one was just popped. The heap list
    is the real min-heap order. The run stops when the destination is popped, which is the
    early exit in the code.
35. **Why does the run explore stations in the wrong direction?** Dijkstra has no sense of
    direction. It settles stations in order of time from the start, so nearby stations in every
    direction come first. (A* would use a distance estimate to aim at the goal.)


## 14. Version 2 algorithms (after the audit)

**Time-dependent Dijkstra (metro timetable).** A state is (station, line) at a time t.
The next train in each direction is found by **binary search** (`bisect`) over that
station's sorted departure times: O(log n). The cost of an edge depends on *when* you
arrive, because you may have to wait. Dijkstra is still correct because the network is
**FIFO**: arriving earlier never makes you arrive later. Trains after midnight are stored
as times like 24:20 in the previous day's service, so the search checks today's services
plus yesterday's shifted back by 24 h.

**A\*.** Priority = cost so far + h(node). With h = straight-line distance ÷ the fastest
speed of any train, h never overestimates, so A\* is optimal. It settles fewer states, for
example 56 instead of 75 for Whitefield → Silk Institute.

**Yen's k-shortest paths.** To find the next best route, take each node of the previous
route as a "spur", keep the part before it, ban the edges earlier routes used next, and run
Dijkstra for the rest. The cheapest candidate becomes route k+1. The test compares the
results with brute force.

**Mixed-mode Dijkstra.** A state is (location, current mode). Boarding a new mode costs
its base fare (plus 5 min if it is a change). Riding costs per-km × km. The cost minimised
is fare + λ × minutes, where λ comes from the priority (₹0 / ₹2 / ₹50 per minute). The
costs are non-negative, so Dijkstra applies.

**Ties.** Costs are pairs, (distance, time) or (time, distance), so ties are broken by the
second measure. Python tuples compare element by element, so the heap code did not change.

**Sliding-window rate limiter.** Each IP has a deque of request times. Old times are
dropped from the left, and a request is allowed if fewer than the limit remain. This is
O(1) amortised.

### Viva questions (v2)
36. **Why is time-dependent Dijkstra correct?** It is correct because of the FIFO
    property, and because waiting is never worse than leaving later.
37. **What makes an A\* heuristic admissible?** It must never overestimate the remaining
    cost. Ours uses top speed, so the real time is always at least the estimate.
38. **Why is "Fastest" now the car's ring-road route?** Each mode now also gets its
    quickest route, not only its cheapest. The badge goes to the single quickest journey.
39. **What is the trade-off between the Cheapest and Balanced priorities?** Balanced
    counts each minute as ₹2. Metro (₹91, 45 min) then beats Bus (₹57, 88 min), because
    91 + 90 < 57 + 176.
