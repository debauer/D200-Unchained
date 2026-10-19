// Pure helpers on the config (version 2: pages of keys). No DOM; tested with node.

export const ROOT = "root";
// Colours are left out: a key without its own colour follows the page default.
export const BLANK = { label: "", label_pos: "bottom" };
const WHITE = "ffffff";
const CYAN = "00f0ff";
const MAGENTA = "ff2bd6";
// Every new subpage starts with a key that leads back, as in the design.
export const BACK_KEY = { index: 0, label: "< Back", color: "0a0d12", label_color: "00f0ff", label_pos: "center", action: { type: "back" } };
const PAGE_ID = /^[a-z0-9_-]{1,32}$/;

export function pageOf(cfg, id) {
  return cfg.pages.find((p) => p.id === id);
}

// The page's default colours (else the global background / white text).
export function defaultsOf(cfg, pageId) {
  const page = pageOf(cfg, pageId);
  return { color: page?.color || cfg.background, label_color: page?.label_color || WHITE, accent_color: page?.accent_color || MAGENTA };
}

// Clock colours: time = the key's / page's text colour (cyan if none), date = secondary colour.
export function clockColors(cfg, pageId, index) {
  const page = pageOf(cfg, pageId);
  return { time: keyOf(cfg, pageId, index).label_color || page?.label_color || CYAN, date: defaultsOf(cfg, pageId).accent_color };
}

// The key as stored; colours it does not set are missing (they follow the page default).
export function keyOf(cfg, pageId, index) {
  return pageOf(cfg, pageId)?.keys.find((k) => k.index === index) || { index, ...BLANK };
}

// The key with its colours resolved, for drawing.
export function viewOf(cfg, pageId, index) {
  const k = keyOf(cfg, pageId, index);
  const d = defaultsOf(cfg, pageId);
  return { ...k, color: k.color || d.color, label_color: k.label_color || d.label_color };
}

// The first colour rule of a key whose variable currently has its value (values: name -> text).
export function ruleFor(key, values) {
  return (key.rules || []).find((r) => r.variable in values && String(values[r.variable]).trim() === r.value.trim()) || null;
}

export function withPage(cfg, page) {
  const exists = cfg.pages.some((p) => p.id === page.id);
  return { ...cfg, pages: exists ? cfg.pages.map((p) => (p.id === page.id ? page : p)) : [...cfg.pages, page] };
}

export function withKey(cfg, pageId, entry) {
  const page = pageOf(cfg, pageId);
  const keys = [...page.keys.filter((k) => k.index !== entry.index), entry].sort((a, b) => a.index - b.index);
  return withPage(cfg, { ...page, keys });
}

export function children(cfg, id) {
  return cfg.pages.filter((p) => p.parent === id);
}

export function subtree(cfg, id) {
  const found = new Set([id]);
  let grew = true;
  while (grew) {
    grew = false;
    for (const p of cfg.pages) {
      if (!found.has(p.id) && found.has(p.parent)) {
        found.add(p.id);
        grew = true;
      }
    }
  }
  return found;
}

// Pages some page key leads to; the others are shown as FREE (unreachable).
export function linkedPages(cfg) {
  const linked = new Set();
  for (const p of cfg.pages) {
    for (const k of p.keys) if (k.action?.type === "page" && k.action.target) linked.add(k.action.target);
  }
  return linked;
}

// Depth-first rows for the page tree: [{ id, name, depth, orphan }].
export function treeRows(cfg) {
  const linked = linkedPages(cfg);
  const rows = [];
  const walk = (id, depth) => {
    const p = pageOf(cfg, id);
    rows.push({ id, name: p.name, depth, orphan: id !== ROOT && !linked.has(id) });
    for (const c of children(cfg, id)) walk(c.id, depth + 1);
  };
  walk(ROOT, 0);
  return rows;
}

// Pages from the root down to `id`.
export function crumbs(cfg, id) {
  const path = [];
  for (let p = pageOf(cfg, id); p; p = p.parent ? pageOf(cfg, p.parent) : null) path.unshift(p);
  return path;
}

// A free page id derived from `name` ("Studio Lights" -> "studio-lights", "studio-lights-2", ...).
export function newPageId(cfg, name) {
  const ascii = name.normalize("NFKD").replace(/[\u0300-\u036f]/g, "").toLowerCase(); // accents dropped: an umlaut becomes its base letter
  const base = ascii.replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 24) || "page";
  for (let n = 1; ; n += 1) {
    const id = n === 1 ? base : `${base}-${n}`;
    if (PAGE_ID.test(id) && !pageOf(cfg, id)) return id;
  }
}

export function addChild(cfg, parentId, name) {
  const id = newPageId(cfg, name);
  return { cfg: withPage(cfg, { id, name, parent: parentId, keys: [{ ...BACK_KEY }] }), id };
}

// Without `id` and its subpages; page keys that led into them lose their target.
export function withoutSubtree(cfg, id) {
  const gone = subtree(cfg, id);
  const unlink = (k) => (k.action?.type === "page" && gone.has(k.action.target) ? { ...k, action: { ...k.action, target: "" } } : k);
  return { ...cfg, pages: cfg.pages.filter((p) => !gone.has(p.id)).map((p) => ({ ...p, keys: p.keys.map(unlink) })) };
}

// Whether page `id` (with its subpages) may become a child of `parentId`.
export function canMove(cfg, id, parentId) {
  const page = pageOf(cfg, id);
  return Boolean(page) && id !== ROOT && Boolean(pageOf(cfg, parentId)) && page.parent !== parentId && !subtree(cfg, id).has(parentId);
}

// Move page `id` with its subpages under `parentId`. Page keys of the old parent that
// led to it lose their target; the page is FREE under the new parent until linked.
export function movePage(cfg, id, parentId) {
  const old = pageOf(cfg, pageOf(cfg, id).parent);
  const unlink = (k) => (k.action?.type === "page" && k.action.target === id ? { ...k, action: { ...k.action, target: "" } } : k);
  const moved = withPage(cfg, { ...pageOf(cfg, id), parent: parentId });
  return withPage(moved, { ...old, keys: old.keys.map(unlink) });
}

// Rename a variable, its placeholders in every key text and its colour rules.
export function renameVariable(cfg, from, to) {
  const rules = (k) => (k.rules ? { ...k, rules: k.rules.map((r) => (r.variable === from ? { ...r, variable: to } : r)) } : k);
  const swap = (k) => rules(k.label ? { ...k, label: k.label.split(`{${from}}`).join(`{${to}}`) } : k);
  return {
    ...cfg,
    variables: (cfg.variables || []).map((v) => (v.name === from ? { ...v, name: to } : v)),
    pages: cfg.pages.map((p) => ({ ...p, keys: p.keys.map(swap) })),
  };
}
