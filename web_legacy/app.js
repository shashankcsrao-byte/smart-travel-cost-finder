/*
 * Smart Travel Cost Finder: frontend logic.
 *
 * The browser never computes routes itself. It calls the backend REST API
 * (src/server.py), which runs the Python Dijkstra implementation, and then
 * renders the JSON it gets back.
 */
"use strict";

const MODE_ICONS = { bus: "🚌", metro: "🚇", bike_taxi: "🏍️", car_taxi: "🚕" };
const COLORS = { orange: "#F26B1D", charcoal: "#23252B", road: "#C9CBCF", metro: "#7A5AF8", ink: "#2B2D33" };

const state = {
  locations: [],   // [{name, x, y}]
  network: [],     // [{from, to, mode, label, distance_km, time_min}]
  fares: [],       // [{mode, label, base, per_km}]
  result: null,    // last /api/routes response
  selectedMode: null,
};

// Helpers ($, esc, rupees, km, minutes, api) come from common.js.

function showAlert(message, kind = "error") {
  const box = $("#alert");
  box.className = `alert ${kind}`;
  box.textContent = message;
  box.hidden = false;
}

function hideAlert() {
  $("#alert").hidden = true;
}

function setLoading(isLoading) {
  const button = $("#find-btn");
  button.classList.toggle("loading", isLoading);
  button.disabled = isLoading;
}

// ---------------------------------------------------------------------------
// Startup: load reference data from the API
// ---------------------------------------------------------------------------

async function init() {
  bindEvents();
  try {
    const [locations, network, fares] = await Promise.all([
      api("/api/locations"),
      api("/api/network"),
      api("/api/fares"),
    ]);
    state.locations = locations;
    state.network = network;
    state.fares = fares;
  } catch (error) {
    showAlert(error.message);
    return;
  }

  fillLocationSelects();
  renderFareTable();
  renderNetworkTable();
  loadHistory();

  // Support shareable links such as /?from=College&to=Airport
  const params = new URLSearchParams(location.search);
  if (params.get("from") && params.get("to")) {
    search(params.get("from"), params.get("to"), { scroll: false });
  }
}

function fillLocationSelects() {
  const options = state.locations.map((l) => `<option value="${esc(l.name)}">${esc(l.name)}</option>`).join("");
  for (const id of ["#source", "#destination"]) {
    const select = $(id);
    select.innerHTML = options;
    select.disabled = false;
  }
  $("#source").value = "College";
  $("#destination").value = "Airport";
  $("#find-btn").disabled = false;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

function bindEvents() {
  $("#planner-form").addEventListener("submit", (event) => {
    event.preventDefault();
    search($("#source").value, $("#destination").value);
  });

  $("#swap").addEventListener("click", () => {
    const source = $("#source");
    const destination = $("#destination");
    [source.value, destination.value] = [destination.value, source.value];
  });

  document.querySelectorAll(".quick .chip").forEach((chip) => {
    chip.addEventListener("click", () => search(chip.dataset.from, chip.dataset.to));
  });

  $("#cards").addEventListener("click", (event) => {
    const card = event.target.closest(".card[data-mode]");
    if (card) selectMode(card.dataset.mode);
  });

  $("#clear-history").addEventListener("click", async () => {
    try {
      await api("/api/history", { method: "DELETE" });
      loadHistory();
    } catch (error) {
      showAlert(error.message);
    }
  });

  for (const list of ["#recent-list", "#popular-list"]) {
    $(list).addEventListener("click", (event) => {
      const button = event.target.closest("button[data-from]");
      if (button) search(button.dataset.from, button.dataset.to);
    });
  }

  document.querySelectorAll(".tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach((t) => {
        t.classList.toggle("active", t === tab);
        t.setAttribute("aria-selected", t === tab);
      });
      document.querySelectorAll(".tab-panel").forEach((panel) => {
        panel.hidden = panel.dataset.panel !== tab.dataset.tab;
      });
    });
  });

  $("#network-filter").addEventListener("change", renderNetworkTable);
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

async function search(source, destination, { scroll = true } = {}) {
  hideAlert();

  if (source === destination) {
    showAlert("Source and destination are the same, so no journey is needed. Please choose two different locations.", "warn");
    return;
  }

  setLoading(true);
  try {
    const query = new URLSearchParams({ source, destination });
    const result = await api(`/api/routes?${query}`);
    state.result = result;
    state.selectedMode = result.cheapest_mode;
    // Show the server's exact location names (e.g. "college" -> "College").
    $("#source").value = result.source;
    $("#destination").value = result.destination;
    history.replaceState(null, "", `?from=${encodeURIComponent(source)}&to=${encodeURIComponent(destination)}`);
    renderResults();
    loadHistory();
    if (scroll) $("#results-area").scrollIntoView({ behavior: "smooth" });
  } catch (error) {
    showAlert(error.message);
  } finally {
    setLoading(false);
  }
}

// ---------------------------------------------------------------------------
// Rendering results
// ---------------------------------------------------------------------------

function renderResults() {
  const r = state.result;
  $("#empty-state").hidden = true;
  $("#results").hidden = false;
  $("#nav-results").hidden = false;
  $("#results-title").textContent = `${r.source} → ${r.destination}`;
  $("#results-sub").textContent = `${r.options.length} of ${r.options.length + r.unavailable.length} modes available`;

  if (r.options.length === 0) {
    $("#summary").innerHTML = "<b>No transport mode can reach this destination in the sample network.</b>";
    ["#stats", "#cards"].forEach((id) => ($(id).innerHTML = ""));
    return;
  }

  const cheapest = r.options.find((o) => o.mode === r.cheapest_mode);
  const fastest = r.options.find((o) => o.mode === r.fastest_mode);
  const tiedCheap = r.options.filter((o) => o.is_cheapest);
  const tieText = tiedCheap.length > 1 ? ` (same fare: ${tiedCheap.map((o) => esc(o.label)).join(", ")})` : "";
  const tradeOff = cheapest.mode !== fastest.mode && cheapest.time_min > fastest.time_min
    ? ` Choosing the cheapest option adds <b>${minutes(cheapest.time_min - fastest.time_min)}</b> but saves <b>${rupees(fastest.fare - cheapest.fare)}</b> compared with the fastest.`
    : "";
  $("#summary").innerHTML =
    `💡 <b>Recommendation:</b> the cheapest option is <b>${esc(cheapest.label)}</b> at <b>${rupees(cheapest.fare)}</b>${tieText}. ` +
    `The fastest option is <b>${esc(fastest.label)}</b> at <b>${minutes(fastest.time_min)}</b>.${tradeOff}`;

  const shortest = Math.min(...r.options.map((o) => o.distance_km));
  $("#stats").innerHTML = [
    ["Cheapest fare", rupees(cheapest.fare), cheapest.label],
    ["Fastest time", minutes(fastest.time_min), fastest.label],
    ["Shortest distance", km(shortest), "across all modes"],
    ["Modes available", `${r.options.length} / ${r.options.length + r.unavailable.length}`,
      r.unavailable.length ? `No ${r.unavailable.map((u) => u.label).join(", ")}` : "All modes reach it"],
  ].map(([k, v, s]) => `<div class="stat"><div class="k">${k}</div><div class="v">${v}</div><div class="s">${esc(s)}</div></div>`).join("");

  renderCards();
  renderCompareTable();
  renderSelected();
}

function renderCards() {
  const r = state.result;
  const available = r.options.map((o) => `
    <button type="button" class="card ${o.mode === state.selectedMode ? "selected" : ""}" data-mode="${o.mode}"
            aria-pressed="${o.mode === state.selectedMode}">
      <span class="rank">#${o.rank} · SORTED BY FARE</span>
      <span class="mode">${MODE_ICONS[o.mode]} ${esc(o.label)}
        ${o.is_cheapest ? '<span class="badge cheap">CHEAPEST</span>' : ""}
        ${o.is_fastest ? '<span class="badge fast">FASTEST</span>' : ""}</span>
      <span class="fare">${rupees(o.fare)}</span>
      <span class="metrics"><span>📏 ${km(o.distance_km)}</span><span>⏱️ ${minutes(o.time_min)}</span><span>🔁 ${o.legs.length} leg${o.legs.length === 1 ? "" : "s"}</span></span>
      <span class="path">${o.path.map((p) => `<b>${esc(p)}</b>`).join(" → ")}</span>
    </button>`);
  const unavailable = r.unavailable.map((u) => `
    <div class="card off">
      <span class="rank">NOT AVAILABLE</span>
      <span class="mode">${MODE_ICONS[u.mode]} ${esc(u.label)} <span class="badge na">NO ROUTE</span></span>
      <span class="fare">—</span>
      <span class="path">This mode does not connect these two locations in the sample network.</span>
    </div>`);
  $("#cards").innerHTML = available.concat(unavailable).join("");
}

function renderCompareTable() {
  $("#compare-table tbody").innerHTML = state.result.options.map((o) => `
    <tr class="${o.is_cheapest ? "best" : ""}">
      <td>${o.rank}</td>
      <td>${MODE_ICONS[o.mode]} ${esc(o.label)}</td>
      <td class="num"><b>${rupees(o.fare)}</b></td>
      <td class="num">${km(o.distance_km)}</td>
      <td class="num">${minutes(o.time_min)}</td>
      <td>${o.path.map(esc).join(" → ")}</td>
    </tr>`).join("") + state.result.unavailable.map((u) => `
    <tr><td>–</td><td>${MODE_ICONS[u.mode]} ${esc(u.label)}</td><td class="num muted" colspan="3">Not available</td><td class="muted">No route using this mode only</td></tr>`).join("");
}

function selectMode(mode) {
  state.selectedMode = mode;
  document.querySelectorAll(".card[data-mode]").forEach((card) => {
    const on = card.dataset.mode === mode;
    card.classList.toggle("selected", on);
    card.setAttribute("aria-pressed", on);
  });
  renderSelected();
}

function renderSelected() {
  const option = state.result.options.find((o) => o.mode === state.selectedMode);
  $("#map-title").textContent = `Route map · ${option.label}`;
  $("#details-title").textContent = `${MODE_ICONS[option.mode]} ${option.label}: leg-by-leg`;

  $("#legs-table tbody").innerHTML = option.legs.map((leg, i) => `
    <tr><td>${i + 1}</td><td>${esc(leg.from)}</td><td>${esc(leg.to)}</td>
        <td class="num">${leg.distance_km}</td><td class="num">${leg.time_min}</td></tr>`).join("") + `
    <tr><td></td><td colspan="2"><b>Total</b></td><td class="num"><b>${option.distance_km}</b></td><td class="num"><b>${option.time_min}</b></td></tr>`;

  const b = option.fare_breakdown;
  $("#fare-calc").innerHTML = `
    <div><b>Fare</b> = base + per-km × distance <span class="muted">(base charged once)</span></div>
    <div>= ₹${b.base} + ₹${b.per_km} × ${option.distance_km} km</div>
    <div class="total">= ${rupees(option.fare)}</div>
    <div class="muted small">Total time = sum of leg times = ${minutes(option.time_min)}</div>`;

  drawMap(option);
}

// ---------------------------------------------------------------------------
// SVG map (drawing only; routes come from the backend)
// ---------------------------------------------------------------------------

function drawMap(option) {
  const svg = $("#map");
  const scale = 100;
  const pad = 90;
  const xs = state.locations.map((l) => l.x);
  const ys = state.locations.map((l) => l.y);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const width = (maxX - minX) * scale + pad * 2;
  const height = (maxY - minY) * scale + pad * 2;
  const pos = {};
  for (const l of state.locations) {
    // SVG's y axis points down, so flip it.
    pos[l.name] = { x: (l.x - minX) * scale + pad, y: (maxY - l.y) * scale + pad };
  }
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);

  // One line per pair of locations, remembering which modes serve it.
  const pairs = new Map();
  for (const c of state.network) {
    const key = [c.from, c.to].sort().join("|");
    if (!pairs.has(key)) pairs.set(key, { a: c.from, b: c.to, modes: new Set() });
    pairs.get(key).modes.add(c.mode);
  }

  const parts = [];
  for (const { a, b, modes } of pairs.values()) {
    const metroOnly = modes.size === 1 && modes.has("metro");
    parts.push(`<line x1="${pos[a].x}" y1="${pos[a].y}" x2="${pos[b].x}" y2="${pos[b].y}"
      stroke="${metroOnly ? COLORS.metro : COLORS.road}" stroke-width="5" ${metroOnly ? 'stroke-dasharray="14 10" opacity="0.75"' : ""} stroke-linecap="round"/>`);
  }

  const onRoute = new Set(option ? option.path : []);
  if (option) {
    for (const leg of option.legs) {
      const p = pos[leg.from], q = pos[leg.to];
      parts.push(`<line x1="${p.x}" y1="${p.y}" x2="${q.x}" y2="${q.y}" stroke="${COLORS.orange}" stroke-width="14" stroke-linecap="round"/>`);
    }
    for (const leg of option.legs) {
      const p = pos[leg.from], q = pos[leg.to];
      const mx = (p.x + q.x) / 2, my = (p.y + q.y) / 2;
      const text = `${leg.distance_km} km`;
      const w = text.length * 12 + 20;
      parts.push(`<g><rect x="${mx - w / 2}" y="${my - 17}" width="${w}" height="34" rx="17" fill="#fff" stroke="${COLORS.orange}" stroke-width="2.5"/>
        <text x="${mx}" y="${my + 7}" text-anchor="middle" font-size="20" font-weight="700" fill="${COLORS.ink}">${text}</text></g>`);
    }
  }

  const ends = option ? [option.path[0], option.path[option.path.length - 1]] : [];
  for (const l of state.locations) {
    const p = pos[l.name];
    const isEnd = ends.includes(l.name);
    const isOn = onRoute.has(l.name);
    const fill = isEnd ? COLORS.charcoal : isOn ? COLORS.orange : "#fff";
    const stroke = isEnd ? COLORS.orange : COLORS.charcoal;
    const r = isEnd ? 24 : 19;
    parts.push(`<circle cx="${p.x}" cy="${p.y}" r="${r}" fill="${fill}" stroke="${stroke}" stroke-width="${isEnd ? 6 : 3.5}">
      <title>${esc(l.name)}</title></circle>`);
    parts.push(`<text x="${p.x}" y="${p.y + r + 26}" text-anchor="middle" font-size="22" font-weight="${isOn ? 700 : 500}"
      fill="${COLORS.ink}" paint-order="stroke" stroke="#fff" stroke-width="7" stroke-linejoin="round">${esc(l.name)}</text>`);
  }
  svg.innerHTML = parts.join("");
}

// ---------------------------------------------------------------------------
// History, fare table and network table
// ---------------------------------------------------------------------------

async function loadHistory() {
  try {
    const data = await api("/api/history?limit=8");
    $("#recent-list").innerHTML = data.recent.length
      ? data.recent.map((h) => `
        <li><button type="button" data-from="${esc(h.source)}" data-to="${esc(h.destination)}">
          <span>${esc(h.source)} → ${esc(h.destination)}</span>
          <span class="meta">${h.cheapest_mode ? `${MODE_ICONS[h.cheapest_mode]} ${rupees(h.cheapest_fare)}` : "no route"}</span>
        </button></li>`).join("")
      : '<li class="none">No searches yet.</li>';
    $("#popular-list").innerHTML = data.popular.length
      ? data.popular.map((p) => `
        <li><button type="button" data-from="${esc(p.source)}" data-to="${esc(p.destination)}">
          <span>${esc(p.source)} → ${esc(p.destination)}</span>
          <span class="meta">${p.searches} search${p.searches === 1 ? "" : "es"}</span>
        </button></li>`).join("")
      : '<li class="none">No searches yet.</li>';
  } catch (error) {
    $("#recent-list").innerHTML = `<li class="none">${esc(error.message)}</li>`;
    $("#popular-list").innerHTML = "";
  }
}

function renderFareTable() {
  $("#fares-table tbody").innerHTML = state.fares.map((f) => `
    <tr><td>${MODE_ICONS[f.mode]} ${esc(f.label)}</td><td class="num">₹${f.base}</td><td class="num">₹${f.per_km}</td>
        <td class="num">${rupees(f.base + f.per_km * 10)}</td></tr>`).join("");
  $("#network-filter").innerHTML = '<option value="">All modes</option>' +
    state.fares.map((f) => `<option value="${f.mode}">${esc(f.label)}</option>`).join("");
}

function renderNetworkTable() {
  const mode = $("#network-filter").value;
  const rows = state.network.filter((c) => !mode || c.mode === mode);
  $("#network-count").textContent = `${rows.length} two-way connections`;
  $("#network-table tbody").innerHTML = rows.map((c) => `
    <tr><td>${esc(c.from)}</td><td>${esc(c.to)}</td><td>${MODE_ICONS[c.mode]} ${esc(c.label)}</td>
        <td class="num">${c.distance_km}</td><td class="num">${c.time_min}</td></tr>`).join("");
}

init();
