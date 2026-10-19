// UI state and key data: config entries, cached key images/icons, composition.

import { api } from "./api.js";
import { composeClock, composeKey } from "./compose.js";
import { ROOT, clockColors, keyOf, ruleFor, viewOf, withKey } from "./model.js";
import { BUILTINS, builtinValues, fill, isDynamic } from "./placeholders.js";
import { toCssColor } from "./rgb565.js";

export const state = {
  info: null,
  config: null,
  mode: "edit",
  page: ROOT, // page shown in the UI and on the device
  selected: 0,
  connected: false,
  offline: false, // event stream to the host is down
  brightness: { level: 255, percent: 100, display_on: true },
  values: {}, // placeholder values from the host
  // "page/index" -> { image, icon (raw|null|undefined), iconDirty, mediaName, dynamic }
  keys: new Map(),
  loaded: new Set(), // pages whose key images were fetched
};

export const windowOf = (i) => state.info.keys.find((k) => k.index === i);
export const entryOf = (page, i) => keyOf(state.config, page, i); // as stored
export const lookOf = (page, i) => viewOf(state.config, page, i); // colours resolved
export const setEntry = (page, i, entry) => (state.config = withKey(state.config, page, entry));

export function slot(page, i) {
  const id = `${page}/${i}`;
  if (!state.keys.has(id)) state.keys.set(id, { image: null, icon: undefined, iconDirty: false });
  return state.keys.get(id);
}

export function forgetPages(ids) {
  for (const id of ids) {
    for (const k of [...state.keys.keys()]) if (k.startsWith(`${id}/`)) state.keys.delete(k);
    state.loaded.delete(id);
  }
}

const isInfoWindow = (w) => w.width > w.height * 1.5;

// The image the device shows: null for a plain fill. Dynamic keys (placeholders, the
// empty info window's clock) are rendered by the host; here only as a preview with the
// current values, and `slot.dynamic` keeps autosave from uploading them.
export function compose(page, i, { rules = false } = {}) {
  const base = lookOf(page, i);
  // `rules`: preview with the colours of the matching colour rule; the stored image never has them
  // (the host draws a matching rule itself and falls back to the stored image).
  const rule = rules ? ruleFor(base, state.values) : null;
  const e = rule ? { ...base, color: rule.color || base.color, label_color: rule.label_color || base.label_color } : base;
  const s = slot(page, i);
  const w = windowOf(i);
  const label = (e.label || "").trim();
  const now = builtinValues(new Date());
  s.dynamic = isDynamic(label) || (isInfoWindow(w) && !s.icon && !label);
  if (!s.icon && !label) {
    if (!s.dynamic && !rule) return null;
    if (!s.dynamic) return composeKey({ w: w.width, h: w.height, color: toCssColor(e.color), label: "", labelColor: toCssColor(e.label_color), labelPos: "bottom", labelSize: 100 });
    const c = clockColors(state.config, page, i);
    const colors = { color: toCssColor(e.color), timeColor: toCssColor(c.time), dateColor: toCssColor(c.date) };
    return composeClock({ w: w.width, h: w.height, ...colors, ...now });
  }
  const known = new Set([...BUILTINS, ...(state.config.variables || []).map((v) => v.name)]);
  return composeKey({
    w: w.width, h: w.height, color: toCssColor(e.color), icon: s.icon,
    label: s.dynamic ? fill(e.label, { ...state.values, ...now }, known) : e.label,
    labelColor: toCssColor(e.label_color), labelPos: e.label_pos || "bottom", labelSize: e.label_size || 100,
  });
}

// Load the stored icon once. Only icons the user set count: the key image is
// generated (background + icon + text) and must never be reused as an icon.
export function ensureIcon(page, i) {
  const s = slot(page, i);
  s.iconLoad ??= api.icon(page, i).then((icon) => {
    if (s.icon === undefined) s.icon = icon;
  });
  return s.iconLoad;
}

// Fetch the device images of a page's keys once.
export async function ensurePageImages(page) {
  if (state.loaded.has(page)) return;
  state.loaded.add(page);
  await Promise.all(
    state.info.keys.map(async (k) => {
      const s = slot(page, k.index);
      if (entryOf(page, k.index).image && !s.image) s.image = await api.image(page, k.index);
    }),
  );
}

// Preview of a key with colour rules: the image with the matching rule's colours, or null for the stored image.
export function refreshPreview(page, i) {
  const s = slot(page, i);
  s.preview = ruleFor(lookOf(page, i), state.values) ? compose(page, i, { rules: true }) : null;
}

// Recompose the dynamic keys of `page` (placeholders, the empty info window's clock)
// as a preview with the current values; `updated(index)` is called for each.
export async function refreshDynamicKeys(page, updated) {
  for (const w of state.info.keys) {
    const e = entryOf(page, w.index);
    const emptyInfo = isInfoWindow(w) && !e.image && !(e.label || "").trim();
    const dynamic = isDynamic(e.label) || emptyInfo;
    if (!dynamic && !e.rules?.length) continue;
    await ensureIcon(page, w.index);
    if (dynamic) slot(page, w.index).image = compose(page, w.index);
    refreshPreview(page, w.index);
    updated(w.index);
  }
}
