// D200 // Configurator: state, pages, autosave, edit/test modes, live events.

import { api } from "./api.js";
import { AuthFlow, LoginView } from "./auth.js";
import { AltMedia } from "./altmedia.js";
import { Autosave } from "./autosave.js";
import { fontsReady, iconFromFile, rawToCanvas } from "./compose.js";
import { DeckView, badgeOf } from "./deck.js";
import { Editor } from "./editor.js";
import { bindIO } from "./io.js";
import { BLANK, ROOT, addChild, defaultsOf, children, movePage, pageOf, subtree, withPage, withoutSubtree } from "./model.js";
import { HostLog } from "./hostlog.js";
import { MqttView } from "./mqtt.js";
import { PagesView } from "./pages.js";
import { StorageView } from "./storage.js";
import { toCssColor } from "./rgb565.js";
import {
  compose, ensureIcon, ensurePageImages, entryOf, forgetPages, lookOf, refreshDynamicKeys, refreshPreview, setEntry, slot, state,
  windowOf,
} from "./store.js";
import { toast } from "./ui.js";
import { VariablesView } from "./variables.js";

const $ = (id) => document.getElementById(id);

const hostLog = new HostLog();
const storage = new StorageView();

const deck = new DeckView({
  keys: $("keys"), dim: $("dim"), led: $("led"), statusLine: $("status-line"),
  log: $("log"), logEmpty: $("log-empty"), logLines: $("log-lines"), logBl: $("log-bl"),
});

const fail = (e) => toast(e.message, true);

function drawKey(i) {
  const e = lookOf(state.page, i);
  const s = slot(state.page, i);
  deck.draw(i, s.preview ?? s.image, toCssColor(e.color));
  deck.setBadge(i, state.mode === "edit" && e.action ? badgeOf(e.action.type) : null);
}

function solid(w, color) {
  const c = Object.assign(document.createElement("canvas"), { width: w.width, height: w.height });
  const ctx = c.getContext("2d");
  ctx.fillStyle = toCssColor(color);
  ctx.fillRect(0, 0, w.width, w.height);
  return c;
}

const altMedia = new AltMedia(changed, fail);

const ownColors = (e) => ({ color: Boolean(e.color), label_color: Boolean(e.label_color) });

function showEditor() {
  const { page, selected: i } = state;
  const w = windowOf(i);
  const s = slot(page, i);
  const current = pageOf(state.config, page);
  const parent = current.parent ? pageOf(state.config, current.parent) : null;
  const k = lookOf(page, i);
  altMedia.show({ rules: k.rules || [], alt: s.alt, name: s.altName, w: w.width, h: w.height });
  editor.show({
    variables: (state.config.variables || []).map((v) => v.name),
    key: lookOf(page, i), own: ownColors(entryOf(page, i)), window: w, actions: state.info.actions,
    media: { hasMedia: Boolean(s.icon), name: s.mediaName },
    preview: s.image ? rawToCanvas(s.image, w.width, w.height) : solid(w, lookOf(page, i).color),
        nav: { pageName: current.name, parentName: parent?.name ?? null, targets: children(state.config, page) },
  });
}

function refreshStatus() {
  if (document.activeElement !== $("brightness")) $("brightness").value = state.brightness.level; // follows the device
  deck.setStatus({ mode: state.mode, connected: state.connected, offline: state.offline, brightness: state.brightness });
}

// --- editing and autosave --------------------------------------------------

const autosave = new Autosave({ config: () => state.config, entryOf, setEntry, slot }, (text, isError = false) => {
  $("save-state").textContent = text;
  $("save-state").classList.toggle("error", isError);
});

// Recompose only once the icon is known, else an early edit would drop it.
async function changed(page, i) {
  await ensureIcon(page, i);
  slot(page, i).image = compose(page, i);
  refreshPreview(page, i);
  if (page === state.page) {
    drawKey(i);
    if (i === state.selected) showEditor();
  }
  autosave.touch(page, i);
}

function edit(patch) {
  const { page, selected: i } = state;
  const e = { ...entryOf(page, i), ...patch };
  for (const name of Object.keys(patch)) if (patch[name] === null) delete e[name]; // null: back to the default
  setEntry(page, i, e);
  changed(page, i).catch(fail);
  pages.render(state.config, state.page); // FREE marks follow page targets
}

// A new subpage under the shown page (with its back key, which needs an image).
function createSubpage(name) {
  const created = addChild(state.config, state.page, name);
  state.config = created.cfg;
  autosave.touchConfig(); // now, so a following flush (e.g. before showing the page) stores it
  changed(created.id, 0).catch(fail);
  return created.id;
}

const editor = new Editor({
  edit,
  async media(file) {
    const { page, selected: i } = state;
    const w = windowOf(i);
    try {
      const icon = await iconFromFile(file, w.width, w.height, toCssColor(lookOf(page, i).color));
      if (state.selected !== i || state.page !== page) return; // another key was opened while decoding
      Object.assign(slot(page, i), { icon, iconDirty: true, mediaName: file.name });
      await changed(page, i);
    } catch (e) {
      toast(`Cannot read ${file.name}: ${e.message}`, true);
    }
  },
  clearMedia() {
    Object.assign(slot(state.page, state.selected), { icon: null, iconDirty: true, mediaName: "" });
    changed(state.page, state.selected).catch(fail);
  },
  reset() {
    const { page, selected: i } = state;
    setEntry(page, i, { index: i, ...BLANK });
    Object.assign(slot(page, i), { icon: null, iconDirty: true, mediaName: "", alt: null, altDirty: true, altName: "" });
    changed(page, i).catch(fail);
  },
  newTarget() {
    const label = entryOf(state.page, state.selected).label.trim();
    const id = createSubpage(label || `Subpage ${state.config.pages.length}`);
    edit({ action: { type: "page", target: id } });
  },
  openTarget: (id) => openPage(id),
});

// Preview of the keys the host renders itself (placeholders, the clock); nothing is saved.
function refreshDynamic(page) {
  return refreshDynamicKeys(page, (i) => {
    if (page !== state.page) return;
    drawKey(i);
    if (i === state.selected) showEditor();
  });
}

const variables = new VariablesView({
  config: () => state.config,
  setConfig: (cfg) => (state.config = cfg),
  changed() {
    autosave.touchConfig();
    variables.render();
    refreshDynamic(state.page).catch(fail);
  },
  values: () => state.values,
  types: () => state.info?.variable_types || ["command"],
});

const mqtt = new MqttView({ toast });

// --- pages -------------------------------------------------------------------

// Show `page` here and (unless the device already switched) on the device.
async function openPage(page, tellDevice = true) {
  if (!pageOf(state.config, page)) return;
  state.page = page;
  pages.render(state.config, page);
  state.info.keys.forEach((k) => drawKey(k.index)); // at once with this page's colours; images and values follow
  // Save first: a page created a moment ago is unknown to the host until then.
  if (tellDevice) autosave.flush().then(() => api.showPage(page)).catch(fail);
  await ensurePageImages(page);
  await refreshDynamic(page);
  if (state.page !== page) return;
  state.info.keys.forEach((k) => drawKey(k.index));
  await select(state.selected);
}

const pages = new PagesView({
  open: (id) => openPage(id),
  rename(name) {
    state.config = withPage(state.config, { ...pageOf(state.config, state.page), name });
    pages.render(state.config, state.page);
    showEditor();
    autosave.touchConfig();
  },
  up() {
    const parent = pageOf(state.config, state.page).parent;
    if (parent) openPage(parent);
  },
  remove() {
    const page = pageOf(state.config, state.page);
    const below = subtree(state.config, page.id).size - 1;
    const extra = below ? ` and its ${below} subpage${below > 1 ? "s" : ""}` : "";
    if (!window.confirm(`Delete page "${page.name}"${extra}? Its keys and images are removed.`)) return;
    forgetPages(subtree(state.config, page.id));
    state.config = withoutSubtree(state.config, page.id);
    autosave.touchConfig();
    openPage(page.parent);
  },
  addChild: () => openPage(createSubpage(`Subpage ${state.config.pages.length}`)),
  move(id, parentId) {
    const page = pageOf(state.config, id);
    const links = pageOf(state.config, page.parent).keys.filter((k) => k.action?.type === "page" && k.action.target === id).length;
    state.config = movePage(state.config, id, parentId);
    pages.render(state.config, state.page);
    state.info.keys.forEach((k) => drawKey(k.index)); // PAGE badges of cleared links
    showEditor();
    autosave.touchConfig();
    toast(`"${page.name}" moved below "${pageOf(state.config, parentId).name}"${links ? `, ${links} link${links > 1 ? "s" : ""} removed` : ""}`);
  },
  colors(patch) {
    const page = { ...pageOf(state.config, state.page), ...patch };
    for (const name of Object.keys(patch)) if (patch[name] === null) delete page[name];
    state.config = withPage(state.config, page);
    pages.render(state.config, state.page);
    for (const k of state.info.keys) {
      const own = page.keys.find((e) => e.index === k.index);
      if (own && !(own.color && own.label_color)) changed(state.page, k.index).catch(fail); // image uses the colours
      else if (!own) drawKey(k.index);
    }
    refreshDynamic(state.page).catch(fail); // the clock follows the colours
    showEditor();
    autosave.touchConfig();
  },
});

// --- selection, modes, loading, events ------------------------------------------

async function select(i) {
  const page = state.page;
  state.selected = i;
  deck.setSelected(state.mode === "edit" ? i : -1);
  showEditor();
  await ensureIcon(page, i);
  if (state.selected === i && state.page === page) showEditor(); // now with the media info
}

async function onKeyClick(i) {
  if (state.mode === "edit") return select(i);
  try {
    const { started } = await api.press(state.page, i);
    if (!started) deck.addLog(`! key ${i + 1}: no action`, true);
  } catch (e) {
    deck.addLog(`! ${e.message}`, true);
  }
}

// Double-click on a page key (edit mode) opens its target, as in the design.
function onKeyDouble(i) {
  const a = entryOf(state.page, i).action;
  if (state.mode === "edit" && a?.type === "page" && pageOf(state.config, a.target)) openPage(a.target);
}

function setMode(mode) {
  state.mode = mode;
  document.querySelectorAll("#mode button").forEach((b) => b.classList.toggle("on", b.dataset.mode === mode));
  $("editor").hidden = mode !== "edit";
  deck.setSelected(mode === "edit" ? state.selected : -1);
  state.info?.keys.forEach((k) => drawKey(k.index));
  refreshStatus();
}

async function loadAll() {
  [state.info, state.config] = await Promise.all([api.info(), api.config()]);
  Object.assign(state, { connected: state.info.connected, brightness: state.info.brightness, values: state.info.values });
  variables.render();
  mqtt.load().catch(fail);
  hostLog.show(Boolean(state.info.log));
  storage.show(state.info.storage);
  refreshStatus();
  await fontsReady();
  state.keys.clear();
  state.loaded.clear();
  deck.build(state.info, onKeyClick, onKeyDouble);
  setMode(state.mode);
  state.selected = Math.min(state.selected, state.info.keys.length - 1);
  await openPage(pageOf(state.config, state.info.page) ? state.info.page : ROOT, false);
}

// Reload everything from the host, but store pending edits first so none are lost.
function reload() {
  return autosave
    .flush()
    .catch((e) => console.warn("pending edits not saved before reload:", e)) // also shown in the editor
    .then(loadAll)
    .catch(fail);
}

document.querySelectorAll("#mode button").forEach((b) => b.addEventListener("click", () => setMode(b.dataset.mode)));
setInterval(() => state.info?.keys.length && refreshDynamic(state.page).catch(fail), 15000); // {time}, clock
bindIO({ flush: () => autosave.flush(), config: () => state.config, setConfig: (c) => (state.config = c), reload, toast });

let stream = null;

function connect() {
  stream?.close();
  stream = api.events({
    key: ({ index, pressed }) => deck.setPressed(index, pressed),
    action: ({ detail }) => deck.addLog(detail, detail.startsWith("!")),
    page: ({ page }) => page !== state.page && openPage(page, false).catch(fail),
    values: (values) => {
      state.values = values;
      variables.render();
      refreshDynamic(state.page).catch(fail);
    },
    mqtt: (s) => mqtt.apply(s),
    brightness: (b) => {
      state.brightness = b;
      refreshStatus();
    },
    status: ({ connected }) => {
      const reconnected = connected && !state.connected;
      state.connected = connected;
      refreshStatus();
      if (reconnected) reload();
    },
    offline: () => {
      state.offline = true;
      refreshStatus();
      // A stream refused with 401 looks like any other break: ask whether the login ended.
      auth.verify().then((lost) => lost && relogin()).catch((e) => console.debug("session check failed:", e));
    },
    // After a break the host may have restarted: resync instead of trusting old state.
    online: () => {
      const wasOffline = state.offline;
      state.offline = false;
      refreshStatus();
      if (wasOffline) reload();
    },
  });
}

const auth = new AuthFlow({
  view: new LoginView(),
  autosave,
  stop: () => stream?.close(),
});

// The login ended: log in again; the same user keeps unsaved edits (stored by the reload), another user does not.
let relogging = null;

function relogin() {
  relogging ??= auth
    .recover()
    .then((same) => (same ? reload() : loadAll()))
    .then(connect)
    .finally(() => (relogging = null));
  return relogging;
}

auth.view.onLogout = () => auth.logout().then(relogin).catch(fail);

auth
  .init()
  .then(() => api.onAuthLost(() => relogin().catch(fail)))
  .then(loadAll)
  .then(connect)
  .catch((e) => toast(`Cannot reach the host: ${e.message}`, true));
