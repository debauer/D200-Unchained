// Right-hand key editor. Renders one key and reports edits through callbacks;
// it holds no state of its own beyond what is on screen.

import { ACTIONS, keyTitle } from "./deck.js";
import { joinCommand, splitCommand, toConfigColor, toCssColor } from "./rgb565.js";

const SWATCHES = ["0a0d12", "00f0ff", "ff2bd6", "7cff4f", "ffb800", "6a2bff", "ff3b5c", "ffffff"];
const TEXT_COLORS = [["Light", "ffffff"], ["Dark", "05070b"], ["Laser", "00f0ff"]];
const POSITIONS = [["Top", "top"], ["Middle", "center"], ["Bottom", "bottom"]];
const NO_MEDIA = "PNG \u00b7 JPG \u00b7 GIF \u2014 or drop here";

const $ = (id) => document.getElementById(id);

function segButton(label, on, onClick) {
  const b = document.createElement("button");
  b.type = "button";
  b.textContent = label;
  b.classList.toggle("on", on);
  b.addEventListener("click", onClick);
  return b;
}

export class Editor {
  // callbacks: { edit(patch), media(file), clearMedia(), reset(), newTarget(), openTarget(id) }
  constructor(callbacks) {
    this.cb = callbacks;
    this.key = null;
    $("target").addEventListener("change", (e) => this.cb.edit({ action: { type: "page", target: e.target.value } }));
    $("new-target").addEventListener("click", () => this.cb.newTarget());
    $("open-target").addEventListener("click", () => this.cb.openTarget(this.key.action.target));
    $("bg").addEventListener("input", (e) => this.cb.edit({ color: toConfigColor(e.target.value) }));
    $("bg-default").addEventListener("click", () => this.cb.edit({ color: null }));
    $("text-color-default").addEventListener("click", () => this.cb.edit({ label_color: null }));
    $("key-text").addEventListener("input", (e) => this.cb.edit({ label: e.target.value }));
    $("text-color-custom").addEventListener("input", (e) => this.cb.edit({ label_color: toConfigColor(e.target.value) }));
    $("text-size").addEventListener("input", (e) => this.cb.edit({ label_size: Number(e.target.value) }));
    $("command").addEventListener("input", () => this.onCommand());
    $("media-file").addEventListener("change", (e) => {
      if (e.target.files[0]) this.cb.media(e.target.files[0]);
      e.target.value = "";
    });
    $("clear-media").addEventListener("click", () => this.cb.clearMedia());
    $("reset").addEventListener("click", () => this.cb.reset());
    const drop = $("drop");
    drop.addEventListener("dragover", (e) => {
      e.preventDefault();
      drop.classList.add("over");
    });
    drop.addEventListener("dragleave", () => drop.classList.remove("over"));
    drop.addEventListener("drop", (e) => {
      e.preventDefault();
      drop.classList.remove("over");
      const file = e.dataTransfer.files[0];
      if (file && file.type.startsWith("image/")) this.cb.media(file);
    });
  }

  // key: config entry; window: key window from /api/info; actions: types the host runs;
  // media: { name, hasMedia }; preview: canvas with the composed key image;
  // own: { color, label_color } true when the key sets that colour itself; nav: { pageName, parentName (null on the top page), targets: [{ id, name }] (subpages) }.
  show({ key, window, actions, media, preview, nav, own }) {
    const switched = this.key?.index !== key.index;
    this.key = key;
    $("sel-title").textContent = keyTitle(window);
    const color = key.color || "0a0d12";
    $("bg").value = toCssColor(color);
    $("bg-hex").textContent = `#${color}`;
    $("bg-default").hidden = !own.color;
    $("text-color-default").hidden = !own.label_color;
    this.renderSwatches(color);
    if (switched || document.activeElement !== $("key-text")) $("key-text").value = key.label || "";
    this.renderSegments(key);
    this.renderActions(key, actions);
    this.renderNav(key, nav);
    this.renderMedia(media, preview);
  }

  renderNav(key, nav) {
    const type = key.action?.type;
    $("page-box").hidden = type !== "page";
    $("back-info").hidden = type !== "back";
    if (type === "page") {
      $("target-parent").textContent = nav.pageName;
      const none = new Option("\u2014 select \u2014", "");
      $("target").replaceChildren(none, ...nav.targets.map((t) => new Option(t.name || t.id, t.id)));
      $("target").value = nav.targets.some((t) => t.id === key.action.target) ? key.action.target : "";
      $("open-target").hidden = !$("target").value;
    }
    if (type === "back") {
      $("back-info").textContent = nav.parentName ? `> jumps to "${nav.parentName}"` : "> top page: back does nothing here";
    }
  }

  renderSwatches(color) {
    const box = $("swatches");
    box.querySelectorAll(".swatch").forEach((el) => el.remove());
    const picker = $("bg");
    for (const c of SWATCHES) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "swatch";
      b.style.background = toCssColor(c);
      b.setAttribute("aria-label", `#${c}`);
      b.classList.toggle("on", c === color);
      if (c === color) b.style.boxShadow = `0 0 12px ${toCssColor(c)}`;
      b.addEventListener("click", () => this.cb.edit({ color: c }));
      box.insertBefore(b, picker);
    }
  }

  renderSegments(key) {
    const textColor = key.label_color || "ffffff";
    const pos = key.label_pos || "bottom";
    const size = key.label_size || 100;
    $("text-color").replaceChildren(
      ...TEXT_COLORS.map(([label, c]) => segButton(label, c === textColor, () => this.cb.edit({ label_color: c }))),
    );
    if (document.activeElement !== $("text-color-custom")) $("text-color-custom").value = toCssColor(textColor);
    if (document.activeElement !== $("text-size")) $("text-size").value = size;
    $("text-size-value").textContent = `${size}%`;
    $("text-pos").replaceChildren(
      ...POSITIONS.map(([label, p]) => segButton(label, p === pos, () => this.cb.edit({ label_pos: p }))),
    );
  }

  renderActions(key, available) {
    const type = key.action?.type || "none";
    const offered = ACTIONS.filter(([t]) => t === "none" || available.includes(t));
    $("actions").replaceChildren(
      ...offered.map(([t, label, badge]) => {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "action";
        b.classList.toggle("on", t === type);
        const tag = document.createElement("span");
        tag.className = "b";
        tag.textContent = badge;
        b.append(tag, label);
        b.addEventListener("click", () => this.pickAction(t));
        return b;
      }),
    );
    const unsupported = type !== "none" && !offered.some(([t]) => t === type);
    $("action-unsupported").hidden = !unsupported;
    $("action-unsupported").textContent = unsupported
      ? `Action "${type}" is not supported by this host; it is kept in the config and ignored here.`
      : "";
    $("command-box").hidden = type !== "command";
    if (type === "command" && document.activeElement !== $("command")) {
      $("command").value = joinCommand(key.action.argv || []);
      $("command").classList.remove("invalid");
    }
  }

  renderMedia(media, preview) {
    const thumb = $("thumb");
    thumb.width = preview.width;
    thumb.height = preview.height;
    thumb.getContext("2d").drawImage(preview, 0, 0);
    $("clear-media").hidden = !media.hasMedia;
    $("media-info").textContent = media.name || (media.hasMedia ? "image" : NO_MEDIA);
  }

  pickAction(type) {
    if (type === "none") this.cb.edit({ action: null });
    else if (type === "command") this.cb.edit({ action: { type, argv: this.key.action?.argv || [] } });
    else if (type === "page") this.cb.edit({ action: { type, target: this.key.action?.target || "" } });
    else this.cb.edit({ action: { type } });
  }

  onCommand() {
    const box = $("command");
    try {
      const argv = splitCommand(box.value);
      box.classList.remove("invalid");
      $("command-note").textContent = "Runs without a shell: quotes group arguments, pipes and && are passed on literally.";
      this.cb.edit({ action: { type: "command", argv } });
    } catch (e) {
      box.classList.add("invalid");
      $("command-note").textContent = `Not saved: ${e.message}`;
    }
  }
}
