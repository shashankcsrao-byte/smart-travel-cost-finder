/*
 * Smart Travel · frontend
 *
 * The browser never computes routes or fares. Everything comes from the
 * FastAPI backend (src/server.py), which runs the project's own Dijkstra.
 * This file only renders, animates and wires up interaction.
 */
(function () {
  "use strict";

  // ------------------------------------------------------------------ utils
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;

  /** Escape text before putting it into HTML (prevents HTML injection). */
  const esc = (v) => String(v ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const rupees = (n) => (Number.isInteger(n) ? `₹${n}` : `₹${Number(n).toFixed(2)}`);
  const km = (v) => (Number.isInteger(v) ? `${v} km` : `${Number(v).toFixed(1)} km`);
  const minutes = (v) => {
    const t = Math.round(v), h = Math.floor(t / 60), m = t % 60;
    return h ? (m ? `${h} h ${m} min` : `${h} h`) : `${m} min`;
  };
  const icon = (name, cls = "") => `<span class="ms ${cls}" aria-hidden="true">${name}</span>`;

  async function api(path, options) {
    let res;
    try { res = await fetch(path, options); } catch {
      throw new Error("Cannot reach the server. Start it with: python -m src.server");
    }
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
      const d = Array.isArray(data.detail) ? data.detail.map((x) => x.msg).join("; ") : data.detail;
      throw new Error(d || `Request failed (${res.status})`);
    }
    return data;
  }

  const MODE_ICON = {
    bus: "directions_bus", metro: "subway", auto: "electric_rickshaw", bike_taxi: "two_wheeler",
    cab: "local_taxi", car_taxi: "local_taxi", walk: "directions_walk", road: "route",
  };
  const KIND = {
    official: "Official fare", regulated: "Govt rate", estimated: "Estimated",
    partial: "Partial fare data", indicative: "Indicative", sample: "Sample data",
  };
  const fareText = (o) => (o.fare_kind === "estimated" ? `≈ ${rupees(o.fare)}` : o.fare_kind === "partial" ? `${rupees(o.fare)}+` : rupees(o.fare));
  const isLight = (hex) => /^#?FFDF00$/i.test(hex || "");      // the Yellow Line needs dark text

  const LIVE_QUICK = [
    { from: { name: "Koramangala", lat: 12.9352, lng: 77.6245 }, to: { name: "Whitefield", lat: 12.9698, lng: 77.75 } },
    { from: { name: "Indiranagar", lat: 12.9784, lng: 77.6408 }, to: { name: "Electronic City", lat: 12.8452, lng: 77.6602 } },
    { from: { name: "MG Road", lat: 12.9756, lng: 77.6066 }, to: { name: "Kempegowda Airport", lat: 13.1986, lng: 77.7066 } },
  ];
  const DEMO_QUICK = [["College", "Airport"], ["Hospital", "Airport"], ["Market", "Tech Park"]];

  const S = {
    mode: "live", status: null, from: null, to: null, result: null, sel: null,
    locations: [], network: [], map: null, layers: null, seq: 0,
  };

  // -------------------------------------------------------- icons + ripple
  if (document.fonts && document.fonts.load) {
    document.fonts.load('24px "Material Symbols Rounded"', "route")
      .then((f) => { if (!f.length) document.body.classList.add("icons-off"); })
      .catch(() => document.body.classList.add("icons-off"));
  }

  if (!reduce) {
    document.addEventListener("pointerdown", (e) => {
      const host = e.target.closest(".rp-host");
      if (!host) return;
      const r = host.getBoundingClientRect();
      const size = Math.max(r.width, r.height) * 2.2;
      const dot = document.createElement("span");
      dot.className = "rp";
      dot.style.cssText = `width:${size}px;height:${size}px;left:${e.clientX - r.left - size / 2}px;top:${e.clientY - r.top - size / 2}px`;
      host.appendChild(dot);
      dot.addEventListener("animationend", () => dot.remove());
    });
  }

  // ---------------------------------------------------------------- snack
  let snackTimer = null;
  function snack(message, { warn = false, ms = 3400 } = {}) {
    const el = $("#snack");
    el.className = `snack${warn ? " snack--warn" : ""}`;
    el.innerHTML = `${icon(warn ? "warning" : "check_circle")}<span>${esc(message)}</span>`;
    requestAnimationFrame(() => el.classList.add("show"));
    clearTimeout(snackTimer);
    snackTimer = setTimeout(() => el.classList.remove("show"), ms);
  }

  // ---------------------------------------------------- place search boxes
  function placeBox(input, list, onPick) {
    let items = [], active = -1, timer = null, req = 0;

    const close = () => { list.hidden = true; input.setAttribute("aria-expanded", "false"); active = -1; };
    const render = () => {
      list.innerHTML = items.length
        ? items.map((p, i) => `<li role="option" id="${list.id}-${i}" data-i="${i}" class="${i === active ? "is-active" : ""}" aria-selected="${i === active}">
            ${icon(p.icon || "location_on")}<b>${esc(p.name)}</b><small>${esc(p.address || "")}</small></li>`).join("")
        : '<li class="menu__none">No matching places</li>';
      list.hidden = false;
      input.setAttribute("aria-expanded", "true");
      if (active >= 0) input.setAttribute("aria-activedescendant", `${list.id}-${active}`);
      else input.removeAttribute("aria-activedescendant");
    };

    async function suggest(text) {
      if (S.mode === "demo") {
        const t = text.toLowerCase();
        items = S.locations.filter((l) => l.name.toLowerCase().includes(t))
          .map((l) => ({ name: l.name, address: "Demo city", icon: "location_on" }));
        active = items.length ? 0 : -1;
        return render();
      }
      if (text.length < 2) return close();
      const mine = ++req;
      try {
        const results = await api(`/api/live/places?q=${encodeURIComponent(text)}`);
        if (mine !== req) return;
        items = results.slice(0, 7);
        active = items.length ? 0 : -1;
        render();
      } catch (err) { close(); snack(err.message, { warn: true }); }
    }

    async function pick(i) {
      const p = items[i];
      if (!p) return;
      close();
      input.value = p.name;
      try {
        let place = { name: p.name };
        if (S.mode === "live") {
          let { lat, lng } = p;
          if (lat == null || lng == null) ({ lat, lng } = await api(`/api/live/place?place_id=${encodeURIComponent(p.place_id)}`));
          place = { name: p.name, lat, lng };
        }
        onPick(place);
      } catch (err) { snack(err.message, { warn: true }); }
    }

    input.addEventListener("input", () => {
      clearTimeout(timer);
      const text = input.value.trim();
      timer = setTimeout(() => suggest(text), S.mode === "demo" ? 0 : 260);   // debounce live search
    });
    input.addEventListener("focus", () => { if (S.mode === "demo") { input.select(); suggest(""); } });
    input.addEventListener("keydown", (e) => {
      if (list.hidden) return;
      if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        e.preventDefault();
        if (!items.length) return;
        active = (active + (e.key === "ArrowDown" ? 1 : -1) + items.length) % items.length;
        render();
      } else if (e.key === "Enter") { e.preventDefault(); pick(Math.max(active, 0)); }
      else if (e.key === "Escape") close();
    });
    list.addEventListener("mousedown", (e) => {
      const li = e.target.closest("li[data-i]");
      if (li) { e.preventDefault(); pick(Number(li.dataset.i)); }
    });
    input.addEventListener("blur", () => setTimeout(close, 140));
    return { set: (p) => { input.value = p ? p.name : ""; } };
  }

  const fromBox = placeBox($("#plan-from"), $("#plan-from-list"), (p) => { S.from = p; maybeCompare(); });
  const toBox = placeBox($("#plan-to"), $("#plan-to-list"), (p) => { S.to = p; maybeCompare(); });
  const againBox = placeBox($("#again-input"), $("#again-list"), (p) => {
    S.to = p; toBox.set(p); $("#again-input").value = "";
    $("#again-input").blur();
    S.scrollToResults = true;          // land on the new results once they render
    maybeCompare();
  });

  function maybeCompare() { if (S.from && S.to) compare(); }

  // -------------------------------------------------------------- compare
  function setTrip(from, to) {
    S.from = from; S.to = to;
    fromBox.set(from); toBox.set(to);
    compare();
  }

  function skeleton() {
    $("#results").setAttribute("aria-busy", "true");
    $("#results-title").textContent = "Comparing…";
    $("#results-meta").textContent = "";
    $("#opts").innerHTML = Array.from({ length: 4 }, () => '<li class="skel" aria-hidden="true"></li>').join("");
    $("#savings").hidden = true;
  }

  async function compare() {
    if (!S.from || !S.to) return;
    if (S.from.name === S.to.name && S.from.lat === S.to.lat) {
      snack("Start and destination are the same place.", { warn: true });
      return;
    }
    const mine = ++S.seq;
    skeleton();
    const t0 = performance.now();
    try {
      let data;
      if (S.mode === "live") {
        const q = new URLSearchParams({
          from_lat: S.from.lat, from_lng: S.from.lng, from_name: S.from.name,
          to_lat: S.to.lat, to_lng: S.to.lng, to_name: S.to.name,
        });
        data = normalizeLive(await api(`/api/live/plan?${q}`));
      } else {
        const q = new URLSearchParams({ source: S.from.name, destination: S.to.name });
        data = normalizeDemo(await api(`/api/routes?${q}`));
      }
      if (mine !== S.seq) return;            // a newer search finished first
      const ms = Math.round(performance.now() - t0);
      $("#tc-ms").textContent = `${ms} ms`;
      S.result = data;
      S.sel = data.cheapest;
      renderResults();
      drawHeroMap();
      if (S.scrollToResults) {
        S.scrollToResults = false;
        const top = $("#results").getBoundingClientRect().top + scrollY - 88;
        scrollTo({ top, behavior: reduce ? "auto" : "smooth" });
      }
      $("#again-from").textContent = S.from.name;
      syncPeakToResult();
      if (S.mode === "live") loadRecent();
    } catch (err) {
      if (mine !== S.seq) return;
      $("#results").setAttribute("aria-busy", "false");
      $("#results-title").textContent = "Could not compare this trip";
      $("#results-meta").textContent = "";
      $("#opts").innerHTML = `<li class="opt opt--off"><div class="opt__main">${icon("warning", "opt__icon")}<span class="opt__body"><span class="opt__title">${esc(err.message)}</span></span><span></span></div></li>`;
      snack(err.message, { warn: true });
    }
  }

  function normalizeLive(d) {
    return {
      kind: "live", origin: d.origin, destination: d.destination, road: d.road, at: d.generated_at,
      options: d.options, unavailable: d.unavailable,
      cheapest: d.cheapest_mode, fastest: d.fastest_mode,
    };
  }

  function normalizeDemo(d) {
    if (d.same_location) throw new Error("Start and destination are the same place.");
    const options = d.options.map((o) => ({
      ...o,
      providers: "Demo city",
      fare_kind: "sample",
      fare_text: `₹${o.fare_breakdown.base} + ₹${o.fare_breakdown.per_km}/km`,
      fare_source: "Illustrative fare rules for the offline demo city.",
      legs: o.legs.map((l) => ({ kind: o.mode, ...l })),
      notes: ["Sample data for the offline demo, not real prices."],
      links: [],
    }));
    return {
      kind: "demo", origin: { name: d.source }, destination: { name: d.destination }, at: null,
      options, unavailable: d.unavailable.map((u) => ({ ...u, reason: "No connection by this mode in the demo city" })),
      cheapest: d.cheapest_mode, fastest: d.fastest_mode,
    };
  }

  // -------------------------------------------------------------- results
  function legHtml(leg, option) {
    if (leg.kind === "metro") {
      return `<li class="leg">${icon("subway")}<div><span class="line-pill${isLight(leg.color) ? " is-light" : ""}" style="--lc:${esc(leg.color)}">${esc(leg.line)}</span> ${leg.stops} stop${leg.stops === 1 ? "" : "s"}
        <small>${esc(leg.from)} → ${esc(leg.to)} · ${minutes(leg.time_min)}</small></div></li>`;
    }
    if (leg.kind === "walk") {
      return `<li class="leg">${icon("directions_walk")}<div>Walk ${km(leg.distance_km)}<small>${esc(leg.from)} → ${esc(leg.to)} · ${minutes(leg.time_min)}</small></div></li>`;
    }
    const name = leg.kind === "auto" && option.mode !== "auto" ? `Auto ${leg.fare ? rupees(leg.fare) : ""}` : option.label;
    return `<li class="leg">${icon(MODE_ICON[leg.kind] || MODE_ICON[option.mode] || "route")}<div>${esc(name)} · ${km(leg.distance_km)}
      <small>${esc(leg.from)} → ${esc(leg.to)} · ${minutes(leg.time_min)}</small></div></li>`;
  }

  function renderResults() {
    const r = S.result;
    $("#results").setAttribute("aria-busy", "false");
    $("#results-title").textContent = `${r.origin.name} → ${r.destination.name}`;
    if (r.kind === "live") {
      const time = r.at ? new Date(r.at).toLocaleTimeString("en-IN", { hour: "numeric", minute: "2-digit", timeZone: "Asia/Kolkata" }) : "";
      $("#results-meta").innerHTML = `<span class="live" aria-hidden="true"></span>Live ${esc(time)} · ${km(r.road.distance_km)} by road, ${minutes(r.road.duration_min)} in traffic now`;
    } else {
      $("#results-meta").innerHTML = `${icon("info")}Demo city · sample data`;
    }

    const cards = r.options.map((o, i) => `
      <li class="opt${o.mode === S.sel ? " is-sel" : ""}" data-mode="${o.mode}" style="--i:${i}">
        <button type="button" class="opt__main rp-host" aria-expanded="${o.mode === S.sel}" aria-controls="more-${o.mode}">
          <span class="opt__icon">${icon(MODE_ICON[o.mode] || "route")}</span>
          <span class="opt__body">
            <span class="opt__title">${esc(o.label)}
              ${o.is_cheapest ? `<span class="badge badge--cheap">${icon("savings")}Cheapest</span>` : ""}
              ${o.is_fastest ? `<span class="badge badge--fast">${icon("bolt")}Fastest</span>` : ""}</span>
            <span class="opt__meta">${minutes(o.time_min)} · ${km(o.distance_km)} · <span class="kind kind--${o.fare_kind}">${KIND[o.fare_kind] || o.fare_kind}</span></span>
          </span>
          <span class="opt__fare">${fareText(o)}</span>
        </button>
        <div class="opt__more" id="more-${o.mode}"><div><div class="opt__inner">
          <ol class="legs">${o.legs.map((l) => legHtml(l, o)).join("")}</ol>
          <p class="opt__src"><b>${esc(o.fare_text)}.</b> ${esc(o.fare_source)} ${o.notes.map(esc).join(" ")}</p>
          ${o.links.length ? `<div class="apps">${o.links.map((l, j) => `<a class="app rp-host${j === 0 ? " app--primary" : ""}" href="${esc(l.url)}" target="_blank" rel="noopener noreferrer">${esc(l.label)}${icon("open_in_new")}</a>`).join("")}</div>` : ""}
        </div></div></div>
      </li>`);
    const off = r.unavailable.map((u, i) => `
      <li class="opt opt--off" style="--i:${r.options.length + i}">
        <div class="opt__main"><span class="opt__icon">${icon(MODE_ICON[u.mode] || "route")}</span>
          <span class="opt__body"><span class="opt__title">${esc(u.label)}</span><span class="opt__meta">${esc(u.reason)}</span></span>
          <span class="opt__fare">Not available</span></div>
      </li>`);
    $("#opts").innerHTML = cards.concat(off).join("");

    const cheap = r.options.find((o) => o.mode === r.cheapest);
    const fast = r.options.find((o) => o.mode === r.fastest);
    const box = $("#savings");
    if (cheap && fast && cheap.mode !== fast.mode && fast.fare > cheap.fare) {
      box.innerHTML = `${icon("savings")}${esc(cheap.label)} saves ${rupees(fast.fare - cheap.fare)} over ${esc(fast.label)}`;
      box.hidden = false;
      box.style.animation = "none"; void box.offsetWidth; box.style.animation = "";
    } else {
      box.hidden = true;
    }
  }

  $("#opts").addEventListener("click", (e) => {
    const btn = e.target.closest(".opt__main");
    const li = btn && btn.closest(".opt:not(.opt--off)");
    if (!li || !S.result) return;
    S.sel = li.dataset.mode;
    $$(".opt", $("#opts")).forEach((el) => {
      const on = el.dataset.mode === S.sel;
      el.classList.toggle("is-sel", on);
      const b = $(".opt__main", el);
      if (b && b.hasAttribute("aria-expanded")) b.setAttribute("aria-expanded", String(on));
    });
    drawHeroMap();
  });

  // ------------------------------------------------------------- hero map
  function decodePolyline(str, precision) {
    const f = 10 ** precision, pts = [];
    let i = 0, lat = 0, lng = 0;
    while (i < str.length) {
      for (const which of [0, 1]) {
        let result = 0, shift = 0, b;
        do { b = str.charCodeAt(i++) - 63; result |= (b & 0x1f) << shift; shift += 5; } while (b >= 0x20 && i < str.length);
        const d = result & 1 ? ~(result >> 1) : result >> 1;
        if (which === 0) lat += d; else lng += d;
      }
      pts.push([lat / f, lng / f]);
    }
    return pts;
  }

  function roadPath(r) {
    if (r.road && r.road.polyline) {
      for (const p of [5, 6]) {
        try {
          const pts = decodePolyline(r.road.polyline, p);
          if (pts.length > 1 && pts.every(([a, b]) => a > 12 && a < 14 && b > 77 && b < 78.5)) return pts;
        } catch { /* try next precision */ }
      }
    }
    return [[r.origin.lat, r.origin.lng], [r.destination.lat, r.destination.lng]];
  }

  function captionFor(o) {
    const r = S.result;
    $("#map-cap").innerHTML = `${icon(MODE_ICON[o.mode] || "near_me")}<span><b>${esc(o.label)}</b> · ${esc(r.origin.name)} → ${esc(r.destination.name)} · ${minutes(o.time_min)} · ${fareText(o)}</span>`;
  }

  function drawHeroMap() {
    const r = S.result;
    if (!r) return;
    const o = r.options.find((x) => x.mode === S.sel) || r.options[0];
    if (!o) return;
    captionFor(o);
    if (r.kind === "live") drawLeaflet(r, o); else drawDemoMap(r, o);
  }

  function drawLeaflet(r, o) {
    const host = $("#hero-map");
    if (typeof L === "undefined") {
      host.innerHTML = `<div class="map-off" style="display:grid;place-items:center;height:100%;color:#4A4E59">${icon("wifi_off")} Map needs internet</div>`;
      return;
    }
    if (!S.map) {
      host.innerHTML = "";
      S.map = L.map(host, { scrollWheelZoom: false, zoomControl: true, attributionControl: true }).setView([12.97, 77.6], 12);
      L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 19, attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
      }).addTo(S.map);
      S.layers = L.layerGroup().addTo(S.map);
    }
    S.layers.clearLayers();
    const a = [r.origin.lat, r.origin.lng], b = [r.destination.lat, r.destination.lng];
    const pts = [a, b];
    if (o.mode === "metro") {
      let cursor = a;
      o.legs.forEach((leg, i) => {
        if (leg.kind === "metro") {
          L.polyline(leg.coords, { color: leg.color, weight: 7, opacity: 0.95 }).addTo(S.layers);
          leg.coords.forEach((c) => L.circleMarker(c, { radius: 3, color: "#fff", weight: 1, fillColor: leg.color, fillOpacity: 1 }).addTo(S.layers));
          pts.push(...leg.coords);
          cursor = leg.coords[leg.coords.length - 1];
        } else {
          const next = o.legs[i + 1];
          const end = i === 0 && next && next.coords ? next.coords[0] : b;
          const start = i === 0 ? a : cursor;
          L.polyline([start, end], { color: leg.kind === "walk" ? "#16181D" : "#304FFE", weight: 4, dashArray: "6 8" }).addTo(S.layers);
        }
      });
    } else {
      const path = roadPath(r);
      L.polyline(path, { color: "#16181D", weight: 7, opacity: 0.9, dashArray: o.mode === "bus" ? "12 9" : null }).addTo(S.layers);
      pts.push(...path);
    }
    const pin = (cls) => L.divIcon({ className: "", html: `<div class="pin ${cls}"></div>`, iconSize: [26, 26], iconAnchor: [13, 13] });
    L.marker(a, { icon: pin("pin--a") }).bindTooltip(`Start: ${esc(r.origin.name)}`).addTo(S.layers);
    L.marker(b, { icon: pin("pin--b") }).bindTooltip(`Destination: ${esc(r.destination.name)}`).addTo(S.layers);
    S.map.invalidateSize();
    S.map.fitBounds(pts, { padding: [40, 40], animate: !reduce });
  }

  function drawDemoMap(r, o) {
    if (S.map) { S.map.remove(); S.map = null; }
    const host = $("#hero-map");
    if (!S.locations.length) return;
    const xs = S.locations.map((l) => l.x), ys = S.locations.map((l) => l.y);
    const minX = Math.min(...xs), maxX = Math.max(...xs), minY = Math.min(...ys), maxY = Math.max(...ys);
    const scale = 100, pad = 80;
    const W = (maxX - minX) * scale + pad * 2, H = (maxY - minY) * scale + pad * 2;
    const pos = {};
    S.locations.forEach((l) => { pos[l.name] = [(l.x - minX) * scale + pad, (maxY - l.y) * scale + pad]; });
    const pairs = new Map();
    S.network.forEach((c) => {
      const k = [c.from, c.to].sort().join("|");
      if (!pairs.has(k)) pairs.set(k, { a: c.from, b: c.to, modes: new Set() });
      pairs.get(k).modes.add(c.mode);
    });
    const onPath = new Set(o.path || []);
    const parts = [];
    for (const { a, b, modes } of pairs.values()) {
      const metro = modes.size === 1 && modes.has("metro");
      parts.push(`<line class="dm-edge${metro ? " dm-edge--metro" : ""}" x1="${pos[a][0]}" y1="${pos[a][1]}" x2="${pos[b][0]}" y2="${pos[b][1]}"/>`);
    }
    if (o.path) parts.push(`<polyline class="dm-route" points="${o.path.map((n) => pos[n].join(",")).join(" ")}"/>`);
    const ends = [r.origin.name, r.destination.name];
    S.locations.forEach((l) => {
      const [x, y] = pos[l.name];
      const cls = ends.includes(l.name) ? "end" : onPath.has(l.name) ? "on" : "";
      parts.push(`<circle class="dm-node ${cls}" cx="${x}" cy="${y}" r="${cls === "end" ? 14 : 11}"/>`);
      parts.push(`<text class="dm-label" x="${x}" y="${y + 34}" text-anchor="middle">${esc(l.name)}</text>`);
    });
    host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="xMidYMid meet" aria-hidden="true">${parts.join("")}</svg>`;
  }

  // ------------------------------------------------------- recent + chips
  async function loadRecent() {
    let recent = [];
    try { recent = await api("/api/live/history?limit=12"); } catch { /* optional */ }
    const seen = new Set();
    S.recent = recent.filter((h) => {
      const k = `${h.origin.name}|${h.destination.name}`;
      if (seen.has(k)) return false;
      seen.add(k); return true;
    }).slice(0, 2);
    renderChips();
  }

  function renderChips() {
    const box = $("#quick");
    let chips;
    if (S.mode === "live") {
      const hist = (S.recent || []).map((h) => ({ from: h.origin, to: h.destination, hist: true }));
      const keys = new Set(hist.map((h) => `${h.from.name}|${h.to.name}`));
      chips = hist.concat(LIVE_QUICK.filter((q) => !keys.has(`${q.from.name}|${q.to.name}`))).slice(0, 4);
    } else {
      chips = DEMO_QUICK.map(([a, b]) => ({ from: { name: a }, to: { name: b } }));
    }
    S.chips = chips;
    box.innerHTML = chips.map((c, i) => `<button type="button" class="chip rp-host${c.hist ? " chip--hist" : ""}" data-i="${i}" style="animation-delay:${i * 50}ms">${icon(c.hist ? "schedule" : "near_me")}${esc(c.from.name)} → ${esc(c.to.name)}</button>`).join("");
  }

  $("#quick").addEventListener("click", (e) => {
    const chip = e.target.closest(".chip");
    if (!chip) return;
    const c = S.chips[Number(chip.dataset.i)];
    setTrip(c.from, c.to);
  });

  $("#swap").addEventListener("click", () => {
    [S.from, S.to] = [S.to, S.from];
    fromBox.set(S.from); toBox.set(S.to);
    maybeCompare();
  });

  $("#locate").addEventListener("click", () => {
    const btn = $("#locate");
    if (!navigator.geolocation) return snack("This browser cannot share its location.", { warn: true });
    btn.classList.add("is-busy");
    navigator.geolocation.getCurrentPosition(
      (pos) => {
        btn.classList.remove("is-busy");
        S.from = { name: "My location", lat: pos.coords.latitude, lng: pos.coords.longitude };
        fromBox.set(S.from);
        snack("Using your current location");
        maybeCompare();
      },
      () => { btn.classList.remove("is-busy"); snack("Location permission was denied. Type a place instead.", { warn: true }); },
      { enableHighAccuracy: true, timeout: 10000 },
    );
  });

  // ------------------------------------------------------------ mode switch
  function setMode(mode, { quiet = false } = {}) {
    S.mode = mode;
    document.body.dataset.mode = mode;
    $$(".seg__btn").forEach((b) => b.setAttribute("aria-checked", String(b.dataset.mode === mode)));
    $("#hero-sub").textContent = mode === "live"
      ? "Bus, metro, auto, bike taxi and cab on live Bengaluru roads."
      : "The offline demo city: 12 sample places, sample fares, no internet needed.";
    $("#plan-from").placeholder = mode === "live" ? "Start typing a place" : "Pick a demo place";
    $("#plan-to").placeholder = mode === "live" ? "Where are you going?" : "Pick a demo place";
    $("#locate").hidden = mode !== "live";
    S.result = null;
    renderChips();
    if (mode === "live") {
      const first = (S.recent && S.recent[0]) ? { from: S.recent[0].origin, to: S.recent[0].destination } : LIVE_QUICK[0];
      setTrip(first.from, first.to);
    } else {
      setTrip({ name: "College" }, { name: "Airport" });
    }
    if (!quiet) snack(mode === "live" ? "Live Bengaluru: real roads, real metro" : "Demo city: works fully offline");
  }

  $$(".seg__btn").forEach((b) => b.addEventListener("click", () => {
    if (b.disabled || b.dataset.mode === S.mode) return;
    setMode(b.dataset.mode);
  }));

  // =================================================================== PEAK
  // Scroll is the playhead of a real Dijkstra run on the real metro graph.
  const P = { st: {}, lines: [], trace: null, from: "WHTM", to: "APTS", step: -9, done: null, active: false, els: {}, edges: [] };
  const peak = $("#dijkstra");
  const svg = $("#metro");
  const NS = "http://www.w3.org/2000/svg";

  async function initPeak() {
    let data;
    try { data = await api("/api/live/metro"); } catch { return; }
    if (!data.stations.length) return;
    data.stations.forEach((s) => { P.st[s.id] = s; });
    P.lines = data.lines;
    drawMetro();
    fillSelects();
    await loadTrace();
    new IntersectionObserver((entries) => { P.active = entries[0].isIntersecting; if (P.active) tick(); }, { rootMargin: "20% 0px" }).observe(peak);
  }

  function lineColor(id) { const l = P.lines.find((x) => x.id === id); return l ? l.color : "#888"; }
  function lineName(id) { const l = P.lines.find((x) => x.id === id); return l ? l.name.replace(" Line", "") : id; }

  function drawMetro() {
    const all = Object.values(P.st);
    const lat0 = all.reduce((s, x) => s + x.lat, 0) / all.length;
    const k = Math.cos((lat0 * Math.PI) / 180);
    const xs = all.map((s) => s.lng * k), ys = all.map((s) => -s.lat);
    const minX = Math.min(...xs), maxX = Math.max(...xs), minY = Math.min(...ys), maxY = Math.max(...ys);
    const W = 1000, pad = 50, s = (W - pad * 2) / (maxX - minX), H = (maxY - minY) * s + pad * 2;
    const pos = (st) => [pad + (st.lng * k - minX) * s, pad + (-st.lat - minY) * s];
    svg.setAttribute("viewBox", `0 0 ${W} ${Math.round(H)}`);
    svg.setAttribute("preserveAspectRatio", "xMidYMid meet");
    const gEdges = document.createElementNS(NS, "g");
    const gRoute = document.createElementNS(NS, "g");
    const gSt = document.createElementNS(NS, "g");
    const gLab = document.createElementNS(NS, "g");
    P.lines.forEach((line) => {
      for (let i = 0; i + 1 < line.stations.length; i++) {
        const a = P.st[line.stations[i]], b = P.st[line.stations[i + 1]];
        const [x1, y1] = pos(a), [x2, y2] = pos(b);
        const el = document.createElementNS(NS, "line");
        el.setAttribute("class", "m-edge");
        el.setAttribute("x1", x1); el.setAttribute("y1", y1); el.setAttribute("x2", x2); el.setAttribute("y2", y2);
        el.style.setProperty("--lc", line.color);
        gEdges.appendChild(el);
        P.edges.push({ a: a.id, b: b.id, el });
      }
    });
    all.forEach((st) => {
      const [x, y] = pos(st);
      st.x = x; st.y = y;
      const g = document.createElementNS(NS, "g");
      g.setAttribute("class", "m-st");
      g.setAttribute("tabindex", "0");
      g.setAttribute("role", "button");
      g.setAttribute("aria-label", `Replay to ${st.name}`);
      g.dataset.id = st.id;
      g.innerHTML = `<circle class="ring" cx="${x}" cy="${y}" r="5"/><circle class="dot" cx="${x}" cy="${y}" r="4.2"/><circle cx="${x}" cy="${y}" r="13" fill="transparent"/>`;
      gSt.appendChild(g);
      const t = document.createElementNS(NS, "text");
      t.setAttribute("class", "m-label");
      t.setAttribute("x", x + 10); t.setAttribute("y", y - 10);
      t.textContent = st.name.replace(/^(Nadaprabhu Kempegowda Station, )/, "").replace(/ Station.*$/, "");
      gLab.appendChild(t);
      P.els[st.id] = { g, label: t };
    });
    svg.append(gEdges, gRoute, gSt, gLab);
    P.gRoute = gRoute;
    svg.addEventListener("click", (e) => {
      const g = e.target.closest(".m-st");
      if (g) retarget(g.dataset.id);
    });
    svg.addEventListener("keydown", (e) => {
      const g = e.target.closest(".m-st");
      if (g && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); retarget(g.dataset.id); }
    });
  }

  function fillSelects() {
    const opts = Object.values(P.st).sort((a, b) => a.name.localeCompare(b.name))
      .map((s) => `<option value="${s.id}">${esc(s.name)}</option>`).join("");
    $("#run-from").innerHTML = opts;
    $("#run-to").innerHTML = opts;
    $("#run-from").addEventListener("change", (e) => { P.from = e.target.value; loadTrace(true); });
    $("#run-to").addEventListener("change", (e) => { P.to = e.target.value; loadTrace(true); });
  }

  function retarget(id) {
    if (id === P.from) return snack("That is the start station. Pick another one.", { warn: true });
    P.to = id;
    loadTrace(true);
  }

  async function loadTrace(announce) {
    if (P.from === P.to) return;
    try {
      P.trace = await api(`/api/live/metro/trace?from=${P.from}&to=${P.to}`);
    } catch (err) { return snack(err.message, { warn: true }); }
    $("#run-from").value = P.from;
    $("#run-to").value = P.to;
    $("#run-title").textContent = "Dijkstra · live run";
    P.step = -9; P.done = null;
    P.gRoute.innerHTML = "";
    if (announce) snack(`Replaying ${P.trace.events.length} heap pops to ${P.trace.to.name}`);
    render(progress(), true);
  }

  function syncPeakToResult() {
    const r = S.result;
    if (!r || r.kind !== "live" || !P.trace) return;
    const m = r.options.find((o) => o.mode === "metro");
    if (!m) return;
    const rides = m.legs.filter((l) => l.kind === "metro" && l.from_id);
    if (!rides.length) return;
    const from = rides[0].from_id, to = rides[rides.length - 1].to_id;
    if (from !== P.from || to !== P.to) { P.from = from; P.to = to; loadTrace(false); }
  }

  function progress() {
    const v = parseFloat(peak.style.getPropertyValue("--sc-p"));
    if (!Number.isNaN(v)) return v;
    const r = peak.getBoundingClientRect();
    return Math.min(1, Math.max(0, -r.top / Math.max(1, r.height - innerHeight)));
  }

  let lastP = -1;
  function tick() {
    if (!P.active) return;
    const p = progress();
    if (p !== lastP) { lastP = p; render(p); }
    requestAnimationFrame(tick);
  }

  function render(p, force) {
    const tr = P.trace;
    if (!tr) return;
    const ev = tr.events, N = ev.length;
    const A = 0.03, B = 0.82;
    const k = p <= A ? -1 : Math.min(N - 1, Math.floor(((p - A) / (B - A)) * N));
    const done = p >= 0.86;
    $("#trace-range").value = String(Math.round(p * 1000));
    if (!force && k === P.step && done === P.done) return;
    P.step = k; P.done = done;
    // Rounded signature of what actually paints, for the verification harness.
    peak.querySelector("[data-sc-stage]").setAttribute("data-sc-verify-state", `${k}|${done ? 1 : 0}`);

    const settled = new Set(), frontier = new Set();
    for (let i = 0; i <= k; i++) {
      settled.add(ev[i].station);
      ev[i].frontier.forEach((f) => frontier.add(f.station));
    }
    const cur = k >= 0 ? ev[k].station : null;
    Object.entries(P.els).forEach(([id, { g, label }]) => {
      let cls = "m-st";
      if (id === tr.from.id) cls += " start";
      else if (id === tr.to.id) cls += " end";
      else if (id === cur && !done) cls += " cur";
      else if (settled.has(id)) cls += " settled";
      else if (frontier.has(id)) cls += " frontier";
      if (g.getAttribute("class") !== cls) g.setAttribute("class", cls);
      const show = id === tr.from.id || id === tr.to.id || (id === cur && !done) || P.st[id].lines.length > 1;
      label.classList.toggle("show", show);
    });
    P.edges.forEach((e) => e.el.classList.toggle("on", settled.has(e.a) && settled.has(e.b)));

    const now = k >= 0 ? ev[k] : null;
    $("#st-pops").textContent = String(k + 1);
    $("#st-settled").textContent = String(settled.size);
    $("#st-time").textContent = now ? minutes(now.t) : "0 min";
    $("#trace-out").textContent = done ? "route found" : `step ${k + 1} / ${N}`;
    $("#run-status").innerHTML = now
      ? `Settled <b>${esc(P.st[now.station].name)}</b> on ${esc(lineName(now.line))} at ${minutes(now.t)}${now.frontier.length ? `, pushed ${now.frontier.length} neighbour${now.frontier.length === 1 ? "" : "s"}` : ""}`
      : `Start at <b>${esc(tr.from.name)}</b>. Every other station is ∞.`;
    $("#heap").innerHTML = (now ? now.heap : []).map((h) =>
      `<li><i style="--lc:${lineColor(h.line)}"></i><span>${esc(P.st[h.station].name)}</span><span>${minutes(h.t)}</span></li>`).join("")
      || `<li><i></i><span>${now ? "Destination reached, the run stops" : "Start station, 0 min"}</span><span></span></li>`;
    const recent = ev.slice(Math.max(0, k - 5), k + 1).reverse();
    $("#log").innerHTML = recent.map((e) =>
      `<li><i style="--lc:${lineColor(e.line)}"></i><span>${esc(P.st[e.station].name)}</span><span>${minutes(e.t)}</span></li>`).join("");

    const result = $("#run-result");
    if (done) {
      if (!P.gRoute.childNodes.length) drawRoute(tr);
      if (result.hidden) {
        const changes = tr.route.changes;
        result.innerHTML = `<b>${rupees(tr.fare.token)}</b> · ${minutes(tr.route.time_min)}
          <p>${esc(tr.from.name)} → ${esc(tr.to.name)}, ${changes ? `${changes} line change${changes > 1 ? "s" : ""}` : "no line change"}.</p>
          <p>Official BMRCL fare${tr.fare.smart_card ? `, smart card ₹${tr.fare.smart_card} (${tr.fare.timeframe})` : ""}. ${N} heap pops, ${tr.states} graph states.</p>`;
        result.hidden = false;
      }
    } else {
      P.gRoute.innerHTML = "";
      result.hidden = true;
    }
  }

  function drawRoute(tr) {
    tr.route.rides.forEach((ride, i) => {
      const pts = ride.stations.map((id) => `${P.st[id].x},${P.st[id].y}`).join(" ");
      const pl = document.createElementNS(NS, "polyline");
      pl.setAttribute("points", pts);
      pl.setAttribute("class", "m-route draw");
      pl.style.setProperty("--lc", ride.color);
      P.gRoute.appendChild(pl);
      const len = pl.getTotalLength ? pl.getTotalLength() : 1000;
      pl.style.setProperty("--len", len);
      pl.style.animationDelay = `${i * 260}ms`;
    });
    const d = P.st[tr.to.id];
    const g = document.createElementNS(NS, "g");
    g.setAttribute("class", "m-fare");
    const label = rupees(tr.fare.token);
    g.innerHTML = `<rect x="${d.x - 10}" y="${d.y - 58}" rx="14" width="${22 + label.length * 11}" height="30"/><text x="${d.x + 1}" y="${d.y - 37}">${esc(label)}</text>`;
    P.gRoute.appendChild(g);
  }

  $("#trace-range").addEventListener("input", (e) => {
    const p = Number(e.target.value) / 1000;
    const top = peak.getBoundingClientRect().top + scrollY;
    scrollTo({ top: top + p * (peak.offsetHeight - innerHeight), behavior: "auto" });
  });

  // ================================================================== FARES
  async function initFares() {
    let rules = [];
    try { rules = await api("/api/live/fares"); } catch { return; }
    rules.forEach((r) => {
      const card = $(`.rate[data-mode="${r.mode}"]`);
      if (!card) return;
      $(".rate__kind", card).innerHTML = `<span class="kind kind--${r.kind}">${KIND[r.kind]}</span>`;
      $(".rate__rule", card).textContent = r.rule;
      $(".rate__ex", card).textContent = r.example;
      $(".rate__src", card).textContent = r.source;
    });
    dispatchEvent(new Event("resize"));      // let the engine re-measure the rail
    if (reduce) { $$(".rate").forEach((c) => c.classList.add("is-in")); return; }
    const io = new IntersectionObserver((entries) => entries.forEach((en) => {
      if (en.isIntersecting) { en.target.classList.add("is-in"); io.unobserve(en.target); }
    }), { threshold: 0.6 });
    $$(".rate").forEach((c) => io.observe(c));
  }

  // ================================================================= SYSTEM
  function primeTelemetry(st) {
    const set = (id, n) => { const el = $(id); if (el && Number.isFinite(n)) el.setAttribute("data-sc-count", `0 ${n}`); };
    set("#tc-stations", st.metro_stations);
    set("#tc-lines", st.metro_lines);
    set("#tc-states", st.metro_states);
    set("#tc-demo", st.demo_locations);
    set("#tc-conn", st.demo_connections);
    $("#tc-maps").innerHTML = st.maps_configured
      ? `${icon("verified")}Live from Ola Maps`
      : `${icon("wifi_off")}Offline demo only`;
  }

  // ================================================================ CHROME
  addEventListener("scroll", () => $("#appbar").classList.toggle("is-raised", scrollY > 8), { passive: true });
  const tabFor = { plan: "top", dijkstra: "dijkstra", fares: "fares", system: "system" };
  const tabIO = new IntersectionObserver((entries) => entries.forEach((en) => {
    if (!en.isIntersecting) return;
    const t = tabFor[en.target.id];
    $$(".tab").forEach((a) => a.classList.toggle("is-on", a.dataset.tab === t));
  }), { rootMargin: "-45% 0px -50% 0px" });
  Object.keys(tabFor).forEach((id) => tabIO.observe(document.getElementById(id)));

  // ================================================================== START
  async function start() {
    if ("scrollRestoration" in history) history.scrollRestoration = "manual";
    if (!location.hash) scrollTo(0, 0);
    const statusP = api("/api/live/status").catch(() => null);
    const demoP = Promise.all([api("/api/locations"), api("/api/network")]).catch(() => [[], []]);
    const status = await Promise.race([statusP, new Promise((r) => setTimeout(() => r(null), 1500))]);
    S.status = status;
    if (status) primeTelemetry(status);

    // Mount the scroll engine after the telemetry targets are known, so counters are real.
    if (window.ScrollCraft) window.ScrollCraft.mount(document.body);

    [S.locations, S.network] = await demoP;
    const live = !!(status && status.maps_configured);
    document.body.dataset.live = live ? "on" : "off";
    if (!live) {
      const btn = $('.seg__btn[data-mode="live"]');
      btn.disabled = true;
      btn.title = "Live mode needs an Ola Maps API key";
    }
    if (live) await loadRecent();
    setMode(live ? "live" : "demo", { quiet: true });
    if (!live) snack("Live mode is not set up, showing the offline demo city", { warn: true, ms: 4200 });

    initPeak();
    initFares();
  }

  start();
})();
