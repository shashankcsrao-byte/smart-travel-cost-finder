# Smart Travel Cost Finder: redesign brief

Interviewed 2026-10-13 (four multiple-choice decisions plus the user's written request).
Topics the user did not answer directly are marked **Authored** and derived from their
answers. The page ships at `web/` (served by the FastAPI backend at `/`), not inside this
folder, because it is a working app with an API. This folder holds the brief and the
verification evidence.

## The user's words

> "redesign full from zero according to the project ... Motion website. More reactive and
> full pop icons and very interactive. Choose colours according to material design rules.
> The best good landing page and use very standard colours of black white ... and most
> transitions and popping up. Make number of clicks less and more attractive."

Chosen options: **Bold & playful** feel · **Planner first, story below** · **Code-drawn
visuals (no image generation)** · signature moment **Dijkstra comes alive**.

## The eight topics

1. **Vibe.** Bold, playful, black and white, popping. Material Design colour rules.
   *Authored references:* a transit departures board (dense, live, legible at a glance),
   Material 3 component behaviour (state layers, ripples, filled-on-select icons).
2. **Journey (user: planner first, story below).** Land on an answer, then see how the answer
   was found, then see why the fares can be trusted, then the system's real numbers, then
   ask again.
3. **Energy curve (authored).** Busy and useful at the top, one dark quiet beat, the loudest
   moment in the middle (the algorithm), calmer and factual after, quiet at the end.
4. **Feeling and the one moment.** See the feeling curve and the peak below.
5. **The thing no other site does (user):** "Dijkstra comes alive": the real algorithm
   visibly spreading over the real metro map as you scroll.
6. **Range (user):** Bold & playful, inside the Material Design colour system.
7. **One world or scenes (authored from "planner first"):** distinct parts of one working
   app. Not a continuous world.
8. **Assets (user):** none. Everything is drawn in code from real data (GTFS metro
   coordinates, API results). No image generation, no spend.

## Grammar: Live surface

The page is the product running. App chrome replaces marketing nav, the hero is the planner
already holding a computed answer, copy speaks in the app's own idiom (labels, status lines,
panel titles), and the page ends on an actual input.

Why the other seven lost: filmic one-shot and continuous world are cinematic journeys with
no tool in them, and the user asked for fewer clicks; chaptered editorial is a reading
format; typographic poster has no room for a working planner; gallery is for a range of
objects; split stage is a two-sided argument (we compare five modes, not two); rhythmic
cutlist suits energy brands and bans the pin the algorithm act needs.

## Journey

1. **Answer**: they open the page and their trip is already compared.
2. **Pause**: the ground goes dark and the app offers to show its working.
3. **Proof**: the real Dijkstra run on the real metro graph, driven by their scroll.
4. **Trust**: every fare rule with its source.
5. **Confidence**: live system telemetry from the backend.
6. **Again**: an input with a cursor: where to next.

## Feeling curve (written before the acts)

```
1  Delight     the answer is already on screen, cards pop in ranked, no click needed
2  Stillness   the page goes dark and almost empty, one line offers to show the working
3  Awe         stations light up one by one across the real map until the route snaps into colour
4  Trust       rate cards slide past, each fare with its formula and its honest source label
5  Confidence  real counters from the live backend tick up in a dense status panel
6  Readiness   one large input with a blinking cursor: where to next
```

Authored silence: act 2 is deliberately near-empty (one line on a dark ground). It is the
silence before the peak, not dead scroll.

## The peak

> "You scroll and the algorithm floods across Bengaluru's actual metro map station by station,
> the heap list shuffles, and then your route snaps into the line colours with its real fare."

Lives in act 3. It gets the largest span on the page and the only full-viewport visual.

## Tell-someone sentence

It's the site where **you scroll and watch the shortest-route algorithm spread across the real
Namma Metro map until your own route lights up, and tapping any station replays it to there.**

## Signature move

**Scroll as the playhead of a real algorithm run.** The page asks the backend to run the
project's own `dijkstra()` with tracing on the real 251-state metro graph, receives every pop,
relaxation and heap snapshot, and maps act progress to the event index. Stations settle, the
frontier advances, the heap panel shows the true min-heap order, and the log fills. Tapping a
station re-runs the trace to that destination. A slider mirrors the playhead for keyboard and
reduced-motion visitors.

## Colour (Material Design rules)

Neutral black and white roles with one Material Indigo accent, two lightnesses keyed to the
ground (the page hard-cuts between light and dark grounds):

| Role | Light ground | Dark ground |
|---|---|---|
| canvas | `#F7F7FA` | `#111318` |
| surface | `#FFFFFF` | `#1B1E25` |
| ink | `#16181D` | `#F2F3F7` |
| ink-soft | `#4A4E59` | `#B9BDC9` |
| accent | `#304FFE` (Indigo A700) | `#AAB6FF` (Indigo A100 tone) |
| accent-ink | `#FFFFFF` | `#0B1033` |

Data colours that are not accents: the three metro line colours from the GTFS feed, and
Material status tones for fare-source labels (official, govt rate, estimated, indicative).

Type: Outfit (display) and Geist (text). Icons: Material Symbols Rounded, with the FILL axis
animated on selection.

## Score (device per beat)

| Beat | Act | Device | Span | Why this one |
|---|---|---|---|---|
| Answer | Planner hero | `flow` (natural, not pinned) | ~1.3 vh | A working surface should scroll like a document; the answer is already in it |
| Pause | Dark curtain | `reveal` (wipe up) | ~0.8 vh | A change of ground is a change of state: from using to understanding |
| Proof (peak) | Dijkstra run | `pin` + page JS on `--sc-p` | 4.8 | The surface holds still while its state advances; largest span on the page |
| Trust | Rate cards | `pan` | 4.2 | Five modes side by side read as options, not as an argument |
| Confidence | System status | `flow` + `in` + `count` | ~1 vh | Real telemetry from the backend, counted in once |
| Again | Input | `pin` | 1.2 | The surface hands over an input and holds |

Five device families (flow, reveal, pin, pan, count), never the same twice in a row. Grounds
are painted per section (light, dark, dark, light, dark, light) with hard edges; no drift.

## Feel check (after the first verification run)

Scrolled the contact sheets cold, one word per act, then compared with the curve above.

| Act | Intended | Felt | Change |
|---|---|---|---|
| 1 | Delight | Useful | Static frames cannot show the pops; confirmed live in the browser that cards, icons and badges spring in. No change |
| 2 | Stillness | Pause | Matches |
| 3 | Awe | Fascination | Matches; the peak is the largest visual change and the longest span on every sheet |
| 4 | Trust | Clear | Matches |
| 5 | Confidence | Proof | Matches |
| 6 | Readiness | Slightly empty | Shortened the close span from 1.3 to 1.2 so the hold is shorter |

Bugs found by verification and fixed: the map route was invisible (engine reset and a page
rule squeezed Leaflet's SVG layer to 0 px); on phones the final result card overflowed its
panel and the fare bubble covered a label; the heap snapshot was taken before neighbours were
pushed; the closing input did not land on the new results.
