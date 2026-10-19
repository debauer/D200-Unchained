// Second image of a key: shown instead of the image while a colour rule matches.

import { iconFromFile, rawToCanvas } from "./compose.js";
import { toCssColor } from "./rgb565.js";
import { lookOf, slot, state, windowOf } from "./store.js";
import { toast } from "./ui.js";

const $ = (id) => document.getElementById(id);

export class AltMedia {
  // changed(page, i): recompose and save the key; fail(error): report
  constructor(changed, fail) {
    $("alt-file").addEventListener("change", (e) => {
      if (e.target.files[0]) this.choose(e.target.files[0], changed);
      e.target.value = "";
    });
    $("clear-alt").addEventListener("click", () => {
      const { page, selected: i } = state;
      Object.assign(slot(page, i), { alt: null, altDirty: true, altName: "" });
      changed(page, i).catch(fail);
    });
  }

  async choose(file, changed) {
    const { page, selected: i } = state;
    const w = windowOf(i);
    try {
      const alt = await iconFromFile(file, w.width, w.height, toCssColor(lookOf(page, i).color));
      if (state.selected !== i || state.page !== page) return; // another key was opened while decoding
      Object.assign(slot(page, i), { alt, altDirty: true, altName: file.name });
      await changed(page, i);
    } catch (e) {
      toast(`Cannot read ${file.name}: ${e.message}`, true);
    }
  }

  // Only keys with colour rules have a second image; `alt`: raw RGB565 or null.
  show({ rules, alt, name, w, h }) {
    $("alt-box").hidden = !rules.length;
    $("clear-alt").hidden = !alt;
    $("alt-info").textContent = alt ? name || "image" : "Keeps the image above";
    const thumb = $("alt-thumb");
    thumb.width = w;
    thumb.height = h;
    const ctx = thumb.getContext("2d");
    ctx.clearRect(0, 0, w, h);
    if (alt) ctx.drawImage(rawToCanvas(alt, w, h), 0, 0);
  }
}
