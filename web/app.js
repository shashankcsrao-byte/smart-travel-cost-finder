/*
 * Smart Travel · frontend
 *
 * The browser never computes routes or fares. Everything comes from the
 * FastAPI backend (src/server.py), which runs the project's own Dijkstra,
 * A*, Yen's k-shortest paths, the mixed-mode search and the metro timetable
 * search. This file renders, animates and wires up interaction.
 */
(function () {
  "use strict";

  // ------------------------------------------------------------------ utils
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const ICONS = "vendor/icons.svg";

  /** Escape text before putting it into HTML (prevents HTML injection). */
  const esc = (v) => String(v ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const safeUrl = (u) => (/^https:\/\//i.test(String(u || "")) ? u : "#");
  const safeColor = (c) => (/^#[0-9a-f]{3,8}$/i.test(String(c || "")) ? c : "#888888");
  const rupees = (n) => (Number.isInteger(n) ? `₹${n}` : `₹${Number(n).toFixed(2)}`);
  const km = (v) => (Number.isInteger(v) ? `${v} km` : `${Number(v).toFixed(1)} km`);
  const minutes = (v) => {
    const t = Math.round(v), h = Math.floor(t / 60), m = t % 60;
    return h ? (m ? `${h} h ${m} min` : `${h} h`) : `${m} min`;
  };

  /** Material Symbols icon from the local sprite: outline + filled, CSS crossfades them. */
  const iconSvgs = (name) =>
    `<svg class="ic__o" aria-hidden="true"><use href="${ICONS}#o-${name}"></use></svg>` +
    `<svg class="ic__f" aria-hidden="true"><use href="${ICONS}#f-${name}"></use></svg>`;
  const icon = (name, cls = "", fill = false) =>
    `<i class="ic ${cls}${fill ? " ic--fill" : ""}" aria-hidden="true">${iconSvgs(name)}</i>`;
  function hydrateIcons(root = document) {
    $$("i.ic[data-i]", root).forEach((el) => {
      if (el.dataset.done) return;
      el.innerHTML = iconSvgs(el.dataset.i);
      el.setAttribute("aria-hidden", "true");
      if (el.hasAttribute("data-fill")) el.classList.add("ic--fill");
      el.dataset.done = "1";
    });
  }

  async function api(path, { timeout = 20000, ...options } = {}) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), timeout);
    let res;
    try {
      res = await fetch(path, { ...options, signal: ctl.signal });
    } catch (err) {
      throw new Error(err.name === "AbortError" ? "The server took too long to answer. Please try again."
        : "Cannot reach the server. Start it with: python -m src.server");
    } finally { clearTimeout(timer); }
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
      const d = Array.isArray(data.detail) ? data.detail.map((x) => x.msg).join("; ") : data.detail;
      throw new Error(d || `Request failed (${res.status})`);
    }
    return data;
  }

  /** localStorage that never throws (private windows, blocked storage). */
  const store = {
    get(key, fallback) { try { const v = localStorage.getItem(key); return v ? JSON.parse(v) : fallback; } catch { return fallback; } },
    set(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); } catch { /* ignore */ } },
  };

  const MODE_ICON = {
    walk: "directions_walk", bus: "directions_bus", metro: "subway", auto: "electric_rickshaw",
    bike_taxi: "two_wheeler", cab: "local_taxi", car_taxi: "local_taxi", mixed: "alt_route", road: "route",
  };
  const KIND = {
    official: "Official fare", regulated: "Govt rate", estimated: "Estimated", partial: "Partial fare data",
    indicative: "Indicative", sample: "Sample data", free: "Free",
  };
  const MODE_STROKE = { bus: "#16181D", metro: "#304FFE", bike_taxi: "#0B7A54", car_taxi: "#B8336A" };
  const fareText = (o) => (o.fare_kind === "estimated" ? `≈ ${rupees(o.fare)}` : o.fare_kind === "partial" ? `${rupees(o.fare)}+` : o.fare === 0 ? "Free" : rupees(o.fare));
  const isLight = (hex) => /^#?FFDF00$/i.test(hex || "");      // the Yellow Line needs dark text

  const LIVE_QUICK = [
    { from: { name: "Koramangala", lat: 12.9352, lng: 77.6245 }, to: { name: "Whitefield", lat: 12.9698, lng: 77.75 } },
    { from: { name: "Indiranagar", lat: 12.9784, lng: 77.6408 }, to: { name: "Electronic City", lat: 12.8452, lng: 77.6602 } },
    { from: { name: "MG Road", lat: 12.9756, lng: 77.6066 }, to: { name: "Kempegowda Airport", lat: 13.1986, lng: 77.7066 } },
  ];
  const DEMO_QUICK = [["College", "Airport"], ["Hospital", "Airport"], ["Residential Area", "Railway Station"]];

  const S = {
    mode: "live", priority: "cheapest", at: "", status: null, from: null, to: null, result: null, sel: null,
    locations: [], network: [], map: null, layers: null, metroLayer: null, seq: 0, chipsKey: "",
    scrollToResults: false, sc: null,
  };

  // ------------------------------------------------------------- ripple
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
    el.innerHTML = `${icon(warn ? "warning" : "check_circle", "", true)}<span>${esc(message)}</span>`;
    requestAnimationFrame(() => el.classList.add("show"));
    clearTimeout(snackTimer);
    snackTimer = setTimeout(() => el.classList.remove("show"), ms);
  }

  // ---------------------------------------------------- radiogroups (keyboard)
  function radiogroup(root, attr, onPick) {
    const buttons = $$("[role=radio]", root);
    const select = (btn, fire = true) => {
      buttons.forEach((b) => {
        const on = b === btn;
        b.setAttribute("aria-checked", String(on));
        b.tabIndex = on ? 0 : -1;
      });
      if (fire) onPick(btn.dataset[attr], btn);
    };
    buttons.forEach((b) => {
      b.addEventListener("click", () => { if (!b.disabled && b.getAttribute("aria-checked") !== "true") select(b); });
      b.addEventListener("keydown", (e) => {
        if (!["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(e.key)) return;
        e.preventDefault();
        const enabled = buttons.filter((x) => !x.disabled);
        const i = enabled.indexOf(b) + (e.key === "ArrowRight" || e.key === "ArrowDown" ? 1 : -1);
        const next = enabled[(i + enabled.length) % enabled.length];
        next.focus();
        select(next);
      });
    });
    return { set: (value) => { const b = buttons.find((x) => x.dataset[attr] === value); if (b) select(b, false); } };
  }

  // ------------------------------------------------- per-device trips/places
  const recentKey = () => `stcf-recent-${S.mode}`;
  const tripKey = (t) => `${S.mode}|${t.from.name}|${t.from.lat ?? ""}|${t.to.name}|${t.to.lat ?? ""}`;
  function rememberTrip(from, to) {
    const list = store.get(recentKey(), []).filter((t) => tripKey(t) !== tripKey({ from, to }));
    list.unshift({ from, to });
    store.set(recentKey(), list.slice(0, 8));
  }
  const savedTrips = () => store.get("stcf-saved", []).filter((t) => t.mode === S.mode);
  function isSaved() {
    if (!S.from || !S.to) return false;
    return savedTrips().some((t) => tripKey(t) === tripKey({ from: S.from, to: S.to }));
  }
  function knownPlaces() {
    const seen = new Set(), out = [];
    [...savedTrips(), ...store.get(recentKey(), [])].forEach((t) => [t.from, t.to].forEach((p) => {
      if (p && p.name !== "My location" && !seen.has(p.name)) { seen.add(p.name); out.push(p); }
    }));
    return out.slice(0, 5);
  }

  // ---------------------------------------------------- place search boxes
  function placeBox(input, list, end, onPick) {
    let items = [], active = -1, timer = null, req = 0, pickSeq = 0;

    const close = () => {
      list.hidden = true;
      input.setAttribute("aria-expanded", "false");
      input.removeAttribute("aria-activedescendant");
      active = -1;
    };
    const render = () => {
      if (document.activeElement !== input) return close();        // user already left the field
      list.innerHTML = items.length
        ? items.map((p, i) => `<li role="option" id="${list.id}-${i}" data-i="${i}" class="${i === active ? "is-active" : ""}" aria-selected="${i === active}">
            ${icon(p.icon || "location_on")}<b>${esc(p.name)}</b><small>${esc(p.address || "")}</small></li>`).join("")
        : '<li class="menu__none">No matching places</li>';
      list.hidden = false;
      input.setAttribute("aria-expanded", "true");
      if (active >= 0) input.setAttribute("aria-activedescendant", `${list.id}-${active}`);
      else input.removeAttribute("aria-activedescendant");
    };
    const shortcuts = () => {
      const out = [];
      if (S.mode === "live" && end === "from" && navigator.geolocation) out.push({ name: "My location", address: "Use this device's location", icon: "my_location", locate: true });
      knownPlaces().forEach((p) => out.push({ ...p, address: S.mode === "live" ? "Recent place" : "Demo city", icon: "schedule", known: true }));
      return out;
    };

    async function suggest(text) {
      const mine = ++req;
      if (S.mode === "demo") {
        const t = text.toLowerCase();
        items = S.locations.filter((l) => l.name.toLowerCase().includes(t))
          .map((l) => ({ name: l.name, address: "Demo city", icon: "location_on" }));
        active = items.length ? 0 : -1;
        return render();
      }
      if (text.length < 2) {
        items = shortcuts();
        active = -1;
        return items.length ? render() : close();
      }
      try {
        const results = await api(`/api/live/places?q=${encodeURIComponent(text)}`);
        if (mine !== req) return;                                  // a newer request was sent
        items = results.slice(0, 7);
        active = items.length ? 0 : -1;
        render();
      } catch (err) { if (mine === req) { close(); snack(err.message, { warn: true }); } }
    }

    async function pick(i) {
      const p = items[i];
      if (!p) return;
      close();
      const mine = ++pickSeq, mode = S.mode;
      if (p.locate) return locate(end);
      try {
        let place = { name: p.name };
        if (mode === "live") {
          let { lat, lng } = p;
          if (lat == null || lng == null) ({ lat, lng } = await api(`/api/live/place?place_id=${encodeURIComponent(p.place_id)}`));
          place = { name: p.name, lat, lng };
        }
        if (mine !== pickSeq || mode !== S.mode) return;          // superseded by a newer pick
        input.value = place.name;
        onPick(place);
      } catch (err) {
        restore();
        snack(err.message, { warn: true });
      }
    }
    function restore() { const p = end === "from" ? S.from : end === "to" ? S.to : null; input.value = p ? p.name : ""; }

    input.addEventListener("input", () => {
      clearTimeout(timer);
      const text = input.value.trim();
      timer = setTimeout(() => suggest(text), S.mode === "demo" ? 0 : 260);   // debounce live search
    });
    input.addEventListener("focus", () => {
      if (S.mode === "demo") { input.select(); suggest(""); }
      else if (!input.value.trim() || end === "again") suggest(end === "again" ? input.value.trim() : "");
    });
    input.addEventListener("keydown", (e) => {
      if (e.key === "ArrowDown" && list.hidden) { suggest(input.value.trim()); return; }
      if (list.hidden) return;
      if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        e.preventDefault();
        if (!items.length) return;
        active = (active + (e.key === "ArrowDown" ? 1 : -1) + items.length) % items.length;
        render();
      } else if (e.key === "Enter") { e.preventDefault(); pick(Math.max(active, 0)); }
      else if (e.key === "Escape") { close(); restore(); }
    });
    list.addEventListener("mousedown", (e) => {
      const li = e.target.closest("li[data-i]");
      if (li) { e.preventDefault(); pick(Number(li.dataset.i)); }
    });
    input.addEventListener("blur", () => setTimeout(() => {
      clearTimeout(timer);
      req++;                                                       // ignore answers that arrive later
      close();
      if (end !== "again" && !pickSeq) restore();
      else if (end !== "again") restore();
    }, 150));
    return { set: (p) => { input.value = p ? p.name : ""; } };
  }

  const fromBox = placeBox($("#plan-from"), $("#plan-from-list"), "from", (p) => { S.from = p; maybeCompare(); });
  const toBox = placeBox($("#plan-to"), $("#plan-to-list"), "to", (p) => { S.to = p; maybeCompare(); });
  placeBox($("#again-input"), $("#again-list"), "again", (p) => {
    S.to = p; toBox.set(p); $("#again-input").value = "";
    $("#again-input").blur();
    S.scrollToResults = true;
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
    $("#opts").setAttribute("aria-busy", "true");
    $("#results-title").textContent = "Comparing…";
    $("#results-meta").textContent = "";
    $("#opts").innerHTML = Array.from({ length: 4 }, () => '<li class="skel" aria-hidden="true"></li>').join("");
    $("#savings").hidden = true;
  }

  function samePlace(a, b) {
    if (!a || !b) return false;
    if (S.mode === "demo") return a.name === b.name;
    return Math.abs(a.lat - b.lat) < 1e-4 && Math.abs(a.lng - b.lng) < 1e-4;
  }

  function showError(message) {
    $("#opts").setAttribute("aria-busy", "false");
    $("#results-title").textContent = "Could not compare this trip";
    $("#results-meta").textContent = "";
    $("#opts").innerHTML = `<li class="opt opt--off"><div class="opt__main">${icon("warning", "opt__icon")}<span class="opt__body"><span class="opt__title">${esc(message)}</span></span>
      <button type="button" class="app rp-host" id="retry">${icon("refresh")}Try again</button></div></li>`;
    $("#retry").addEventListener("click", compare);
  }

  async function compare() {
    if (!S.from || !S.to) return;
    const mine = ++S.seq;                       // cancels any older comparison still running
    if (samePlace(S.from, S.to)) {
      S.scrollToResults = false;
      showError("Start and destination are the same place.");
      return;
    }
    const mode = S.mode;
    skeleton();
    const t0 = performance.now();
    let data;
    try {
      if (mode === "live") {
        const q = new URLSearchParams({
          from_lat: S.from.lat, from_lng: S.from.lng, from_name: S.from.name,
          to_lat: S.to.lat, to_lng: S.to.lng, to_name: S.to.name, priority: S.priority,
        });
        if (S.at) q.set("at", S.at);
        data = normalizeLive(await api(`/api/live/plan?${q}`));
      } else {
        const q = new URLSearchParams({ source: S.from.name, destination: S.to.name, priority: S.priority });
        data = normalizeDemo(await api(`/api/routes?${q}`));
      }
    } catch (err) {
      if (mine !== S.seq) return;
      S.scrollToResults = false;
      showError(err.message);
      snack(err.message, { warn: true });
      return;
    }
    if (mine !== S.seq || mode !== S.mode) return;    // a newer search, or the mode changed
    $("#tc-ms").textContent = `${Math.round(performance.now() - t0)} ms`;
    S.result = data;
    S.sel = data.options[0] ? data.options[0].id : null;     // new answer: show its top-ranked option
    renderResults();
    rememberTrip(S.from, S.to);
    syncUrl();
    $("#again-from").textContent = S.from.name;
    // The parts below are extras: a failure there must not replace good results.
    try { drawHeroMap(); } catch (err) { console.error(err); }
    try { syncPeakToResult(); } catch (err) { console.error(err); }
    try { renderChips(); } catch (err) { console.error(err); }
    if (mode === "live") refreshStatus();
    if (S.scrollToResults) {
      S.scrollToResults = false;
      const top = $("#results").getBoundingClientRect().top + scrollY - 88;
      scrollTo({ top, behavior: reduce ? "auto" : "smooth" });
    }
    relayout();
  }

  function normalizeLive(d) {
    return {
      kind: "live", origin: d.origin, destination: d.destination, road: d.road, at: d.generated_at,
      depart: d.depart_at, options: d.options, unavailable: d.unavailable,
      cheapest: d.cheapest_mode, fastest: d.fastest_mode,
    };
  }

  function normalizeDemo(d) {
    if (d.same_location) throw new Error("Start and destination are the same place.");
    const options = d.options.map((o) => ({
      ...o,
      providers: o.variant === "mixed" ? "Changes mode on the way" : o.variant === "fastest" ? "Quickest route for this mode" : "Demo city",
      fare_kind: "sample",
      fare_text: o.parts ? o.parts.map((p) => `${p.label} ${rupees(p.fare)}`).join(" + ")
        : `₹${o.fare_breakdown.base} + ₹${o.fare_breakdown.per_km}/km`,
      fare_source: o.parts ? "Each boarding pays that mode's base fare (illustrative demo fares)."
        : "Illustrative fare rules for the offline demo city.",
      legs: o.legs.map((l) => ({ kind: l.mode, ...l })),
      notes: o.parts ? [`Includes ${o.parts.length - 1} change(s) of mode, 5 min each.`] : ["Sample data for the offline demo, not real prices."],
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
      const times = leg.depart ? ` · ${esc(leg.depart)} → ${esc(leg.arrive)}` : "";
      return `<li class="leg">${icon("subway")}<div><span class="line-pill${isLight(leg.color) ? " is-light" : ""}" style="--lc:${safeColor(leg.color)}">${esc(leg.line)}</span> ${leg.stops} stop${leg.stops === 1 ? "" : "s"}
        <small>${esc(leg.from)} → ${esc(leg.to)} · ${minutes(leg.time_min)}${times}</small></div></li>`;
    }
    if (leg.kind === "walk") {
      return `<li class="leg">${icon("directions_walk")}<div>Walk ${km(leg.distance_km)}<small>${esc(leg.from)} → ${esc(leg.to)} · ${minutes(leg.time_min)}</small></div></li>`;
    }
    const legMode = leg.mode || leg.kind;
    const name = leg.kind === "auto" && option.mode === "metro" ? `Auto ${leg.fare ? rupees(leg.fare) : ""}`
      : option.variant === "mixed" ? (MODE_LABEL[legMode] || option.label) : option.label;
    return `<li class="leg">${icon(MODE_ICON[legMode] || MODE_ICON[option.mode] || "route")}<div>${esc(name)} · ${km(leg.distance_km)}
      <small>${esc(leg.from)} → ${esc(leg.to)} · ${minutes(leg.time_min)}</small></div></li>`;
  }
  const MODE_LABEL = { bus: "Bus", metro: "Metro", bike_taxi: "Bike Taxi", car_taxi: "Car Taxi" };

  function renderResults() {
    const r = S.result;
    $("#opts").setAttribute("aria-busy", "false");
    $("#results-title").textContent = `${r.origin.name} → ${r.destination.name}`;
    if (r.kind === "live") {
      const leave = r.depart ? new Date(r.depart).toLocaleTimeString("en-IN", { hour: "numeric", minute: "2-digit", timeZone: "Asia/Kolkata" }) : "";
      $("#results-meta").innerHTML = `<span class="live" aria-hidden="true"></span>${S.at ? `Leaving ${esc(leave)}` : `Live, leaving now`} · ${km(r.road.distance_km)} by road, ${minutes(r.road.duration_min)} in traffic`;
    } else {
      $("#results-meta").innerHTML = `${icon("info")}Demo city · sample data`;
    }
    $("#save-trip").setAttribute("aria-pressed", String(isSaved()));

    const cards = r.options.map((o, i) => {
      const sel = o.id === S.sel;
      const alts = r.kind === "demo" && o.variant === "cheapest"
        ? `<button type="button" class="app rp-host alts-btn" data-alts="${esc(o.mode)}">${icon("alt_route")}Other routes</button><ol class="alts" hidden></ol>` : "";
      return `
      <li class="opt${sel ? " is-sel" : ""}" data-id="${esc(o.id)}" style="--i:${i}">
        <button type="button" class="opt__main rp-host" aria-expanded="${sel}" aria-controls="more-${esc(o.id)}">
          <span class="opt__icon">${icon(MODE_ICON[o.mode] || "route")}</span>
          <span class="opt__body">
            <span class="opt__title">${esc(o.label)}
              ${o.is_cheapest ? `<span class="badge badge--cheap">${icon("savings", "", true)}Cheapest</span>` : ""}
              ${o.is_fastest ? `<span class="badge badge--fast">${icon("bolt", "", true)}Fastest</span>` : ""}</span>
            <span class="opt__meta">${minutes(o.time_min)} · ${km(o.distance_km)} · <span class="kind kind--${esc(o.fare_kind)}">${esc(KIND[o.fare_kind] || o.fare_kind)}</span></span>
          </span>
          <span class="opt__fare">${fareText(o)}</span>
        </button>
        <div class="opt__more" id="more-${esc(o.id)}"${sel ? "" : " inert"}><div><div class="opt__inner">
          <ol class="legs">${o.legs.map((l) => legHtml(l, o)).join("")}</ol>
          <p class="opt__src"><b>${esc(o.fare_text)}.</b> ${esc(o.fare_source)} ${o.notes.map(esc).join(" ")}</p>
          ${o.links.length || alts ? `<div class="apps">${o.links.map((l, j) => `<a class="app rp-host${j === 0 ? " app--primary" : ""}" href="${esc(safeUrl(l.url))}" target="_blank" rel="noopener noreferrer">${esc(l.label)}${icon("open_in_new")}</a>`).join("")}${alts}</div>` : ""}
        </div></div></div>
      </li>`;
    });
    const off = r.unavailable.map((u, i) => `
      <li class="opt opt--off" style="--i:${r.options.length + i}">
        <div class="opt__main"><span class="opt__icon">${icon(MODE_ICON[u.mode] || "route")}</span>
          <span class="opt__body"><span class="opt__title">${esc(u.label)}</span><span class="opt__meta">${esc(u.reason)}</span></span>
          <span class="opt__fare">Not available</span></div>
      </li>`);
    $("#opts").innerHTML = cards.concat(off).join("");

    const cheap = r.options.find((o) => o.is_cheapest);
    const fast = r.options.find((o) => o.is_fastest);
    $("#results-live").textContent = `${r.options.length} options. Cheapest: ${cheap ? `${cheap.label}, ${fareText(cheap)}` : "none"}. Fastest: ${fast ? `${fast.label}, ${minutes(fast.time_min)}` : "none"}.`;
    const box = $("#savings");
    if (cheap && fast && cheap.id !== fast.id && fast.fare > cheap.fare) {
      box.innerHTML = `${icon("savings", "", true)}${esc(cheap.label)} saves ${rupees(fast.fare - cheap.fare)} over ${esc(fast.label)}`;
      box.hidden = false;
      box.style.animation = "none"; void box.offsetWidth; box.style.animation = "";
    } else {
      box.hidden = true;
    }
  }

  $("#opts").addEventListener("click", async (e) => {
    const altsBtn = e.target.closest("[data-alts]");
    if (altsBtn) return showAlternatives(altsBtn);
    const btn = e.target.closest(".opt__main");
    const li = btn && btn.closest(".opt:not(.opt--off)");
    if (!li || !S.result) return;
    S.sel = li.dataset.id;
    $$(".opt", $("#opts")).forEach((el) => {
      const on = el.dataset.id === S.sel;
      el.classList.toggle("is-sel", on);
      const b = $(".opt__main", el);
      if (b && b.hasAttribute("aria-expanded")) b.setAttribute("aria-expanded", String(on));
      const more = $(".opt__more", el);
      if (more) more.inert = !on;                                   // hidden links leave the tab order
    });
    try { drawHeroMap(); } catch (err) { console.error(err); }
    relayout();
  });

  async function showAlternatives(btn) {
    const list = btn.nextElementSibling;
    if (!list.hidden) { list.hidden = true; relayout(); return; }
    btn.disabled = true;
    try {
      const q = new URLSearchParams({ source: S.from.name, destination: S.to.name, mode: btn.dataset.alts, k: 3 });
      const alts = await api(`/api/routes/alternatives?${q}`);
      list.innerHTML = alts.map((a) => `<li><b>#${a.rank}</b> ${a.path.map(esc).join(" → ")}<small>${km(a.distance_km)} · ${minutes(a.time_min)} · ${rupees(a.fare)}</small></li>`).join("")
        || "<li>No other routes.</li>";
      list.hidden = false;
    } catch (err) { snack(err.message, { warn: true }); }
    finally { btn.disabled = false; relayout(); }
  }

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
    $("#map-cap").innerHTML = `${icon(MODE_ICON[o.mode] || "near_me", "", true)}<span><b>${esc(o.label)}</b> · ${esc(r.origin.name)} → ${esc(r.destination.name)} · ${minutes(o.time_min)} · ${fareText(o)}</span>`;
  }

  function drawHeroMap() {
    const r = S.result;
    if (!r) return;
    const o = r.options.find((x) => x.id === S.sel) || r.options[0];
    if (!o) return;
    captionFor(o);
    if (r.kind === "live") drawLeaflet(r, o); else drawDemoMap(r, o);
  }

  function ensureLeaflet() {
    const host = $("#hero-map");
    if (typeof L === "undefined") {
      host.innerHTML = `<div class="map-off">${icon("wifi_off")}<span>Map library could not load</span></div>`;
      return false;
    }
    if (S.map) return true;
    host.innerHTML = "";
    S.map = L.map(host, {
      scrollWheelZoom: false, zoomControl: true, attributionControl: true,
      dragging: !L.Browser.mobile, tap: false,                  // one finger scrolls the page on phones
    }).setView([12.97, 77.6], 12);
    L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
      maxZoom: 19, attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
    }).addTo(S.map);
    S.metroLayer = L.layerGroup().addTo(S.map);
    S.layers = L.layerGroup().addTo(S.map);
    if (P.lines.length) drawMetroOverlay();
    S.map.on("click", mapPick);
    $("#map-hint").hidden = false;
    return true;
  }

  function drawMetroOverlay() {
    if (!S.metroLayer) return;
    S.metroLayer.clearLayers();
    P.lines.forEach((line) => L.polyline(line.coords, { color: safeColor(line.color), weight: 3, opacity: 0.35, interactive: false }).addTo(S.metroLayer));
  }

  function mapPick(e) {
    if (S.mode !== "live") return;
    const { lat, lng } = e.latlng;
    const wrap = document.createElement("div");
    wrap.className = "pick";
    wrap.innerHTML = `<button type="button" data-end="from">${icon("trip_origin")}Start here</button><button type="button" data-end="to">${icon("location_on", "", true)}Go here</button>`;
    const pop = L.popup({ closeButton: false, className: "pick-pop" }).setLatLng(e.latlng).setContent(wrap).openOn(S.map);
    wrap.addEventListener("click", async (ev) => {
      const b = ev.target.closest("button[data-end]");
      if (!b) return;
      S.map.closePopup(pop);
      try {
        const place = await api(`/api/live/reverse?lat=${lat}&lng=${lng}`);
        const p = { name: place.name, lat, lng };
        if (b.dataset.end === "from") { S.from = p; fromBox.set(p); } else { S.to = p; toBox.set(p); }
        snack(`${b.dataset.end === "from" ? "Start" : "Destination"}: ${place.name}`);
        maybeCompare();
      } catch (err) { snack(err.message, { warn: true }); }
    });
  }

  function drawLeaflet(r, o) {
    if (!ensureLeaflet()) return;
    S.layers.clearLayers();
    const a = [r.origin.lat, r.origin.lng], b = [r.destination.lat, r.destination.lng];
    const pts = [a, b];
    if (o.mode === "metro") {
      let cursor = a;
      o.legs.forEach((leg, i) => {
        if (leg.kind === "metro") {
          L.polyline(leg.coords, { color: safeColor(leg.color), weight: 7, opacity: 0.95 }).addTo(S.layers);
          leg.coords.forEach((c) => L.circleMarker(c, { radius: 3, color: "#fff", weight: 1, fillColor: safeColor(leg.color), fillOpacity: 1 }).addTo(S.layers));
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
      L.polyline(path, { color: "#16181D", weight: 7, opacity: 0.9, dashArray: o.mode === "bus" || o.mode === "walk" ? "12 9" : null }).addTo(S.layers);
      pts.push(...path);
    }
    const pin = (cls) => L.divIcon({ className: "", html: `<div class="pin ${cls}"></div>`, iconSize: [26, 26], iconAnchor: [13, 13] });
    L.marker(a, { icon: pin("pin--a"), keyboard: false }).bindTooltip(`Start: ${esc(r.origin.name)}`).addTo(S.layers);
    L.marker(b, { icon: pin("pin--b"), keyboard: false }).bindTooltip(`Destination: ${esc(r.destination.name)}`).addTo(S.layers);
    S.map.invalidateSize();
    S.map.fitBounds(pts, { padding: [40, 40], animate: !reduce });
  }

  function drawDemoMap(r, o) {
    if (S.map) { S.map.remove(); S.map = null; S.layers = null; S.metroLayer = null; }
    $("#map-hint").hidden = true;
    const host = $("#hero-map");
    if (!S.locations.length) return;
    const xs = S.locations.map((l) => l.x), ys = S.locations.map((l) => l.y);
    const minX = Math.min(...xs), maxX = Math.max(...xs), maxY = Math.max(...ys), minY = Math.min(...ys);
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
    (o.legs || []).forEach((leg) => {
      const color = o.variant === "mixed" ? (MODE_STROKE[leg.mode] || "#304FFE") : "#304FFE";
      parts.push(`<line class="dm-route" style="stroke:${color}" x1="${pos[leg.from][0]}" y1="${pos[leg.from][1]}" x2="${pos[leg.to][0]}" y2="${pos[leg.to][1]}"/>`);
    });
    const ends = [r.origin.name, r.destination.name];
    S.locations.forEach((l) => {
      const [x, y] = pos[l.name];
      const cls = ends.includes(l.name) ? "end" : onPath.has(l.name) ? "on" : "";
      parts.push(`<circle class="dm-node ${cls}" cx="${x}" cy="${y}" r="${cls === "end" ? 14 : 11}"/>`);
      parts.push(`<text class="dm-label" x="${x}" y="${y + 34}" text-anchor="middle">${esc(l.name)}</text>`);
    });
    host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="xMidYMid meet" aria-hidden="true">${parts.join("")}</svg>`;
  }

  // ------------------------------------------------- chips, save, share, URL
  function renderChips() {
    const saved = savedTrips().map((t) => ({ ...t, kind: "saved" }));
    const recent = store.get(recentKey(), []).map((t) => ({ ...t, kind: "recent" }));
    const quick = (S.mode === "live" ? LIVE_QUICK : DEMO_QUICK.map(([a, b]) => ({ from: { name: a }, to: { name: b } })))
      .map((t) => ({ ...t, kind: "quick" }));
    const seen = new Set(), chips = [];
    [...saved, ...recent.slice(0, 2), ...quick].forEach((t) => {
      const k = tripKey(t);
      if (!seen.has(k) && chips.length < 5) { seen.add(k); chips.push(t); }
    });
    const key = JSON.stringify(chips.map((c) => c.kind + tripKey(c)));
    if (key === S.chipsKey) return;                      // unchanged: keep focus where it is
    const box = $("#quick");
    const focusedIndex = box.contains(document.activeElement) ? Number(document.activeElement.dataset.i) : -1;
    S.chipsKey = key;
    S.chips = chips;
    box.innerHTML = chips.map((c, i) => `<button type="button" class="chip rp-host chip--${c.kind}" data-i="${i}" style="animation-delay:${i * 50}ms">${icon(c.kind === "saved" ? "star" : c.kind === "recent" ? "schedule" : "near_me", "", c.kind === "saved")}${esc(c.from.name)} → ${esc(c.to.name)}</button>`).join("");
    if (focusedIndex >= 0 && box.children[Math.min(focusedIndex, box.children.length - 1)]) box.children[Math.min(focusedIndex, box.children.length - 1)].focus();
  }

  $("#quick").addEventListener("click", (e) => {
    const chip = e.target.closest(".chip");
    if (!chip) return;
    const c = S.chips[Number(chip.dataset.i)];
    setTrip(c.from, c.to);
  });

  $("#save-trip").addEventListener("click", () => {
    if (!S.from || !S.to) return;
    const all = store.get("stcf-saved", []);
    const key = tripKey({ from: S.from, to: S.to });
    const exists = all.some((t) => t.mode === S.mode && tripKey(t) === key);
    const next = exists ? all.filter((t) => !(t.mode === S.mode && tripKey(t) === key))
      : [{ mode: S.mode, from: S.from, to: S.to }, ...all].slice(0, 12);
    store.set("stcf-saved", next);
    $("#save-trip").setAttribute("aria-pressed", String(!exists));
    snack(exists ? "Removed from saved trips" : "Saved: it is now one tap away");
    renderChips();
  });

  function tripUrl() {
    const enc = (p) => (S.mode === "live" ? `${p.lat.toFixed(5)},${p.lng.toFixed(5)},${p.name}` : p.name);
    const q = new URLSearchParams({ m: S.mode, f: enc(S.from), t: enc(S.to), p: S.priority });
    if (S.at) q.set("at", S.at);
    return `${location.origin}${location.pathname}?${q}`;
  }
  function syncUrl() { try { history.replaceState(null, "", tripUrl()); } catch { /* ignore */ } }

  $("#share-trip").addEventListener("click", async () => {
    if (!S.from || !S.to) return;
    const url = tripUrl(), title = `${S.from.name} → ${S.to.name}`;
    try {
      if (navigator.share) { await navigator.share({ title, url }); return; }
      await navigator.clipboard.writeText(url);
      snack("Link copied. Anyone with it sees this trip.");
    } catch { snack("Could not share. Copy the address bar instead.", { warn: true }); }
  });

  function tripFromUrl() {
    const q = new URLSearchParams(location.search);
    const m = q.get("m"), f = q.get("f"), t = q.get("t");
    if (!m || !f || !t) return null;
    const dec = (v) => {
      if (m !== "live") return { name: v };
      const [lat, lng, ...name] = v.split(",");
      const p = { lat: Number(lat), lng: Number(lng), name: name.join(",") || "Pinned place" };
      return Number.isFinite(p.lat) && Number.isFinite(p.lng) ? p : null;
    };
    const from = dec(f), to = dec(t);
    if (!from || !to) return null;
    return { mode: m === "demo" ? "demo" : "live", from, to, priority: q.get("p"), at: q.get("at") };
  }

  // ---------------------------------------------------------- swap, locate
  $("#swap").addEventListener("click", () => {
    if (!S.from && !S.to) return;
    [S.from, S.to] = [S.to, S.from];
    fromBox.set(S.from); toBox.set(S.to);
    maybeCompare();
  });

  function locate(end = "from") {
    const btn = $("#locate");
    if (!navigator.geolocation) return snack("This browser cannot share its location.", { warn: true });
    const mode = S.mode;
    btn.classList.add("is-busy");
    navigator.geolocation.getCurrentPosition(
      (pos) => {
        btn.classList.remove("is-busy");
        if (mode !== S.mode) return;                       // switched to demo meanwhile
        const p = { name: "My location", lat: pos.coords.latitude, lng: pos.coords.longitude };
        if (end === "to") { S.to = p; toBox.set(p); } else { S.from = p; fromBox.set(p); }
        snack("Using your current location");
        maybeCompare();
      },
      () => { btn.classList.remove("is-busy"); snack("Location permission was denied. Type a place instead.", { warn: true }); },
      { enableHighAccuracy: true, timeout: 10000 },
    );
  }
  $("#locate").addEventListener("click", () => locate("from"));

  // --------------------------------------------- priority and departure time
  const priGroup = radiogroup($(".pri"), "p", (p) => {
    S.priority = p;
    store.set("stcf-priority", p);
    maybeCompare();
  });
  $("#leave-at").addEventListener("change", (e) => {
    S.at = e.target.value || "";
    $(".when").classList.toggle("is-set", !!S.at);
    maybeCompare();
  });
  $("#leave-now").addEventListener("click", () => {
    if (!S.at) return;
    S.at = ""; $("#leave-at").value = ""; $(".when").classList.remove("is-set");
    maybeCompare();
  });

  // ------------------------------------------------------------ mode switch
  const modeGroup = radiogroup($(".seg"), "mode", (mode) => setMode(mode));
  function setMode(mode, { quiet = false, trip = null } = {}) {
    S.mode = mode;
    S.seq++;                                    // drop answers for the old mode
    document.body.dataset.mode = mode;
    modeGroup.set(mode);
    store.set("stcf-mode", mode);
    $("#hero-sub").textContent = mode === "live"
      ? "Walk, bus, metro, auto, bike taxi and cab on live Bengaluru roads."
      : "The offline demo city: 12 sample places, sample fares, no internet needed.";
    $("#plan-from").placeholder = mode === "live" ? "Start typing a place" : "Pick a demo place";
    $("#plan-to").placeholder = mode === "live" ? "Where are you going?" : "Pick a demo place";
    $("#locate").hidden = mode !== "live";
    $(".when").hidden = mode !== "live";
    S.result = null;
    S.chipsKey = "";
    renderChips();
    if (trip) setTrip(trip.from, trip.to);
    else {
      const recent = store.get(recentKey(), [])[0];
      if (recent) setTrip(recent.from, recent.to);
      else if (mode === "live") setTrip(LIVE_QUICK[0].from, LIVE_QUICK[0].to);
      else setTrip({ name: "College" }, { name: "Airport" });
    }
    if (!quiet) snack(mode === "live" ? "Live Bengaluru: real roads, real metro timetable" : "Demo city: works fully offline");
  }

  // =================================================================== PEAK
  // Scroll is the playhead of a real search on the real metro timetable.
  const P = {
    st: {}, lines: [], trace: null, from: "WHTM", to: "APTS", algo: "dijkstra", step: -9, done: null,
    active: false, running: false, els: {}, edges: [], seq: 0, dragging: false, playing: false, pops: {},
  };
  const peak = $("#dijkstra");
  const stage = $("[data-stage]", peak);
  const svg = $("#metro");
  const NS = "http://www.w3.org/2000/svg";
  const A = 0.03, B = 0.82, DONE = 0.86;      // event window and "route found" point in act progress

  async function initPeak() {
    let data;
    try { data = await api("/api/live/metro"); } catch { data = { stations: [] }; }
    if (!data.stations.length) {
      // No metro data: remove the section instead of leaving empty pinned scroll.
      ["#hush", "#dijkstra"].forEach((s) => $(s).remove());
      $("#tab-dijkstra").remove();
      relayout();
      return;
    }
    data.stations.forEach((s) => { P.st[s.id] = s; });
    P.lines = data.lines;
    drawMetro();
    fillSelects();
    drawMetroOverlay();
    await loadTrace();
    new IntersectionObserver((entries) => {
      P.active = entries[entries.length - 1].isIntersecting;
      if (P.active && !P.running) { P.running = true; requestAnimationFrame(tick); }
    }, { rootMargin: "20% 0px" }).observe(peak);
  }

  const lineColor = (id) => safeColor((P.lines.find((x) => x.id === id) || {}).color);
  const lineName = (id) => ((P.lines.find((x) => x.id === id) || {}).name || id).replace(" Line", "");

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
        el.style.setProperty("--lc", safeColor(line.color));
        gEdges.appendChild(el);
        P.edges.push({ a: a.id, b: b.id, el });
      }
    });
    all.forEach((st) => {
      const [x, y] = pos(st);
      st.x = x; st.y = y;
      const g = document.createElementNS(NS, "g");
      g.setAttribute("class", "m-st");
      g.dataset.id = st.id;
      g.innerHTML = `<circle class="ring" cx="${x}" cy="${y}" r="5"/><circle class="dot" cx="${x}" cy="${y}" r="4.2"/><circle cx="${x}" cy="${y}" r="13" fill="transparent"><title>${esc(st.name)}</title></circle>`;
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
    // Mouse/touch shortcut. Keyboard users use the two station selects instead,
    // so 80 stations do not each become a tab stop.
    svg.addEventListener("click", (e) => {
      const g = e.target.closest(".m-st");
      if (g) retarget(g.dataset.id);
    });
  }

  function fillSelects() {
    const opts = Object.values(P.st).sort((a, b) => a.name.localeCompare(b.name))
      .map((s) => `<option value="${esc(s.id)}">${esc(s.name)}</option>`).join("");
    $("#run-from").innerHTML = opts;
    $("#run-to").innerHTML = opts;
    $("#run-from").addEventListener("change", (e) => changeEnds(e.target.value, P.to, e.target));
    $("#run-to").addEventListener("change", (e) => changeEnds(P.from, e.target.value, e.target));
  }

  function changeEnds(from, to, select) {
    if (from === to) {
      snack("Pick two different stations.", { warn: true });
      if (select) select.value = select.id === "run-from" ? P.from : P.to;
      return;
    }
    loadTrace(true, from, to);
  }

  function retarget(id) {
    if (id === P.from) return snack("That is the start station. Pick another one.", { warn: true });
    loadTrace(true, P.from, id);
  }

  async function loadTrace(announce, from = P.from, to = P.to) {
    if (from === to) return;
    const mine = ++P.seq;
    const q = new URLSearchParams({ from, to, algorithm: P.algo });
    if (S.at) q.set("at", S.at);
    let trace;
    try { trace = await api(`/api/live/metro/trace?${q}`); } catch (err) { if (mine === P.seq) snack(err.message, { warn: true }); return; }
    if (mine !== P.seq) return;                     // a newer request won
    P.trace = trace;
    P.from = from; P.to = to;                       // only now that it succeeded
    $("#run-from").value = from;
    $("#run-to").value = to;
    P.pops[`${from}|${to}|${trace.at}|${trace.algorithm}`] = trace.events.length;
    $("#run-title").textContent = trace.algorithm === "astar" ? "A* · live run" : "Dijkstra · live run";
    $("#run-tip").innerHTML = trace.timetable
      ? `${icon("schedule")}<span>Real timetable, leaving at ${esc(trace.at)}. Tap a station to make it the destination.</span>`
      : `${icon("info")}<span>No trains run at ${esc(trace.at)}, so this uses average waiting times.</span>`;
    P.step = -9; P.done = null;
    P.gRoute.innerHTML = "";
    $("#run-result").hidden = true;                 // never show the previous run's fare
    $("#run").classList.remove("is-done");
    if (announce) {
      const other = P.pops[`${from}|${to}|${trace.at}|${trace.algorithm === "astar" ? "dijkstra" : "astar"}`];
      snack(other != null
        ? `${trace.algorithm === "astar" ? "A*" : "Dijkstra"}: ${trace.events.length} heap pops (the other one took ${other})`
        : `Replaying ${trace.events.length} heap pops to ${trace.to.name}`);
    }
    render(progress(), true);
  }

  function syncPeakToResult() {
    const r = S.result;
    if (!r || r.kind !== "live" || !P.trace) return;
    const m = r.options.find((o) => o.mode === "metro");
    if (!m) return;
    const rides = m.legs.filter((l) => l.kind === "metro" && l.from_id);
    if (!rides.length) return;
    loadTrace(false, rides[0].from_id, rides[rides.length - 1].to_id);
  }

  function progress() {
    const v = parseFloat(peak.style.getPropertyValue("--p"));
    if (!Number.isNaN(v)) return v;
    const r = peak.getBoundingClientRect();
    return Math.min(1, Math.max(0, -r.top / Math.max(1, r.height - innerHeight)));
  }

  let lastP = -1;
  function tick() {
    if (!P.active) { P.running = false; return; }
    const p = progress();
    if (p !== lastP) { lastP = p; render(p); }
    requestAnimationFrame(tick);
  }

  const stepOf = (p, N) => (p <= A ? -1 : Math.min(N - 1, Math.floor(((p - A) / (B - A)) * N)));
  const pOf = (k, N) => (k < 0 ? 0 : k >= N ? (DONE + 1) / 2 : A + ((k + 0.5) / N) * (B - A));

  function render(p, force) {
    const tr = P.trace;
    if (!tr) return;
    const ev = tr.events, N = ev.length;
    const k = stepOf(p, N);
    const done = p >= DONE;
    if (!P.dragging) $("#trace-range").value = String(Math.round(p * 1000));
    if (!force && k === P.step && done === P.done) return;
    P.step = k; P.done = done;
    stage.setAttribute("data-verify-state", `${k}|${done ? 1 : 0}`);

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
      ? `Settled <b>${esc(P.st[now.station].name)}</b> on ${esc(lineName(now.line))} at +${minutes(now.t)}${now.frontier.length ? `, pushed ${now.frontier.length} neighbour${now.frontier.length === 1 ? "" : "s"}` : ""}`
      : `Start at <b>${esc(tr.from.name)}</b>. Every other station is ∞.`;
    $("#heap").innerHTML = (now ? now.heap : []).map((h) =>
      `<li><i style="--lc:${lineColor(h.line)}"></i><span>${esc(P.st[h.station].name)}</span><span>${minutes(h.t)}</span></li>`).join("")
      || `<li><i></i><span>${now ? "Destination reached, the run stops" : "Start station, 0 min"}</span><span></span></li>`;
    const recent = ev.slice(Math.max(0, k - 5), k + 1).reverse();
    $("#log").innerHTML = recent.map((e) =>
      `<li><i style="--lc:${lineColor(e.line)}"></i><span>${esc(P.st[e.station].name)}</span><span>${minutes(e.t)}</span></li>`).join("");

    const result = $("#run-result");
    const run = $("#run");
    if (done && tr.route) {
      if (!P.gRoute.childNodes.length) drawRoute(tr);
      if (result.hidden) {
        const changes = tr.route.changes;
        const times = tr.timetable ? tr.route.rides.map((r) => `${esc(lineName(r.line))} ${esc(r.depart)}→${esc(r.arrive)}`).join(", ") : "";
        result.innerHTML = `<b>${tr.fare.token != null ? rupees(tr.fare.token) : "Fare n/a"}</b> · ${minutes(tr.route.time_min)}
          <p>${esc(tr.from.name)} → ${esc(tr.to.name)}, ${changes ? `${changes} line change${changes > 1 ? "s" : ""}` : "no line change"}.</p>
          ${times ? `<p>Trains: ${times}.</p>` : ""}
          <p>Official BMRCL fare${tr.fare.smart_card ? `, smart card ₹${tr.fare.smart_card} (${esc(tr.fare.timeframe)})` : ""}. ${N} heap pops (${tr.algorithm === "astar" ? "A*" : "Dijkstra"}).</p>`;
        result.hidden = false;
      }
      run.classList.add("is-done");
    } else {
      P.gRoute.innerHTML = "";
      result.hidden = true;
      run.classList.remove("is-done");
    }
  }

  function drawRoute(tr) {
    tr.route.rides.forEach((ride, i) => {
      const pts = ride.stations.map((id) => `${P.st[id].x},${P.st[id].y}`).join(" ");
      const pl = document.createElementNS(NS, "polyline");
      pl.setAttribute("points", pts);
      pl.setAttribute("class", "m-route draw");
      pl.style.setProperty("--lc", safeColor(ride.color));
      P.gRoute.appendChild(pl);
      const len = pl.getTotalLength ? pl.getTotalLength() : 1000;
      pl.style.setProperty("--len", len);
      pl.style.animationDelay = `${i * 260}ms`;
    });
    const d = P.st[tr.to.id];
    if (tr.fare.token == null) return;
    const label = rupees(tr.fare.token);
    const g = document.createElementNS(NS, "g");
    g.setAttribute("class", "m-fare");
    g.innerHTML = `<rect x="${d.x - 10}" y="${d.y - 58}" rx="14" width="${22 + label.length * 11}" height="30"/><text x="${d.x + 1}" y="${d.y - 37}">${esc(label)}</text>`;
    P.gRoute.appendChild(g);
  }

  // --- replay controls: scrubber, step buttons, play/pause
  function scrollToProgress(p) {
    const top = peak.getBoundingClientRect().top + scrollY;
    scrollTo({ top: top + p * (peak.offsetHeight - innerHeight), behavior: "instant" });
  }
  const range = $("#trace-range");
  range.addEventListener("pointerdown", () => { P.dragging = true; stopPlay(); });
  addEventListener("pointerup", () => { P.dragging = false; });
  range.addEventListener("input", () => scrollToProgress(Number(range.value) / 1000));
  range.addEventListener("keydown", (e) => {
    const delta = { ArrowRight: 1, ArrowUp: 1, ArrowLeft: -1, ArrowDown: -1 }[e.key];
    if (!delta || !P.trace) return;
    e.preventDefault();
    step(delta);
  });
  function step(delta) {
    if (!P.trace) return;
    stopPlay();
    const N = P.trace.events.length;
    const k = Math.max(-1, Math.min(N, (P.done ? N : P.step) + delta));
    scrollToProgress(pOf(k, N));
  }
  $("#step-back").addEventListener("click", () => step(-1));
  $("#step-next").addEventListener("click", () => step(1));

  let playTimer = null;
  function stopPlay() {
    P.playing = false;
    cancelAnimationFrame(playTimer);
    $("#play").setAttribute("aria-label", "Play the run");
    $("#play").innerHTML = icon("play_arrow", "", true);
  }
  $("#play").addEventListener("click", () => {
    if (P.playing) return stopPlay();
    if (!P.trace) return;
    P.playing = true;
    $("#play").setAttribute("aria-label", "Pause");
    $("#play").innerHTML = icon("pause", "", true);
    let p = progress() >= DONE ? 0 : progress();
    const total = reduce ? 1 : 9000;                 // ms for a full run
    let last = performance.now();
    const frame = (t) => {
      if (!P.playing) return;
      p = Math.min(1, p + (t - last) / total);
      last = t;
      scrollToProgress(p);
      if (p >= 0.95) return stopPlay();
      playTimer = requestAnimationFrame(frame);
    };
    playTimer = requestAnimationFrame(frame);
  });
  addEventListener("wheel", () => { if (P.playing) stopPlay(); }, { passive: true });
  addEventListener("touchstart", () => { if (P.playing) stopPlay(); }, { passive: true });

  radiogroup($(".algo"), "a", (a) => { P.algo = a; loadTrace(true); });

  // ================================================================== FARES
  async function initFares() {
    let rules = [];
    try { rules = await api("/api/live/fares"); } catch { return; }
    rules.forEach((r) => {
      const card = $(`.rate[data-mode="${r.mode}"]`);
      if (!card) return;
      $(".rate__kind", card).innerHTML = `<span class="kind kind--${esc(r.kind)}">${esc(KIND[r.kind] || r.kind)}</span>`;
      $(".rate__rule", card).textContent = r.rule;
      $(".rate__ex", card).textContent = r.example;
      $(".rate__src", card).textContent = r.source;
    });
    relayout();
    if (reduce) { $$(".rate").forEach((c) => c.classList.add("is-in")); return; }
    const io = new IntersectionObserver((entries) => entries.forEach((en) => {
      if (en.isIntersecting) { en.target.classList.add("is-in"); io.unobserve(en.target); }
    }), { threshold: 0.6 });
    $$(".rate").forEach((c) => io.observe(c));
  }

  // ================================================================= SYSTEM
  function countUp(el, target) {
    if (!Number.isFinite(target)) return;
    el.dataset.target = String(target);
    if (reduce || el.dataset.seen) { el.textContent = String(target); return; }
  }
  const countIO = new IntersectionObserver((entries) => entries.forEach((en) => {
    if (!en.isIntersecting) return;
    const el = en.target;
    el.dataset.seen = "1";
    countIO.unobserve(el);
    const target = Number(el.dataset.target);
    if (!Number.isFinite(target)) return;
    if (reduce) { el.textContent = String(target); return; }
    const t0 = performance.now(), dur = 1400;
    const frame = (t) => {
      const x = Math.min(1, (t - t0) / dur), eased = 1 - (1 - x) ** 3;      // ease-out: lands, not spins
      el.textContent = String(Math.round(target * eased));
      if (x < 1) requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }), { threshold: 0.5 });
  $$("[data-count]").forEach((el) => countIO.observe(el));

  function applyStatus(st) {
    S.status = st;
    countUp($("#tc-stations"), st.metro_stations);
    countUp($("#tc-lines"), st.metro_lines);
    countUp($("#tc-states"), st.metro_states);
    countUp($("#tc-demo"), st.demo_locations);
    countUp($("#tc-conn"), st.demo_connections);
    $("#tc-calls").textContent = st.map_calls ? String(st.map_calls.calls_today) : "n/a";
    $("#tc-feed").textContent = st.metro_feed_days_left != null ? `${st.metro_feed_days_left} more days` : "n/a";
    $("#tc-maps").innerHTML = st.maps_configured
      ? `${icon("verified", "", true)}Live from Ola Maps`
      : `${icon("wifi_off")}Offline demo only`;
    const warn = $("#tc-warn");
    warn.hidden = !(st.warnings && st.warnings.length);
    warn.textContent = (st.warnings || []).join(" ");
  }
  async function refreshStatus() {
    try { applyStatus(await api("/api/live/status")); } catch { /* telemetry is optional */ }
  }

  // ================================================================ CHROME
  addEventListener("scroll", () => $("#appbar").classList.toggle("is-raised", scrollY > 8), { passive: true });
  const tabFor = { plan: "top", dijkstra: "dijkstra", fares: "fares", system: "system" };
  const tabIO = new IntersectionObserver((entries) => entries.forEach((en) => {
    if (!en.isIntersecting) return;
    const t = tabFor[en.target.id];
    $$(".tab").forEach((a) => a.classList.toggle("is-on", a.dataset.tab === t));
  }), { rootMargin: "-45% 0px -50% 0px" });
  Object.keys(tabFor).forEach((id) => { const el = document.getElementById(id); if (el) tabIO.observe(el); });

  /** The scroll engine caches section positions; re-measure whenever the layout changes. */
  let relayoutTimer = null;
  function relayout() {
    clearTimeout(relayoutTimer);
    relayoutTimer = setTimeout(() => { if (S.sc && S.sc.layout) S.sc.layout(); }, 60);
  }

  // ================================================================== START
  async function start() {
    if ("scrollRestoration" in history) history.scrollRestoration = "manual";
    if (!location.hash) scrollTo(0, 0);
    hydrateIcons();
    if (window.Motion) S.sc = window.Motion.mount(document.body);
    if ("ResizeObserver" in window) new ResizeObserver(relayout).observe($("main"));

    S.priority = store.get("stcf-priority", "cheapest");
    if (!["cheapest", "balanced", "fastest"].includes(S.priority)) S.priority = "cheapest";
    const shared = tripFromUrl();
    if (shared && ["cheapest", "balanced", "fastest"].includes(shared.priority)) S.priority = shared.priority;
    if (shared && /^\d{2}:\d{2}$/.test(shared.at || "")) { S.at = shared.at; $("#leave-at").value = S.at; $(".when").classList.add("is-set"); }
    priGroup.set(S.priority);

    const [status, demo] = await Promise.all([
      api("/api/live/status", { timeout: 15000 }).catch(() => null),
      Promise.all([api("/api/locations"), api("/api/network")]).catch(() => [[], []]),
    ]);
    [S.locations, S.network] = demo;
    if (status) applyStatus(status);
    const live = !!(status && status.maps_configured);
    document.body.dataset.live = live ? "on" : "off";
    if (!live) {
      const btn = $('.seg__btn[data-mode="live"]');
      btn.disabled = true;
      btn.title = "Live mode is not set up on this server";
    }
    let mode = shared ? shared.mode : store.get("stcf-mode", live ? "live" : "demo");
    if (mode === "live" && !live) mode = "demo";
    setMode(mode, { quiet: true, trip: shared && shared.mode === mode ? shared : null });
    if (!live) snack("Live mode is not set up, showing the offline demo city", { warn: true, ms: 4200 });

    initPeak();
    initFares();
  }

  start();
})();
