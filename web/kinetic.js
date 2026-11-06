/*
 * kinetic.js · the page's feel under the hand (no third-party code).
 *
 *   smooth scroll     the wheel eases the page instead of jumping it (fine pointers only);
 *                     the scroll position stays native, so sticky sections and motion.js work
 *   magnetic          buttons lean toward the pointer (the CSS `translate` property)
 *   tilt              fare cards and System tiles turn toward the pointer, with a glare
 *                     (--rx, --ry, --gx, --gy on the element)
 *   velocity          the ticker runs faster as you scroll and follows your direction;
 *                     [data-skew] elements lean with scroll speed (--skew)
 *   parallax          [data-speed] elements drift at their own rate as they pass
 *   scramble          nav links shuffle their letters on hover
 *
 * Load it after app.js. With reduced motion it does nothing.
 */
(function () {
  "use strict";

  if (matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  const fine = matchMedia("(hover: hover) and (pointer: fine)").matches;
  const root = document.documentElement;
  const lerp = (a, b, t) => a + (b - a) * t;
  const clamp = (x, lo, hi) => (x < lo ? lo : x > hi ? hi : x);

  const SCROLL_EASE = 0.11;
  const WHEEL_LINE_PX = 16;
  const NATIVE_SCROLL = ".menu, .run__log, .run__heap, .leaflet-control, select, textarea";
  const MAGNETIC = ".btn, .seg__btn, .pri__btn, .algo__btn, .chip, .icon-btn, .app, .brand";   // not .swap: it is positioned with translate
  const MAGNET_PULL = 0.28;
  const TILT = ".rate, .tile";
  const TILT_DEG = 8;
  const TICKER_BASE = 0.6;          // px per frame at rest
  const TICKER_GAIN = 0.35;         // extra px per frame for each px scrolled per frame
  const SKEW_MAX = 2.5;             // degrees

  /* ---------------------------------------------------------- smooth scroll -- */
  let target = scrollY;
  let current = scrollY;
  let easing = false;

  function maxScroll() { return root.scrollHeight - innerHeight; }

  function smoothScroll() {
    root.classList.add("has-smooth");
    addEventListener("wheel", (e) => {
      if (e.ctrlKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
      if (e.target instanceof Element && e.target.closest(NATIVE_SCROLL)) return;
      e.preventDefault();
      const step = e.deltaMode === 1 ? e.deltaY * WHEEL_LINE_PX : e.deltaMode === 2 ? e.deltaY * innerHeight : e.deltaY;
      if (!easing) current = target = scrollY;
      target = clamp(target + step, 0, maxScroll());
      easing = true;
    }, { passive: false });
    // Anything else that scrolls (keys, links, app.js) takes over; the ease picks up from there.
    addEventListener("scroll", () => { if (!easing) current = target = scrollY; }, { passive: true });
    addEventListener("keydown", () => { easing = false; });
    document.addEventListener("click", (e) => {
      const link = e.target instanceof Element ? e.target.closest('a[href^="#"]') : null;
      const dest = link && link.hash.length > 1 ? document.querySelector(link.hash) : null;
      if (!dest) return;
      e.preventDefault();
      const pad = parseFloat(getComputedStyle(root).scrollPaddingTop) || 0;
      current = scrollY;
      target = clamp(dest.getBoundingClientRect().top + scrollY - pad, 0, maxScroll());
      easing = true;
      history.replaceState(null, "", link.hash);
    });
  }

  function stepScroll() {
    if (!easing) return;
    current = lerp(current, target, SCROLL_EASE);
    if (Math.abs(target - current) < 0.5) { current = target; easing = false; }
    scrollTo(0, current);
  }

  /* --------------------------------------------------------------- magnetic -- */
  function magnetic() {
    let held = null;
    addEventListener("pointermove", (e) => {
      const el = e.target instanceof Element ? e.target.closest(MAGNETIC) : null;
      if (held && held !== el) { held.style.translate = ""; held.classList.remove("is-pulled"); held = null; }
      if (!el) return;
      const box = el.getBoundingClientRect();
      const dx = e.clientX - (box.left + box.width / 2);
      const dy = e.clientY - (box.top + box.height / 2);
      el.style.translate = `${(dx * MAGNET_PULL).toFixed(1)}px ${(dy * MAGNET_PULL).toFixed(1)}px`;
      el.classList.add("is-pulled");
      held = el;
    }, { passive: true });
  }

  /* ------------------------------------------------------------------- tilt -- */
  function tilt() {
    let held = null;
    const release = () => { if (held) { held.classList.remove("is-tilting"); held = null; } };
    addEventListener("pointermove", (e) => {
      const el = e.target instanceof Element ? e.target.closest(TILT) : null;
      if (held !== el) release();
      if (!el) return;
      const box = el.getBoundingClientRect();
      const px = (e.clientX - box.left) / box.width;
      const py = (e.clientY - box.top) / box.height;
      el.style.setProperty("--ry", `${((px - 0.5) * 2 * TILT_DEG).toFixed(2)}deg`);
      el.style.setProperty("--rx", `${((0.5 - py) * 2 * TILT_DEG).toFixed(2)}deg`);
      el.style.setProperty("--gx", `${(px * 100).toFixed(1)}%`);
      el.style.setProperty("--gy", `${(py * 100).toFixed(1)}%`);
      el.classList.add("is-tilting");
      held = el;
    }, { passive: true });
    document.addEventListener("pointerleave", release);
  }

  /* ------------------------------------------------- velocity and parallax -- */
  const ticker = document.querySelector(".ticker__row");
  const skewed = Array.from(document.querySelectorAll("[data-skew]"));
  const drifting = Array.from(document.querySelectorAll("[data-speed]")).map((el) => ({ el, speed: parseFloat(el.dataset.speed) || 0 }));
  let lastY = scrollY;
  let velocity = 0;
  let direction = 1;
  let tickerX = 0;

  function stepVelocity() {
    const dy = scrollY - lastY;
    lastY = scrollY;
    velocity = lerp(velocity, dy, 0.18);
    if (Math.abs(dy) > 0.5) direction = Math.sign(dy);

    if (ticker) {
      const half = ticker.scrollWidth / 2;
      tickerX += (TICKER_BASE + Math.abs(velocity) * TICKER_GAIN) * direction;
      if (half > 0) tickerX = ((tickerX % half) + half) % half;
      ticker.style.transform = `translate3d(${(-tickerX).toFixed(1)}px, 0, 0)`;
    }
    const skew = clamp(velocity * 0.12, -SKEW_MAX, SKEW_MAX).toFixed(2);
    skewed.forEach((el) => el.style.setProperty("--skew", `${skew}deg`));

    const mid = innerHeight / 2;
    drifting.forEach(({ el, speed }) => {
      const box = el.getBoundingClientRect();
      if (box.bottom < -200 || box.top > innerHeight + 200) return;
      const offset = (box.top + box.height / 2 - mid) * speed;
      el.style.translate = `0 ${offset.toFixed(1)}px`;
    });
  }

  /* --------------------------------------------------------------- scramble -- */
  function scramble() {
    const GLYPHS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ*#₹→";
    document.querySelectorAll(".tab, .btn--plain").forEach((el) => {
      const text = el.childNodes[0] && el.childNodes[0].nodeType === Node.TEXT_NODE ? el.childNodes[0] : null;
      if (!text) return;
      const final = text.textContent;
      if (!el.hasAttribute("aria-label")) el.setAttribute("aria-label", final.trim());
      let timer = null;
      el.addEventListener("pointerenter", () => {
        let frame = 0;
        clearInterval(timer);
        timer = setInterval(() => {
          frame += 1;
          text.textContent = final.split("").map((ch, i) => {
            if (ch === " " || i < frame / 2) return ch;
            return GLYPHS[Math.floor(Math.random() * GLYPHS.length)];
          }).join("");
          if (frame / 2 >= final.length) { clearInterval(timer); text.textContent = final; }
        }, 28);
      });
    });
  }

  /* ------------------------------------------------------------------ start -- */
  root.classList.add("has-kinetic");
  if (fine) { smoothScroll(); magnetic(); tilt(); scramble(); }
  (function frame() {
    stepScroll();
    stepVelocity();
    requestAnimationFrame(frame);
  })();
})();
