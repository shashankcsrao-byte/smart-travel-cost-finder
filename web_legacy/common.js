/* Shared helpers used by both the demo (app.js) and live (live.js) pages. */
"use strict";

const $ = (selector) => document.querySelector(selector);

/** Escape text before putting it into HTML (prevents HTML injection). */
function esc(value) {
  return String(value).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function rupees(amount) {
  return Number.isInteger(amount) ? `₹${amount}` : `₹${amount.toFixed(2)}`;
}

function km(value) {
  return Number.isInteger(value) ? `${value} km` : `${value.toFixed(1)} km`;
}

function minutes(value) {
  const total = Math.round(value);
  const h = Math.floor(total / 60);
  const m = total % 60;
  if (h === 0) return `${m} min`;
  return m ? `${h} h ${m} min` : `${h} h`;
}

/** Call the backend and return JSON, turning failures into readable errors. */
async function api(path, options = {}) {
  let response;
  try {
    response = await fetch(path, options);
  } catch {
    throw new Error("Cannot reach the server. Is it running? Start it with: python -m src.server");
  }
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    const detail = Array.isArray(data.detail) ? data.detail.map((d) => d.msg).join("; ") : data.detail;
    throw new Error(detail || `Request failed (${response.status})`);
  }
  return data;
}
