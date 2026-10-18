// Pure pixel helpers (no DOM), shared by the UI and the node tests.

// RGBA bytes (canvas ImageData.data) -> little-endian RGB565.
export function rgbaToRgb565(rgba) {
  const out = new Uint8Array((rgba.length / 4) * 2);
  for (let i = 0, j = 0; i < rgba.length; i += 4, j += 2) {
    const v = ((rgba[i] >> 3) << 11) | ((rgba[i + 1] >> 2) << 5) | (rgba[i + 2] >> 3);
    out[j] = v & 0xff;
    out[j + 1] = v >> 8;
  }
  return out;
}

// Little-endian RGB565 -> RGBA bytes (opaque), same expansion as deckd.
export function rgb565ToRgba(bytes) {
  const out = new Uint8ClampedArray((bytes.length / 2) * 4);
  for (let i = 0, j = 0; i < bytes.length; i += 2, j += 4) {
    const v = bytes[i] | (bytes[i + 1] << 8);
    const r = (v >> 11) & 0x1f;
    const g = (v >> 5) & 0x3f;
    const b = v & 0x1f;
    out[j] = (r << 3) | (r >> 2);
    out[j + 1] = (g << 2) | (g >> 4);
    out[j + 2] = (b << 3) | (b >> 2);
    out[j + 3] = 255;
  }
  return out;
}

// Where to draw a sw x sh source inside a dw x dh box.
// contain: whole source visible; cover: box filled, overflow cropped; stretch: exact box.
export function fitRect(sw, sh, dw, dh, mode) {
  if (mode === "stretch") return { x: 0, y: 0, w: dw, h: dh };
  const scale = mode === "cover" ? Math.max(dw / sw, dh / sh) : Math.min(dw / sw, dh / sh);
  const w = Math.round(sw * scale);
  const h = Math.round(sh * scale);
  return { x: Math.round((dw - w) / 2), y: Math.round((dh - h) / 2), w, h };
}

// "#rrggbb" <-> "rrggbb" (config format).
export function toConfigColor(cssColor) {
  return cssColor.replace(/^#/, "").toLowerCase();
}

export function toCssColor(configColor) {
  return `#${configColor}`;
}

// One command line -> argument list, like a shell would split it but without any
// shell features: whitespace separates, '...' is literal, "..." allows \" and \\.
// Throws on an unterminated quote.
export function splitCommand(line) {
  const args = [];
  let cur = "";
  let inArg = false;
  for (let i = 0; i < line.length; i += 1) {
    const ch = line[i];
    if (ch === "'" || ch === '"') {
      const end = ch === "'" ? line.indexOf("'", i + 1) : findDoubleQuoteEnd(line, i + 1);
      if (end < 0) throw new Error(`unterminated ${ch} quote`);
      const body = line.slice(i + 1, end);
      cur += ch === "'" ? body : body.replace(/\\(["\\])/g, "$1");
      inArg = true;
      i = end;
    } else if (/\s/.test(ch)) {
      if (inArg) args.push(cur);
      cur = "";
      inArg = false;
    } else {
      cur += ch;
      inArg = true;
    }
  }
  if (inArg) args.push(cur);
  return args;
}

function findDoubleQuoteEnd(line, from) {
  for (let i = from; i < line.length; i += 1) {
    if (line[i] === "\\") i += 1;
    else if (line[i] === '"') return i;
  }
  return -1;
}

// Argument list -> one line that splitCommand() turns back into the same list.
export function joinCommand(argv) {
  return argv.map((a) => (a !== "" && /^[\w@%+=:,./-]+$/.test(a) ? a : `'${a.replace(/'/g, `'"'"'`)}'`)).join(" ");
}
