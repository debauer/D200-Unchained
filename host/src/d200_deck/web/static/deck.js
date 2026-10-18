// Device view: key grid, status line, dimming, event log.

import { rawToCanvas } from "./compose.js";

// [type, label, badge]; "none" is always offered, the rest only if the host lists it.
export const ACTIONS = [
  ["none", "None", "\u2014"],
  ["command", "Command", "CMD"],
  ["page", "Page", "SITE"],
  ["back", "Back", "BACK"],
  ["bl_down", "Backlight \u2212", "BL\u2212"],
  ["bl_up", "Backlight +", "BL+"],
  ["disp_off", "Display off", "OFF"],
  ["disp_on", "Display on", "ON"],
];

// The wide key is the "info window" of the design.
export function isWide(window) {
  return window.width > window.height * 1.5;
}

export function keyTitle(window) {
  return isWide(window) ? "Info window" : `Key ${String(window.index + 1).padStart(2, "0")}`;
}

export function badgeOf(type) {
  const known = ACTIONS.find((a) => a[0] === type);
  return known ? known[2] : type.toUpperCase().slice(0, 4);
}

const LOG_LINES = 6;

export class DeckView {
  constructor({ keys, dim, led, statusLine, log, logEmpty, logLines, logBl }) {
    Object.assign(this, { keysEl: keys, dim, led, statusLine, log, logEmpty, logLines, logBl });
    this.buttons = new Map();
  }

  // Create one button per key window, positioned like on the panel.
  build(info, onClick, onDouble) {
    this.keysEl.replaceChildren();
    this.buttons.clear();
    const { width, height } = info.screen;
    this.keysEl.style.aspectRatio = `${width} / ${height}`;
    for (const k of info.keys) {
      const el = document.createElement("button");
      el.type = "button";
      el.className = "key";
      el.dataset.index = k.index;
      el.title = keyTitle(k);
      Object.assign(el.style, {
        left: `${(k.x / width) * 100}%`,
        top: `${(k.y / height) * 100}%`,
        width: `${(k.width / width) * 100}%`,
        height: `${(k.height / height) * 100}%`,
      });
      const canvas = document.createElement("canvas");
      canvas.width = k.width;
      canvas.height = k.height;
      const badge = document.createElement("span");
      badge.className = "badge";
      badge.hidden = true;
      el.append(canvas, badge);
      el.addEventListener("click", () => onClick(k.index));
      el.addEventListener("dblclick", () => onDouble(k.index));
      this.keysEl.append(el);
      this.buttons.set(k.index, { el, canvas, badge, window: k });
    }
  }

  // Show the device image of a key, or its plain fill colour when it has none.
  draw(index, raw, cssColor) {
    const b = this.buttons.get(index);
    if (!b) return;
    const ctx = b.canvas.getContext("2d");
    if (raw) {
      ctx.drawImage(rawToCanvas(raw, b.window.width, b.window.height), 0, 0);
    } else {
      ctx.fillStyle = cssColor;
      ctx.fillRect(0, 0, b.canvas.width, b.canvas.height);
    }
  }

  setBadge(index, text) {
    const b = this.buttons.get(index);
    if (!b) return;
    b.badge.textContent = text || "";
    b.badge.hidden = !text;
  }

  setSelected(index) {
    for (const [i, b] of this.buttons) b.el.classList.toggle("selected", i === index);
  }

  setPressed(index, pressed) {
    this.buttons.get(index)?.el.classList.toggle("pressed", pressed);
  }

  // Footer LED and text, dimming in test mode (as on the real panel).
  setStatus({ mode, connected, offline, brightness }) {
    const live = mode === "test";
    let text = "EDIT";
    let color = "var(--amber)";
    if (offline) {
      text = "HOST OFFLINE";
      color = "var(--red)";
    } else if (!connected) {
      text = "NO DEVICE";
      color = "var(--red)";
    } else if (live && !brightness.display_on) {
      text = "DISPLAY OFF";
      color = "var(--red)";
    } else if (live) {
      text = `LIVE \u00b7 BL ${brightness.percent}%`;
      color = "var(--green)";
    }
    this.statusLine.textContent = text;
    this.led.style.background = color;
    this.led.style.boxShadow = `0 0 8px ${color}`;
    const off = live && !brightness.display_on;
    this.dim.style.opacity = off ? 1 : live ? ((100 - brightness.percent) / 100) * 0.8 : 0;
    this.logBl.textContent = `BL ${brightness.display_on ? brightness.percent : 0}%`;
    this.log.hidden = !live;
  }

  addLog(message, warn = false) {
    const line = document.createElement("div");
    const t = document.createElement("span");
    t.className = "t";
    t.textContent = `[${new Date().toLocaleTimeString("en-GB")}]`;
    const m = document.createElement("span");
    m.className = warn ? "m warn" : "m";
    m.textContent = message;
    line.append(t, m);
    this.logLines.prepend(line);
    while (this.logLines.children.length > LOG_LINES) this.logLines.lastChild.remove();
    this.logEmpty.hidden = true;
  }
}
