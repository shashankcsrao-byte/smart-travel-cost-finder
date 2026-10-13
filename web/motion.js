/*
 * motion.js · the page's own scroll engine (no third-party code).
 *
 * Markup it understands:
 *   <section data-act="flow|pin|pan" data-span="4">     a scroll section; span = height in screens
 *     <div data-stage>                                    pin/pan: stays fixed while the section scrolls
 *       <div data-pan="0.05">                             pan: moved sideways with scroll (+5% overshoot)
 *     <div data-reveal="up" data-reveal-at="0 0.45">      wipes in between those section progresses
 *   <div data-in data-stagger="60">                       fades children in once when scrolled into view
 *   <span data-progress>                                  page scroll progress bar
 *
 * Every section gets a CSS variable --p (0 to 1): how far the reader has scrolled
 * through it. Page code reads that to drive its own effects.
 *
 *   const motion = Motion.mount(document.body);
 *   motion.layout();     // call after content above a section changes height
 */
(function () {
  "use strict";

  const clamp01 = (x) => (x < 0 ? 0 : x > 1 ? 1 : x);

  function mount(root) {
    const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
    const acts = Array.from(root.querySelectorAll("[data-act]")).map((el) => {
      const span = parseFloat(el.dataset.span) || 0;
      if (span) el.style.setProperty("--span", String(span));
      return {
        el, span,
        kind: el.dataset.act,
        rail: el.querySelector("[data-pan]"),
        overshoot: parseFloat((el.querySelector("[data-pan]") || {}).dataset?.pan) || 0,
        reveals: Array.from(el.querySelectorAll("[data-reveal]")).map((r) => ({
          el: r,
          dir: r.dataset.reveal || "up",
          at: (r.dataset.revealAt || "0 1").split(/\s+/).map(Number),
        })),
        top: 0, height: 0, travel: 0, last: -1,
      };
    });
    const bar = root.querySelector("[data-progress]");
    let vh = innerHeight, docMax = 1, queued = false;

    // Measure once, then every frame only does arithmetic (no layout reads).
    function layout() {
      vh = innerHeight;
      const y = scrollY;
      acts.forEach((a) => {
        const r = a.el.getBoundingClientRect();
        a.top = r.top + y;
        a.height = a.el.offsetHeight;
        a.travel = a.rail ? Math.max(0, a.rail.scrollWidth - innerWidth) * (1 + a.overshoot) : 0;
        a.last = -1;
      });
      docMax = Math.max(1, document.documentElement.scrollHeight - vh);
      update();
    }

    function progressOf(a, y) {
      if (a.kind === "pin" || a.kind === "pan") {
        return clamp01((y - a.top) / Math.max(1, a.height - vh));      // 0 when it pins, 1 when it lets go
      }
      return clamp01((y + vh - a.top) / Math.max(1, a.height + vh));   // flow: entering to leaving
    }

    function clip(dir, t) {
      const hide = ((1 - t) * 100).toFixed(2) + "%";
      if (dir === "down") return `inset(0 0 ${hide} 0)`;
      if (dir === "left") return `inset(0 0 0 ${hide})`;
      if (dir === "right") return `inset(0 ${hide} 0 0)`;
      return `inset(${hide} 0 0 0)`;                                    // "up": rises from the bottom
    }

    function update() {
      queued = false;
      const y = scrollY;
      acts.forEach((a) => {
        const p = progressOf(a, y);
        if (p === a.last) return;
        a.last = p;
        a.el.style.setProperty("--p", p.toFixed(4));
        if (a.rail && !reduce) a.rail.style.transform = `translate3d(${(-p * a.travel).toFixed(1)}px,0,0)`;
        a.reveals.forEach((r) => {
          const [from, to] = r.at;
          const t = reduce ? 1 : clamp01((p - from) / Math.max(1e-6, to - from));
          r.el.style.clipPath = t >= 1 ? "none" : clip(r.dir, t);
        });
      });
      if (bar) bar.style.transform = `scaleX(${clamp01(y / docMax).toFixed(4)})`;
    }

    const onScroll = () => { if (!queued) { queued = true; requestAnimationFrame(update); } };
    let lastW = innerWidth;
    const onResize = () => {
      // Phones resize the viewport as the address bar hides; only width changes need a re-measure.
      if (innerWidth !== lastW || Math.abs(innerHeight - vh) > 120) { lastW = innerWidth; layout(); }
      else onScroll();
    };
    addEventListener("scroll", onScroll, { passive: true });
    addEventListener("resize", onResize);

    // Fade-in groups: fire once when half-visible, children staggered.
    const io = new IntersectionObserver((entries) => entries.forEach((en) => {
      if (!en.isIntersecting) return;
      const el = en.target;
      const step = parseInt(el.dataset.stagger, 10) || 0;
      Array.from(el.children).forEach((child, i) => { child.style.transitionDelay = `${i * step}ms`; });
      el.classList.add("is-in");
      io.unobserve(el);
    }), { rootMargin: "0px 0px -12% 0px" });
    root.querySelectorAll("[data-in]").forEach((el) => io.observe(el));

    if (document.fonts && document.fonts.ready) document.fonts.ready.then(layout);
    addEventListener("load", layout);
    layout();
    return {
      layout,
      destroy() { removeEventListener("scroll", onScroll); removeEventListener("resize", onResize); io.disconnect(); },
    };
  }

  window.Motion = { mount };
})();
