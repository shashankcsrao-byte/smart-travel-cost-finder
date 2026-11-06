/*
 * effects.js · word-level animation hooks and the cycling placeholder (no third-party code).
 *
 * Markup it understands:
 *   <h2 data-words>…</h2>           each word becomes <span class="w" style="--i:n">, so
 *                                   effects.css can move words one after another; with
 *                                   data-in, motion.js staggers them as the heading enters
 *   <p data-words="lit">…</p>       the same split; effects.css lights each word as it scrolls
 *   #again-input                    while empty and unfocused, its placeholder suggests places
 *   #ticker                         rebuilt from the .rate cards once app.js fills them
 *   --ax, --ay on <html>            pointer position, -1 to 1, for the landing's colour lights
 *   video[data-ambient]             footage plays only while on screen, never under reduced motion
 *
 * Load it before motion.js, so the words exist when motion.js starts watching [data-in].
 */
(function () {
  "use strict";

  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const PLACES = ["Indiranagar", "Kempegowda Airport", "MG Road", "Electronic City", "Whitefield", "Jayanagar"];
  const CYCLE_MS = 2600;
  const FADE_MS = 260;

  /** Wraps every word of el in a span; child elements (such as <code>) stay whole as one word. */
  function splitWords(el) {
    let i = 0;
    const word = (content) => {
      const span = document.createElement("span");
      span.className = "w";
      span.style.setProperty("--i", String(i++));
      span.append(content);
      return span;
    };
    Array.from(el.childNodes).forEach((node) => {
      if (node.nodeType === Node.ELEMENT_NODE) { node.replaceWith(word(node.cloneNode(true))); return; }
      if (node.nodeType !== Node.TEXT_NODE) return;
      const parts = node.textContent.split(/(\s+)/).filter(Boolean)
        .map((part) => (/^\s+$/.test(part) ? document.createTextNode(part) : word(part)));
      node.replaceWith(...parts);
    });
  }

  /** Suggests a destination in the closing search box, fading between names. */
  function cyclePlaceholder(input) {
    if (!input || reduce) return;
    const base = input.placeholder;
    let n = 0;
    setInterval(() => {
      const idle = !document.hidden && !input.value && document.activeElement !== input;
      if (!idle) { input.placeholder = base; return; }
      input.classList.add("ph-out");
      setTimeout(() => {
        input.placeholder = `Try ${PLACES[n++ % PLACES.length]}`;
        input.classList.remove("ph-out");
      }, FADE_MS);
    }, CYCLE_MS);
  }

  /** Builds the running ticker from the fare cards once app.js has filled them in. */
  function feedTicker(row, cards) {
    if (!row || !cards.length) return;
    let timer = null;
    const build = () => {
      const items = cards.map((card) => {
        const name = card.querySelector("h3");
        const example = card.querySelector(".rate__ex");
        if (!name || !example || !example.textContent.trim()) return null;
        const li = document.createElement("li");
        li.dataset.mode = card.dataset.mode;
        const b = document.createElement("b");
        b.textContent = name.textContent;
        li.append(b, ` ${example.textContent.trim()}`);
        return li;
      }).filter(Boolean);
      if (!items.length) return;
      const copies = items.map((li) => { const c = li.cloneNode(true); c.setAttribute("aria-hidden", "true"); return c; });
      row.replaceChildren(...items, ...copies);                  // two copies make the loop seamless
    };
    const observer = new MutationObserver(() => { clearTimeout(timer); timer = setTimeout(build, 150); });
    cards.forEach((card) => observer.observe(card, { childList: true, subtree: true, characterData: true }));
    build();
  }

  /** The landing's colour lights lean a little toward the pointer. */
  function leanLights() {
    if (reduce || !matchMedia("(hover: hover) and (pointer: fine)").matches) return;
    const root = document.documentElement;
    let last = null;
    addEventListener("pointermove", (e) => {
      if (!last) requestAnimationFrame(() => {
        root.style.setProperty("--ax", ((last.x / innerWidth) * 2 - 1).toFixed(3));
        root.style.setProperty("--ay", ((last.y / innerHeight) * 2 - 1).toFixed(3));
        last = null;
      });
      last = { x: e.clientX, y: e.clientY };
    }, { passive: true });
  }

  /** Footage plays only while it is on screen, and never under reduced motion. */
  function ambientVideo(videos) {
    videos.forEach((video) => {
      video.muted = true;
      if (reduce) { video.removeAttribute("autoplay"); video.pause(); return; }
      new IntersectionObserver(([entry]) => {
        if (entry.isIntersecting) video.play().catch(() => { /* autoplay refused: the poster stays */ });
        else video.pause();
      }, { rootMargin: "200px 0px" }).observe(video);
    });
  }

  document.querySelectorAll("[data-words]").forEach(splitWords);
  cyclePlaceholder(document.getElementById("again-input"));
  feedTicker(document.getElementById("ticker"), Array.from(document.querySelectorAll(".rate[data-mode]")));
  leanLights();
  ambientVideo(Array.from(document.querySelectorAll("video[data-ambient]")));
})();
