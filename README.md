# Smart Travel Cost Finder

**A Graph-Based Multi-Modal Transport Cost Optimization System**

A full-stack web application (HTML/CSS/JS frontend → FastAPI REST backend → Python
routing engine → SQLite database) that compares the cheapest way to travel by bus,
metro, auto, bike taxi and cab. The core routing uses a **hand-written Dijkstra's
algorithm** with a binary min-heap.

It has two modes:

| | **Live Bengaluru** | **Demo city (offline)** |
|---|---|---|
| Places | Any place in Bengaluru (Ola Maps search) | 12 sample locations |
| Road distance and time | **Live, with traffic** (Ola Maps) | Sample data |
| Metro | **Real Namma Metro network** (83 stations, 3 lines). Dijkstra on station + line states | Sample metro edges |
| Fares | Official metro fares, Govt-notified auto and cab rates, partial BMTC fares, indicative bike taxi | Illustrative fare rules |
| Needs | Internet + a free Ola Maps API key | Nothing (fully offline) |

> **Live prices from Uber, Ola, Rapido and Namma Yatri are not available to other apps.**
> Uber no longer issues the server tokens its price-estimate API needs, and Ola, Rapido and
> Namma Yatri have no public fare API. So road fares are **estimates from published rates**,
> clearly labelled, and every option has a button that opens the real app for the exact price.

---

## Setup (one time)

Requires Python 3.10 or newer. Run these commands from the `smart-travel-cost-finder` folder.

**macOS / Linux**
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

**Windows (PowerShell)**
```powershell
py -m venv .venv
.venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

### Turn on live mode
1. Create a free account at [maps.olakrutrim.com](https://maps.olakrutrim.com) and create an **API key**.
2. Copy `.env.example` to `.env` and paste the key:
   ```
   OLA_MAPS_API_KEY=your-key-here
   ```
3. Check it works: `python -m src.live.check` (makes 3 small test calls).
4. Restart the server. The key stays on the server; the browser never sees it.

## Run

| What | Command | Open |
|---|---|---|
| **Web app** (main) | `python -m src.server` | http://localhost:8000 |
| API docs | (same server) | http://localhost:8000/docs |
| Terminal app (demo data) | `python -m src.main` | |
| Streamlit dashboard (demo data) | `streamlit run app.py` | http://localhost:8501 |
| Tests | `python -m pytest -v` | |

## Architecture

```
Browser  web/index.html · common.js · app.js (demo) · live.js (live) · Leaflet map
   │  fetch("/api/live/plan?from_lat=…")          fetch("/api/routes?source=…")
   ▼
FastAPI  src/server.py ─ validation, errors (404/422/502/503), JSON
   │
   ├─► src/live/planner.py ─┬─► src/live/olamaps.py   Ola Maps: places, directions, distance matrix
   │                        │       (TTL + LRU cache: src/live/cache.py)
   │                        ├─► src/live/metro.py     real metro graph → routing.dijkstra()
   │                        └─► src/live/fares.py     published fare rules with sources
   ├─► src/routing.py  dijkstra(), compare_routes()   (demo city: src/graph.py, src/fares.py)
   └─► src/history.py  SQLite: searches, live_searches (parameterised queries)
```

### API endpoints

| Endpoint | Purpose |
|---|---|
| `GET /api/live/status` | Is live mode configured? Is metro data loaded? Data sources |
| `GET /api/live/places?q=` | Place suggestions (proxied to Ola Maps) |
| `GET /api/live/place?place_id=` | Coordinates of a suggestion |
| `GET /api/live/plan?from_lat&from_lng&from_name&to_lat&to_lng&to_name&priority=&at=` | Ranked options for a real trip (priority: cheapest, balanced, fastest; at: leave time HH:MM) |
| `GET /api/live/metro` | Metro stations and lines (for the map) |
| `GET /api/live/history` | Recent live trips |
| `GET /api/live/metro/trace?from=…&to=…&at=HH:MM&algorithm=dijkstra\|astar` | Every step of the timetable search (website replay) |
| `GET /api/live/reverse?lat=…&lng=…` | Name of a point picked on the map |
| `GET /api/routes/alternatives?source=…&destination=…&mode=…&k=3` | Yen's k-shortest routes for one mode (demo) |
| `GET /api/live/fares` | Live-mode fare rules with their sources |
| `GET /api/routes?source=…&destination=…&priority=…` | Demo city: every mode, quicker routes and a Smart mix, ranked by priority |
| `GET /api/locations` · `/api/network` · `/api/fares` | Demo city data |
| `GET /api/history` · `DELETE /api/history` | Search history |

## Version 2 (after the full audit)

**Algorithms (all hand-written, all tested):**
- **Dijkstra** with a time tie-break: among equally short routes the quicker one wins.
- **Time-dependent Dijkstra on the real metro timetable:** binary search finds the next train
  from each station. It handles weekday, Monday, Sunday and holiday timetables, and trains
  running after midnight. It answers "board 09:09, arrive 10:20".
- **A\*** with an admissible straight-line ÷ top-speed heuristic: the same answer while
  settling about 25% fewer states. The website lets you switch between Dijkstra and A\*.
- **Yen's k-shortest paths:** "Other routes" for a mode (demo city).
- **Mixed-mode Dijkstra** on (location, mode) states, paying a base fare per boarding:
  "Smart mix: Bike Taxi → Metro".
- **Priority:** Cheapest, Balanced or Fastest. It converts time to money (₹0 / ₹2 / ₹50 per
  minute) for ranking and for the mixed-mode search.
- **Exactly one Cheapest and one Fastest:** ties go to the cheaper option, then a fixed mode
  order.

**Reliability and security:**
- The cache is thread-safe, and a sliding-window rate limiter protects the Ola quota.
- Bad data from Ola Maps becomes a clean error instead of a crash; status codes are honest.
- The API key never appears in logs.
- Requests are accepted only from trusted hosts, and cross-site requests are refused.
- Responses carry a Content-Security-Policy and other security headers.
- Clearing history requires the header `X-Requested-With: smart-travel`.
- Repeated searches are not stored twice, and history is capped at 500 rows.
- Fonts, icons and the map library are bundled in `web/vendor/`, so the page works offline.
  Rebuild them with `python tools/build_vendor.py`.

**Website:**
- **Priority switch** (Cheapest / Balanced / Fastest) and a **Leave at** time.
- **Saved trips (★) and recent trips,** stored on this device only.
- **Share link:** the trip is kept in the URL.
- **Tap the map** to set a start or destination.
- Metro lines are drawn on the live map, and short trips get a free Walk option.
- **Replay controls:** play, pause, step, Dijkstra / A\*.
- **Fixes:**
  - The replay now stays in sync with scrolling on phones.
  - Async race conditions are guarded.
  - Mobile maps no longer trap one-finger scrolling.
  - Keyboard and screen-reader access is improved.

## The website (redesign, Oct 2026)

Built with the scroll-craft process (brief, grammar, feeling curve, verification). The brief,
score, feel check and screenshot evidence are in `scrollcraft/builds/smart-travel/`.

- **Planner first.** The page opens with a trip already compared (your last live trip, or a
  sample one). Picking a place runs the comparison at once, with no "Find" button. Quick-trip
  chips, a swap button and "use my location" each take one click.
- **Results** pop in ranked by fare, with Cheapest and Fastest badges and a label showing
  where each fare comes from. Clicking a card selects it, expands its steps and app buttons,
  and redraws the map.
- **Dijkstra, live (the centrepiece).** Scrolling plays back the real run of `dijkstra()` on
  the real 251-state metro graph: stations settle one by one, the min-heap and log update, then
  the route lights up in its line colours with the official fare. Tap any station, or use the
  selects, to replay to it. The slider is the same playhead for keyboard users.
- **Fares**, the rate cards with sources, slide sideways. **System** shows real counts read
  from the backend. The page ends on a "Where to next?" input.
- Material Design roles on black and white with one Indigo accent, Material Symbols icons,
  Outfit and Geist type. Respects reduced motion. Works on phones.
- The previous frontend is kept in `web_legacy/` for reference.

## Live mode: what each number means

| Option | Route and time | Fare | Label shown |
|---|---|---|---|
| **Namma Metro** | Walk (≤ 1 km) or auto to one of the 3 nearest stations → **our Dijkstra** on the real network → walk or auto to the destination. Includes average waiting time (half the line's headway, from the timetable) and line changes | Official BMRCL station-to-station fare (token/QR). Smart-card peak and off-peak prices shown too | Official fare |
| **Auto** (Namma Yatri, meter autos) | Live road route (Ola Maps) | Govt meter rate (Aug 2025): ₹36 for 2 km + ₹18/km, +50% from 10 PM to 5 AM | Govt rate |
| **Cab** (Uber, Ola) | Live road route | Govt uniform taxi fare (Feb 2025): ₹100 for 4 km + ₹24/km. Apps may add surge or fees | Govt rate |
| **Bike taxi** (Rapido, Uber Moto) | Live road route | No official rate card, so indicative ₹20 + ₹8/km (edit `src/live/fares.py`) | Indicative |
| **BMTC bus** | Road time × 1.5 + 8 min average wait (estimate) | ₹6 / ₹12 / ₹18 up to 6 km (Jan 2025). Longer trips are **estimated** as the 2020 slabs × 1.2 (the verified ratio): ≈₹24 up to 14 km, ≈₹30 up to 40 km, ≈₹36 beyond | Official / Estimated |

Other live behaviour: metro is marked unavailable outside its operating hours (from the
timetable) or when both places are nearest the same station. Places outside Bengaluru
are rejected. Results can auto-refresh every 2 minutes, and "Use my location" uses the
browser's location.

## The core algorithm (both modes)

- The graph is an **adjacency list** `dict[location] → list[Edge]`.
- **Dijkstra** uses `heapq` with lazy deletion of stale entries, a `parent` map for path
  reconstruction, and early exit at the destination. It takes `O((V + E) log V)` time.
- Demo city: one run per mode, minimising **distance** (fare = base + per_km × distance,
  so shortest = cheapest).
- Live metro: the state graph has nodes `IN:station`, `station@LINE` and `OUT:station`
  (251 states). Dijkstra minimises **time**, because metro fares depend only on the start
  and end stations, and a line change at Majestic or RV Road costs walking and waiting time.

## Project structure

```
smart-travel-cost-finder/
├── web/                     # Frontend: index.html, styles.css, app.js, scrollcraft.js/.css (scroll engine)
├── src/
│   ├── graph.py             # demo city graph (adjacency list)
│   ├── fares.py             # demo fare rules
│   ├── routing.py           # dijkstra(), compare_routes()
│   ├── server.py            # FastAPI backend
│   ├── history.py           # SQLite history
│   ├── main.py              # terminal app
│   ├── visualize.py         # Matplotlib map for Streamlit
│   └── live/
│       ├── ratelimit.py     # sliding-window rate limiter
│       ├── settings.py      # .env loading, paths
│       ├── olamaps.py       # Ola Maps client
│       ├── cache.py         # TTL + LRU cache
│       ├── metro.py         # real Namma Metro graph, fares, hours
│       ├── fares.py         # published Bengaluru fare rules
│       └── planner.py       # builds and ranks live options
├── data/bmrcl-gtfs/         # Namma Metro GTFS (ODbL) + attribution
├── tests/                   # 119 tests (no API key or internet needed)
├── tools/build_vendor.py    # copies fonts/icons/Leaflet into web/vendor (offline)
├── docs/VIVA_NOTES.md
├── app.py                   # Streamlit dashboard
├── .env.example             # copy to .env and add your key
└── requirements.txt
```

## Tests

`python -m pytest -v` runs 119 tests offline. Ola Maps is replaced by a fake HTTP
transport, and the metro tests use the real GTFS file. They cover Dijkstra (including a
brute-force check), fares, the TTL cache, Ola Maps response parsing and errors, real metro
routes and interchanges, official fare lookup, operating hours, the live planner, and every
API endpoint.

## Data sources and credits

- Ola Maps (places, routing with traffic): https://maps.olakrutrim.com
- Namma Metro GTFS by Vonter, https://github.com/Vonter/bmrcl-gtfs (ODbL 1.0), based on BMRCL timetables and OpenStreetMap
- Map tiles © OpenStreetMap contributors (Leaflet)
- Fare rules: Govt. of Karnataka notifications (auto Aug 2025, taxi Feb 2025), BMTC Jan 2025 revision (first stages)

## Limitations

1. Road fares for Uber, Ola, Rapido and Namma Yatri are estimates. Exact prices are only in their apps.
2. Bus routing is estimated from road time, not from BMTC route data. Bus fares above 6 km are estimates (shown as "≈").
3. Metro times come from the timetable plus average waiting time, not live train positions.
4. Live mode covers Bengaluru only. The demo city works anywhere, offline.
