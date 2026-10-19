// Host log: the last log lines of the host (ESP32 firmware), polled from GET /api/log while the tab is visible.

import { api } from "./api.js";

const $ = (id) => document.getElementById(id);
const POLL_MS = 1500;
const MAX_LINES = 1500;

export class HostLog {
  constructor() {
    this.since = 0;
    this.lines = [];
    this.timer = null;
    $("hostlog-pause").addEventListener("click", () => this.togglePause());
    $("hostlog-clear").addEventListener("click", () => this.clear());
    $("hostlog-download").addEventListener("click", () => this.download());
  }

  // Shown only when the host offers a log (`log: true` in /api/info).
  show(available) {
    $("hostlog").hidden = !available;
    if (available && !this.timer) this.poll();
    if (!available) this.stop();
  }

  stop() {
    clearTimeout(this.timer);
    this.timer = null;
  }

  async poll() {
    this.stop();
    let more = false;
    try {
      const { text, next } = await api.log(this.since);
      if (next < this.since) this.add(["--- host restarted ---"]); // the host's counter started over
      this.since = next;
      if (text) this.add(text.split("\n").filter((l) => l !== ""));
      more = text.length > 4096; // a full chunk: ask again at once
    } catch (e) {
      $("hostlog-state").textContent = "no connection";
    }
    this.timer = setTimeout(() => this.poll(), more ? 0 : POLL_MS);
  }

  add(lines) {
    if (!lines.length) return;
    this.lines.push(...lines);
    if (this.lines.length > MAX_LINES) this.lines.splice(0, this.lines.length - MAX_LINES);
    $("hostlog-state").textContent = `${this.lines.length} lines`;
    if ($("hostlog-pause").dataset.paused) return;
    const box = $("hostlog-lines");
    const follow = box.scrollTop + box.clientHeight >= box.scrollHeight - 20;
    box.textContent = this.lines.join("\n");
    if (follow) box.scrollTop = box.scrollHeight;
  }

  togglePause() {
    const button = $("hostlog-pause");
    if (button.dataset.paused) {
      delete button.dataset.paused;
      button.textContent = "Pause";
      $("hostlog-lines").textContent = this.lines.join("\n");
    } else {
      button.dataset.paused = "1";
      button.textContent = "Resume";
    }
  }

  clear() {
    this.lines = [];
    $("hostlog-lines").textContent = "";
    $("hostlog-state").textContent = "0 lines";
  }

  download() {
    const stamp = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
    const url = URL.createObjectURL(new Blob([this.lines.join("\n") + "\n"], { type: "text/plain" }));
    const link = Object.assign(document.createElement("a"), { href: url, download: `${stamp}-d200-host.log` });
    link.click();
    URL.revokeObjectURL(url);
  }
}
