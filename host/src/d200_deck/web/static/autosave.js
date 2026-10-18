// Debounced, serialised saving: config first, then icon and image of each edited key.

import { api } from "./api.js";

const SAVE_DELAY_MS = 500;

// What the host accepts: a command without arguments is still being typed.
// Variables without a command are left out the same way.
export function serializable(config) {
  const clean = (k) => {
    if (k.action?.type !== "command" || k.action.argv?.length) return k;
    const { action, ...rest } = k;
    return rest;
  };
  const variables = (config.variables || []).filter((v) => v.type !== "command" || v.command?.trim());
  return { ...config, pages: config.pages.map((p) => ({ ...p, keys: p.keys.map(clean) })), variables };
}

const keyId = (page, i) => `${page}/${i}`; // page ids cannot contain "/"

export class Autosave {
  // store: { config(), entryOf(page, i), setEntry(page, i, entry), slot(page, i) }; status(text, isError)
  constructor(store, status) {
    this.store = store;
    this.status = status;
    this.dirty = new Set(); // keyId of keys whose images must be saved
    this.configDirty = false;
    this.running = null;
    this.timer = null;
  }

  // Key `i` of `page` changed (config and images).
  touch(page, i) {
    this.dirty.add(keyId(page, i));
    this.touchConfig();
  }

  // Only the config changed (page names, pages added or removed).
  touchConfig() {
    this.configDirty = true;
    this.status("unsaved");
    clearTimeout(this.timer);
    // Errors are shown through status(); log them for the console as well.
    this.timer = setTimeout(() => this.flush().catch((e) => console.warn("autosave failed:", e)), SAVE_DELAY_MS);
  }

  // Save everything changed so far; resolves once it is stored (rejects on error).
  async flush() {
    clearTimeout(this.timer);
    // One save at a time; a failed earlier save was already reported by saveDirty().
    while (this.running) await this.running.catch((e) => console.debug("previous save failed:", e));
    if (!this.configDirty && !this.dirty.size) return;
    this.running = this.saveDirty();
    try {
      await this.running;
    } finally {
      this.running = null;
    }
  }

  async saveDirty() {
    const keys = [...this.dirty];
    this.dirty.clear();
    this.configDirty = false;
    this.status("saving\u2026");
    try {
      await api.putConfig(serializable(this.store.config()));
      for (const id of keys) {
        const [page, i] = id.split("/");
        await this.saveImages(page, Number(i));
      }
      this.status(this.dirty.size || this.configDirty ? "unsaved" : "saved");
    } catch (e) {
      keys.forEach((id) => this.dirty.add(id));
      this.configDirty = true;
      this.status(`not saved: ${e.message}`, true);
      throw e;
    }
  }

  async saveImages(page, i) {
    const { config, entryOf, setEntry, slot } = this.store;
    if (!config().pages.some((p) => p.id === page)) return; // page deleted meanwhile
    const s = slot(page, i);
    if (s.iconDirty) {
      if (s.icon) await api.putIcon(page, i, s.icon);
      else await api.deleteIcon(page, i);
      s.iconDirty = false;
    }
    if (s.image && !s.dynamic) {
      const stored = await api.putImage(page, i, s.image);
      setEntry(page, i, { ...entryOf(page, i), image: stored.image });
    } else if (entryOf(page, i).image) {
      // plain fill, or a dynamic key the host renders: no stored key image
      await api.deleteImage(page, i);
      const { image, ...rest } = entryOf(page, i);
      setEntry(page, i, rest);
    }
  }
}
