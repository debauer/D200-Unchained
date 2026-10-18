// Canvas work in the browser: fit icons to a key window, draw the key text, convert to RGB565.
// The same image is shown in the UI and sent to the device, so preview == device.

import { fitRect, rgb565ToRgba, rgbaToRgb565 } from "./rgb565.js";

const FAMILY = '"Chakra Petch", sans-serif';
const MAX_LINES = 3;
// Proportions of the design (13 px text on a ~109 px key).
const FONT_RATIO = 0.12;
const MIN_FONT_RATIO = 0.075;
const PAD_X_RATIO = 0.055;
const PAD_Y_RATIO = 0.07;
const LINE_HEIGHT = 1.15;

function canvas(w, h) {
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  return c;
}

export function rawToCanvas(raw, w, h) {
  const c = canvas(w, h);
  c.getContext("2d").putImageData(new ImageData(rgb565ToRgba(raw), w, h), 0, 0);
  return c;
}

function toRaw(c) {
  return rgbaToRgb565(c.getContext("2d").getImageData(0, 0, c.width, c.height).data);
}

// Resolve once the key font is usable; a label rendered before would bake the
// fallback font into the device image.
export function fontsReady() {
  return document.fonts.load(`700 24px ${FAMILY}`);
}

// Image file -> icon covering the whole w x h window (RGB565), background behind transparency.
export async function iconFromFile(file, w, h, background) {
  const bitmap = await createImageBitmap(file);
  const c = canvas(w, h);
  const ctx = c.getContext("2d");
  ctx.fillStyle = background;
  ctx.fillRect(0, 0, w, h);
  const r = fitRect(bitmap.width, bitmap.height, w, h, "cover");
  ctx.drawImage(bitmap, r.x, r.y, r.w, r.h);
  bitmap.close();
  return toRaw(c);
}

// Greedy word wrap; words wider than the line are broken anywhere (like overflow-wrap: anywhere).
function wrap(ctx, text, width) {
  const lines = [];
  for (const paragraph of text.split("\n")) {
    let line = "";
    for (const word of paragraph.split(/\s+/).filter(Boolean)) {
      const candidate = line ? `${line} ${word}` : word;
      if (ctx.measureText(candidate).width <= width) {
        line = candidate;
        continue;
      }
      if (line) lines.push(line);
      line = "";
      for (const ch of word) {
        if (line && ctx.measureText(line + ch).width > width) {
          lines.push(line);
          line = "";
        }
        line += ch;
      }
    }
    if (line) lines.push(line);
  }
  return lines;
}

function ellipsize(ctx, line, width) {
  let s = line;
  while (s && ctx.measureText(`${s}...`).width > width) s = s.slice(0, -1).trimEnd();
  return `${s}...`;
}

// Font size and lines for `text`: shrink until it fits MAX_LINES, then cut the last line.
// `scale`: chosen text size as a factor of the automatic one (like textrender.layout).
function layout(ctx, text, width, h, scale) {
  const start = Math.max(1, Math.round(h * FONT_RATIO * scale));
  const min = Math.min(start, Math.round(h * MIN_FONT_RATIO));
  for (let size = start; ; size -= 1) {
    ctx.font = `700 ${size}px ${FAMILY}`;
    const lines = wrap(ctx, text, width);
    if (lines.length <= MAX_LINES) return { size, lines };
    if (size <= min) {
      const kept = lines.slice(0, MAX_LINES);
      kept[MAX_LINES - 1] = ellipsize(ctx, kept[MAX_LINES - 1], width);
      return { size, lines: kept };
    }
  }
}

// Final key image: background colour, optional icon (whole key), optional text at
// top/center/bottom; text over an icon gets a shadow as in the design.
export function composeKey({ w, h, color, icon, label, labelColor, labelPos, labelSize = 100 }) {
  const c = canvas(w, h);
  const ctx = c.getContext("2d");
  ctx.fillStyle = color;
  ctx.fillRect(0, 0, w, h);
  if (icon) ctx.drawImage(rawToCanvas(icon, w, h), 0, 0);
  const text = (label || "").trim();
  if (text) {
    const padX = Math.round(h * PAD_X_RATIO);
    const padY = Math.round(h * PAD_Y_RATIO);
    const { size, lines } = layout(ctx, text, w - 2 * padX, h, labelSize / 100);
    const lineH = size * LINE_HEIGHT;
    const total = lineH * lines.length;
    const top = labelPos === "top" ? padY : labelPos === "center" ? (h - total) / 2 : h - padY - total;
    ctx.fillStyle = labelColor;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    if (icon) {
      ctx.shadowColor = "rgba(0, 0, 0, 0.8)";
      ctx.shadowBlur = Math.round(h * 0.03);
      ctx.shadowOffsetY = Math.max(1, Math.round(h * 0.01));
    }
    lines.forEach((line, i) => ctx.fillText(line, w / 2, top + lineH * (i + 0.5)));
  }
  return toRaw(c);
}

// Info-window clock, same layout as textrender.render_clock on the host (preview only:
// the host renders the real one every minute).
export function composeClock({ w, h, color, timeColor, dateColor, time, date }) {
  const c = canvas(w, h);
  const ctx = c.getContext("2d");
  ctx.fillStyle = color;
  ctx.fillRect(0, 0, w, h);
  const timeSize = Math.round(h * 0.45);
  const dateSize = Math.round(h * 0.13);
  const gap = h * 0.03;
  const timeY = (h - (timeSize + gap + dateSize)) / 2 + timeSize / 2;
  const dateY = timeY + timeSize / 2 + gap + dateSize / 2;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.font = `700 ${timeSize}px ${FAMILY}`;
  ctx.letterSpacing = `${timeSize * 0.08}px`;
  ctx.fillStyle = timeColor;
  ctx.shadowColor = timeColor;
  ctx.shadowBlur = h * 0.06;
  ctx.fillText(time, w / 2, timeY);
  ctx.shadowBlur = 0;
  ctx.font = `500 ${dateSize}px ${FAMILY}`;
  ctx.letterSpacing = `${dateSize * 0.2}px`;
  ctx.fillStyle = dateColor;
  ctx.fillText(date.toUpperCase(), w / 2, dateY);
  return toRaw(c);
}
