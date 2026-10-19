// Storage meter: how full the host's image memory is (`storage` in /api/info, ESP32 only). Polled slowly,
// because uploads change it.

import { api } from "./api.js";

const $ = (id) => document.getElementById(id);
const POLL_MS = 20000;
const WARN = 0.85;

const megabytes = (n) => `${(n / (1024 * 1024)).toFixed(1)} MB`;

export class StorageView {
  constructor() {
    this.timer = null;
  }

  // storage: { total, used } in bytes, or undefined when the host does not report it.
  show(storage) {
    clearInterval(this.timer);
    $("storage").hidden = !storage;
    if (!storage) return;
    this.render(storage);
    this.timer = setInterval(() => this.refresh(), POLL_MS);
  }

  render({ total, used }) {
    const fraction = total ? Math.min(used / total, 1) : 0;
    $("storage-fill").style.width = `${Math.round(fraction * 100)}%`;
    $("storage-fill").classList.toggle("warn", fraction >= WARN);
    $("storage-text").textContent = `${megabytes(used)} of ${megabytes(total)} used`;
  }

  async refresh() {
    try {
      const { storage } = await api.info();
      if (storage) this.render(storage);
    } catch (e) {
      console.warn("storage not updated:", e); // the next poll tries again
    }
  }
}
