/*
 * Live Bengaluru mode.
 *
 * Talks only to OUR backend (/api/live/...). The backend calls Ola Maps with the
 * secret API key, runs Dijkstra on the real Namma Metro graph and applies the
 * published fare rules. The map uses Leaflet + OpenStreetMap tiles.
 */
(function () {
  "use strict";

  const ICONS = { auto: "🛺", cab: "🚕", bike_taxi: "🏍️", bus: "🚌", metro: "🚇", walk: "🚶", road: "🛣️" };
  const KIND_LABELS = {
    official: ["Official fare", "kind-official"],
    regulated: ["Govt rate", "kind-regulated"],
    partial: ["Partial fare data", "kind-partial"],
    estimated: ["Estimated", "kind-partial"],
    indicative: ["Indicative", "kind-indicative"],
  };
  const AUTO_REFRESH_MS = 2 * 60 * 1000;
  const ORANGE = "#F26B1D";
  const CHARCOAL = "#23252B";

  const live = {
    status: null,
    from: null,          // {name, lat, lng}
    to: null,
    result: null,
    selected: null,
    map: null,
    layers: null,
    metroLayer: null,
    timer: null,
  };

  // ----------------------------------------------------------------- mode switch

  function setMode(mode) {
    document.body.dataset.mode = mode;
    document.querySelectorAll(".mode-btn").forEach((b) => b.setAttribute("aria-selected", b.dataset.mode === mode));
    try { localStorage.setItem("stcf-mode", mode); } catch { /* storage unavailable */ }
    if (mode === "live" && live.map) setTimeout(() => live.map.invalidateSize(), 50);
  }

  function savedMode() {
    try { return localStorage.getItem("stcf-mode"); } catch { return null; }
  }

  // ------------------------------------------------------------------- alerts

  function showLiveAlert(message, kind = "error") {
    const box = $("#live-alert");
    box.className = `alert ${kind}`;
    box.textContent = message;
    box.hidden = false;
  }

  function hideLiveAlert() { $("#live-alert").hidden = true; }

  function setLiveLoading(on) {
    $("#live-find").classList.toggle("loading", on);
    $("#live-find").disabled = on;
    $("#live-refresh").disabled = on;
  }

  // ------------------------------------------------------------ place search box

  function placeBox(inputId, listId, onPick) {
    const input = $(inputId);
    const list = $(listId);
    let items = [];
    let active = -1;
    let timer = null;
    let requestNo = 0;

    function close() {
      list.hidden = true;
      input.setAttribute("aria-expanded", "false");
      active = -1;
    }

    function render() {
      list.innerHTML = items.length
        ? items.map((p, i) => `
            <li role="option" id="${listId.slice(1)}-${i}" class="${i === active ? "active" : ""}" data-i="${i}" aria-selected="${i === active}">
              <b>${esc(p.name)}</b><span>${esc(p.address || "")}</span></li>`).join("")
        : '<li class="none">No matching places</li>';
      list.hidden = false;
      input.setAttribute("aria-expanded", "true");
    }

    async function pick(i) {
      const place = items[i];
      if (!place) return;
      close();
      input.value = place.name;
      try {
        let { lat, lng } = place;
        if (lat == null || lng == null) {
          ({ lat, lng } = await api(`/api/live/place?place_id=${encodeURIComponent(place.place_id)}`));
        }
        onPick({ name: place.name, lat, lng });
      } catch (error) {
        showLiveAlert(error.message);
      }
    }

    input.addEventListener("input", () => {
      onPick(null);                         // typing again clears the chosen place
      clearTimeout(timer);
      const text = input.value.trim();
      if (text.length < 2) { close(); return; }
      timer = setTimeout(async () => {      // debounce: wait until the user pauses typing
        const mine = ++requestNo;
        try {
          const results = await api(`/api/live/places?q=${encodeURIComponent(text)}`);
          if (mine !== requestNo) return;   // a newer request has been sent
          items = results.slice(0, 7);
          active = -1;
          render();
        } catch (error) {
          close();
          showLiveAlert(error.message);
        }
      }, 300);
    });

    input.addEventListener("keydown", (event) => {
      if (list.hidden) return;
      if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        const step = event.key === "ArrowDown" ? 1 : -1;
        active = (active + step + items.length) % items.length;
        render();
        input.setAttribute("aria-activedescendant", `${listId.slice(1)}-${active}`);
      } else if (event.key === "Enter" && active >= 0) {
        event.preventDefault();
        pick(active);
      } else if (event.key === "Escape") {
        close();
      }
    });

    list.addEventListener("mousedown", (event) => {
      const li = event.target.closest("li[data-i]");
      if (li) { event.preventDefault(); pick(Number(li.dataset.i)); }
    });
    input.addEventListener("blur", () => setTimeout(close, 150));

    return {
      set(place) { input.value = place ? place.name : ""; },
    };
  }

  // --------------------------------------------------------------- planning

  async function plan({ scroll = true } = {}) {
    hideLiveAlert();
    if (!live.from || !live.to) {
      showLiveAlert("Choose both places from the suggestion list (or use your location).", "warn");
      return;
    }
    setLiveLoading(true);
    try {
      const q = new URLSearchParams({
        from_lat: live.from.lat, from_lng: live.from.lng, from_name: live.from.name,
        to_lat: live.to.lat, to_lng: live.to.lng, to_name: live.to.name,
      });
      live.result = await api(`/api/live/plan?${q}`);
      live.selected = live.result.cheapest_mode;
      renderResults();
      loadRecent();
      if (scroll) $("#live-area").scrollIntoView({ behavior: "smooth" });
    } catch (error) {
      showLiveAlert(error.message);
    } finally {
      setLiveLoading(false);
    }
  }

  function setAutoRefresh(on) {
    clearInterval(live.timer);
    live.timer = on ? setInterval(() => { if (live.result) plan({ scroll: false }); }, AUTO_REFRESH_MS) : null;
  }

  // -------------------------------------------------------------- rendering

  function fareLabel(o) {
    if (o.fare_kind === "estimated") return `≈ ${rupees(o.fare)}`;
    return o.fare_kind === "partial" ? `${rupees(o.fare)}+` : rupees(o.fare);
  }

  function legSummary(o) {
    return o.legs.map((leg) => {
      if (leg.kind === "metro") return `<span class="line-pill" style="--lc:${esc(leg.color)}">${esc(leg.line.replace(" Line", ""))} · ${leg.stops} stop${leg.stops === 1 ? "" : "s"}</span>`;
      return `<span>${ICONS[leg.kind] || "•"} ${km(leg.distance_km)}</span>`;
    }).join('<span class="arrow">→</span>');
  }

  function renderResults() {
    const r = live.result;
    $("#live-empty").hidden = true;
    $("#live-results").hidden = false;
    $("#live-title").textContent = `${r.origin.name} → ${r.destination.name}`;
    const t = new Date(r.generated_at);
    $("#live-updated").textContent = `Live · updated ${t.toLocaleTimeString("en-IN", { hour: "2-digit", minute: "2-digit", timeZone: "Asia/Kolkata" })} IST`;

    const cheapest = r.options.find((o) => o.mode === r.cheapest_mode);
    const fastest = r.options.find((o) => o.mode === r.fastest_mode);
    const saves = fastest.fare - cheapest.fare;
    $("#live-summary").innerHTML =
      `💡 <b>Cheapest:</b> ${esc(cheapest.label)} at <b>${fareLabel(cheapest)}</b> (${minutes(cheapest.time_min)}). ` +
      `<b>Fastest:</b> ${esc(fastest.label)} in <b>${minutes(fastest.time_min)}</b> (${fareLabel(fastest)}).` +
      (cheapest.mode !== fastest.mode && saves > 0 ? ` Taking the cheapest saves about <b>${rupees(saves)}</b>.` : "");

    $("#live-stats").innerHTML = [
      ["Road distance", km(r.road.distance_km), "via Ola Maps"],
      ["Drive time now", minutes(r.road.duration_min), "includes live traffic"],
      ["Cheapest", fareLabel(cheapest), cheapest.label],
      ["Options", `${r.options.length} / ${r.options.length + r.unavailable.length}`,
        r.unavailable.length ? r.unavailable.map((u) => `No ${u.label}`).join(", ") : "all modes available"],
    ].map(([k, v, s]) => `<div class="stat"><div class="k">${k}</div><div class="v">${v}</div><div class="s">${esc(s)}</div></div>`).join("");

    renderCards();
    renderSelected();

    $("#live-notes").innerHTML = r.notes.map((n) => `<li>${esc(n)}</li>`).join("") +
      r.options.map((o) => `<li><b>${esc(o.label)}:</b> ${esc(o.fare_source)}</li>`).join("");
  }

  function renderCards() {
    const r = live.result;
    const cards = r.options.map((o) => {
      const [kindText, kindClass] = KIND_LABELS[o.fare_kind];
      const links = o.links.map((l) => `<a class="app-link" href="${esc(l.url)}" target="_blank" rel="noopener noreferrer">${esc(l.label)} ↗</a>`).join("");
      return `
      <div class="card live-card ${o.mode === live.selected ? "selected" : ""}" role="button" tabindex="0" data-mode="${o.mode}" aria-pressed="${o.mode === live.selected}">
        <span class="rank">#${o.rank} · SORTED BY FARE</span>
        <span class="mode">${ICONS[o.mode]} ${esc(o.label)}
          ${o.is_cheapest ? '<span class="badge cheap">CHEAPEST</span>' : ""}
          ${o.is_fastest ? '<span class="badge fast">FASTEST</span>' : ""}</span>
        <span class="providers">${esc(o.providers)}</span>
        <span class="fare">${fareLabel(o)}</span>
        <span class="kind ${kindClass}" title="${esc(o.fare_source)}">${kindText}</span>
        <span class="fare-note">${esc(o.fare_text)}</span>
        <span class="metrics"><span>⏱️ ${minutes(o.time_min)}</span><span>📏 ${km(o.distance_km)}</span></span>
        <span class="legs">${legSummary(o)}</span>
        <span class="links">${links}</span>
      </div>`;
    });
    const off = r.unavailable.map((u) => `
      <div class="card off">
        <span class="rank">NOT AVAILABLE</span>
        <span class="mode">${ICONS[u.mode] || ""} ${esc(u.label)} <span class="badge na">NO ROUTE</span></span>
        <span class="fare">—</span>
        <span class="path">${esc(u.reason)}</span>
      </div>`);
    $("#live-cards").innerHTML = cards.concat(off).join("");
  }

  function select(mode) {
    live.selected = mode;
    document.querySelectorAll(".live-card").forEach((c) => {
      c.classList.toggle("selected", c.dataset.mode === mode);
      c.setAttribute("aria-pressed", c.dataset.mode === mode);
    });
    renderSelected();
  }

  function renderSelected() {
    const o = live.result.options.find((x) => x.mode === live.selected);
    $("#live-map-title").textContent = `Route map · ${o.label}`;
    $("#live-details-title").textContent = `${ICONS[o.mode]} ${o.label}: step by step`;

    $("#live-steps").innerHTML = o.legs.map((leg) => {
      const what = leg.kind === "metro"
        ? `<span class="line-pill" style="--lc:${esc(leg.color)}">${esc(leg.line)}</span> ${leg.stops} stop${leg.stops === 1 ? "" : "s"}`
        : `${ICONS[leg.kind] || "•"} ${leg.kind === "walk" ? "Walk" : leg.kind === "road" ? "Road" : o.mode === leg.kind ? esc(o.label) : "Auto"}`;
      const fare = leg.fare ? ` · ${rupees(leg.fare)}` : "";
      return `<li><div class="step-what">${what}</div>
        <div class="step-where">${esc(leg.from)} → ${esc(leg.to)}</div>
        <div class="muted small">${km(leg.distance_km)} · ${minutes(leg.time_min)}${fare}</div></li>`;
    }).join("");

    const [kindText] = KIND_LABELS[o.fare_kind];
    $("#live-fare").innerHTML = `
      <div><b>Fare</b> <span class="muted">(${esc(kindText.toLowerCase())})</span>: ${esc(o.fare_text)}</div>
      <div class="total">${fareLabel(o)}</div>
      ${o.notes.map((n) => `<div class="muted small">• ${esc(n)}</div>`).join("")}`;

    drawMap(o);
  }

  // -------------------------------------------------------------------- map

  /** Decode a Google-style encoded polyline into [[lat, lng], ...]. */
  function decodePolyline(str, precision) {
    const factor = 10 ** precision;
    const points = [];
    let index = 0, lat = 0, lng = 0;
    while (index < str.length) {
      for (const which of [0, 1]) {
        let result = 0, shift = 0, byte;
        do {
          byte = str.charCodeAt(index++) - 63;
          result |= (byte & 0x1f) << shift;
          shift += 5;
        } while (byte >= 0x20 && index < str.length);
        const delta = result & 1 ? ~(result >> 1) : result >> 1;
        if (which === 0) lat += delta; else lng += delta;
      }
      points.push([lat / factor, lng / factor]);
    }
    return points;
  }

  function roadPath(r) {
    if (r.road.polyline) {
      for (const precision of [5, 6]) {
        try {
          const pts = decodePolyline(r.road.polyline, precision);
          if (pts.length > 1 && pts.every(([a, b]) => a > 12 && a < 14 && b > 77 && b < 78.5)) return pts;
        } catch { /* try the next precision */ }
      }
    }
    return [[r.origin.lat, r.origin.lng], [r.destination.lat, r.destination.lng]];
  }

  function ensureMap() {
    if (typeof L === "undefined") {
      $("#live-map").innerHTML = '<div class="map-off">Map unavailable offline (the map library loads from the internet).</div>';
      return false;
    }
    if (!live.map) {
      live.map = L.map("live-map", { scrollWheelZoom: false }).setView([12.9716, 77.5946], 12);
      L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 19,
        attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
      }).addTo(live.map);
      live.layers = L.layerGroup().addTo(live.map);
      api("/api/live/metro").then((data) => {
        live.metroLayer = L.layerGroup(data.lines.map((line) =>
          L.polyline(line.coords, { color: line.color, weight: 3, opacity: 0.35 }))).addTo(live.map);
      }).catch(() => {});
    }
    return true;
  }

  function drawMap(o) {
    if (!ensureMap()) return;
    const r = live.result;
    live.layers.clearLayers();
    const bounds = [];
    const add = (layer, pts) => { layer.addTo(live.layers); bounds.push(...pts); };

    const origin = [r.origin.lat, r.origin.lng];
    const dest = [r.destination.lat, r.destination.lng];

    if (o.mode === "metro") {
      let cursor = origin;
      o.legs.forEach((leg, i) => {
        if (leg.kind === "metro") {
          add(L.polyline(leg.coords, { color: leg.color, weight: 7, opacity: 0.95 }), leg.coords);
          leg.coords.forEach((c) => L.circleMarker(c, { radius: 3, color: "#fff", weight: 1, fillColor: leg.color, fillOpacity: 1 }).addTo(live.layers));
          cursor = leg.coords[leg.coords.length - 1];
        } else {
          const next = o.legs[i + 1];
          const end = i === 0 && next && next.coords ? next.coords[0] : dest;
          const start = i === 0 ? origin : cursor;
          add(L.polyline([start, end], { color: leg.kind === "walk" ? CHARCOAL : ORANGE, weight: 4, dashArray: "6 8" }), [start, end]);
        }
      });
    } else {
      const pts = roadPath(r);
      add(L.polyline(pts, { color: ORANGE, weight: 6, opacity: 0.9, dashArray: o.mode === "bus" ? "10 8" : null }), pts);
    }

    L.circleMarker(origin, { radius: 9, color: ORANGE, weight: 3, fillColor: CHARCOAL, fillOpacity: 1 })
      .bindTooltip(`Start: ${esc(r.origin.name)}`).addTo(live.layers);
    L.circleMarker(dest, { radius: 9, color: CHARCOAL, weight: 3, fillColor: ORANGE, fillOpacity: 1 })
      .bindTooltip(`Destination: ${esc(r.destination.name)}`).addTo(live.layers);
    bounds.push(origin, dest);
    live.map.fitBounds(bounds, { padding: [30, 30] });
  }

  // ---------------------------------------------------------------- history

  async function loadRecent() {
    try {
      const seen = new Set();
      const recent = (await api("/api/live/history?limit=12")).filter((h) => {
        const key = `${h.origin.name}|${h.destination.name}`;
        if (seen.has(key)) return false;     // show each trip once
        seen.add(key);
        return true;
      }).slice(0, 5);
      $("#live-recent").innerHTML = recent.length
        ? "<span>Recent:</span>" + recent.map((h, i) =>
            `<button type="button" class="chip" data-i="${i}">${esc(h.origin.name)} → ${esc(h.destination.name)}</button>`).join("")
        : "";
      $("#live-recent").onclick = (event) => {
        const chip = event.target.closest(".chip[data-i]");
        if (!chip) return;
        const h = recent[Number(chip.dataset.i)];
        live.from = h.origin; live.to = h.destination;
        fromBox.set(h.origin); toBox.set(h.destination);
        plan();
      };
    } catch { /* history is optional */ }
  }

  // ------------------------------------------------------------------- init

  const fromBox = placeBox("#live-from", "#live-from-list", (p) => { live.from = p; });
  const toBox = placeBox("#live-to", "#live-to-list", (p) => { live.to = p; });

  async function init() {
    document.querySelectorAll(".mode-btn").forEach((b) => b.addEventListener("click", () => setMode(b.dataset.mode)));

    $("#live-form").addEventListener("submit", (e) => { e.preventDefault(); plan(); });
    $("#live-refresh").addEventListener("click", () => plan({ scroll: false }));
    $("#auto-refresh").addEventListener("change", (e) => setAutoRefresh(e.target.checked));
    $("#live-swap").addEventListener("click", () => {
      [live.from, live.to] = [live.to, live.from];
      fromBox.set(live.from); toBox.set(live.to);
    });
    $("#live-cards").addEventListener("click", (e) => {
      if (e.target.closest("a")) return;          // app buttons open in a new tab
      const card = e.target.closest(".live-card");
      if (card) select(card.dataset.mode);
    });
    $("#live-cards").addEventListener("keydown", (e) => {
      const card = e.target.closest(".live-card");
      if (card && (e.key === "Enter" || e.key === " ") && e.target === card) { e.preventDefault(); select(card.dataset.mode); }
    });
    $("#locate-btn").addEventListener("click", () => {
      if (!navigator.geolocation) { showLiveAlert("Your browser cannot share its location.", "warn"); return; }
      $("#locate-btn").classList.add("busy");
      navigator.geolocation.getCurrentPosition(
        (pos) => {
          $("#locate-btn").classList.remove("busy");
          live.from = { name: "My location", lat: pos.coords.latitude, lng: pos.coords.longitude };
          fromBox.set(live.from);
        },
        () => {
          $("#locate-btn").classList.remove("busy");
          showLiveAlert("Location permission was denied. Type a place instead.", "warn");
        },
        { enableHighAccuracy: true, timeout: 10000 },
      );
    });

    try {
      live.status = await api("/api/live/status");
    } catch (error) {
      live.status = { maps_configured: false, metro_loaded: false };
    }
    const ready = live.status.maps_configured;
    $("#live-setup").hidden = ready;
    $("#live-form").classList.toggle("disabled", !ready);
    $("#live-form").querySelectorAll("input, button").forEach((el) => { el.disabled = !ready; });
    $("#live-status").innerHTML = ready
      ? `<span class="live-dot"></span> Live: Ola Maps connected${live.status.metro_loaded ? ` · Namma Metro data loaded (${live.status.metro_stations} stations)` : ""}`
      : "⚠️ Live mode is not set up yet: add an Ola Maps API key (steps below).";

    const params = new URLSearchParams(location.search);
    const startMode = params.get("from") ? "demo" : savedMode() || (ready ? "live" : "demo");
    setMode(startMode);
    if (ready) loadRecent();
  }

  init();
})();
