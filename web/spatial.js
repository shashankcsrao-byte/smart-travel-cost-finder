/*
 * spatial.js · the depth behind the spatial UI (no third-party code).
 *
 * It only writes CSS variables; spatial.css decides what they do.
 *   --px, --py   on <html>: where the pointer is in the window, -1 to 1 (parallax, window lean)
 *   --mx, --my   on the surface under the pointer: where inside it, in px (the hover highlight)
 *   --tx, --ty   the same, -1 to 1 (bento tiles tilt toward the pointer)
 *   --d, --ad    on each fare card: its offset from the screen centre, -1 to 1, and its size
 *                (the curved wall of cards)
 *   --i          on each bento tile: its order, for the staggered entrance
 *
 * It also marks <html> with .has-spatial (CSS only hides things it will reveal when this runs)
 * and adds .is-in to the bento and the floating windows when they first scroll into view.
 *
 * Load it after app.js, so its scroll handler runs after motion.js has moved the fare rail.
 */
(function () {
  "use strict";

  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const finePointer = matchMedia("(hover: hover) and (pointer: fine)").matches;
  const GAZE_SURFACES = ".opt, .chip, .tile, .field, .app, .rate";
  const REVEAL_TARGETS = "#bento, .system__wrap, .again__inner, .run";
  const root = document.documentElement;
  const fixed3 = (x) => x.toFixed(3);
  const clampUnit = (x) => (x < -1 ? -1 : x > 1 ? 1 : x);

  /** Pointer position for the room parallax and the hover highlight, written once per frame. */
  function trackPointer() {
    let last = null;
    let queued = false;
    const apply = () => {
      queued = false;
      if (!last) return;
      root.style.setProperty("--px", fixed3(clampUnit((last.x / innerWidth) * 2 - 1)));
      root.style.setProperty("--py", fixed3(clampUnit((last.y / innerHeight) * 2 - 1)));
      const surface = last.target instanceof Element ? last.target.closest(GAZE_SURFACES) : null;
      if (!surface) return;
      const box = surface.getBoundingClientRect();
      surface.style.setProperty("--mx", `${Math.round(last.x - box.left)}px`);
      surface.style.setProperty("--my", `${Math.round(last.y - box.top)}px`);
      surface.style.setProperty("--tx", fixed3(clampUnit(((last.x - box.left) / box.width) * 2 - 1)));
      surface.style.setProperty("--ty", fixed3(clampUnit(((last.y - box.top) / box.height) * 2 - 1)));
    };
    addEventListener("pointermove", (e) => {
      last = { x: e.clientX, y: e.clientY, target: e.target };
      if (!queued) { queued = true; requestAnimationFrame(apply); }
    }, { passive: true });
    // When the pointer leaves the window, everything settles back to facing you.
    document.addEventListener("pointerleave", () => {
      last = null;
      root.style.setProperty("--px", "0");
      root.style.setProperty("--py", "0");
    });
  }

  /** Fare cards turn to face the centre of the screen, like a wall curving round you. */
  function curveFareWall() {
    const section = document.getElementById("fares");
    if (!section) return;
    const cards = Array.from(section.querySelectorAll(".rate"));
    if (!cards.length) return;
    let visible = false;
    let queued = false;
    const update = () => {
      queued = false;
      const mid = innerWidth / 2;
      const offsets = cards.map((card) => {
        const box = card.getBoundingClientRect();
        return clampUnit((box.left + box.width / 2 - mid) / mid);
      });
      cards.forEach((card, i) => {
        card.style.setProperty("--d", fixed3(offsets[i]));
        card.style.setProperty("--ad", fixed3(Math.abs(offsets[i])));
      });
    };
    const schedule = () => {
      if (visible && !queued) { queued = true; requestAnimationFrame(update); }
    };
    new IntersectionObserver(([entry]) => { visible = entry.isIntersecting; schedule(); }).observe(section);
    addEventListener("scroll", schedule, { passive: true });
    addEventListener("resize", schedule);
  }

  /** Windows and the bento rise out of the room the first time they scroll into view. */
  function revealOnArrival() {
    const targets = Array.from(document.querySelectorAll(REVEAL_TARGETS));
    const bento = document.getElementById("bento");
    if (bento) Array.from(bento.children).forEach((tile, i) => tile.style.setProperty("--i", String(i)));
    const io = new IntersectionObserver((entries) => entries.forEach((entry) => {
      if (!entry.isIntersecting) return;
      entry.target.classList.add("is-in");
      io.unobserve(entry.target);
    }), { rootMargin: "0px 0px -15% 0px" });
    targets.forEach((el) => io.observe(el));
  }

  if (reduce) return;
  root.classList.add("has-spatial");
  if (finePointer) trackPointer();
  curveFareWall();
  revealOnArrival();
})();
