// Page navigation: tree in the sidebar, path, page header (name, up, delete).

import { ROOT, canMove, crumbs, defaultsOf, treeRows } from "./model.js";
import { toConfigColor, toCssColor } from "./rgb565.js";

const $ = (id) => document.getElementById(id);

export class PagesView {
  // callbacks: { open(id), rename(name), up(), remove(), addChild(), move(id, parentId), colors({ color?, label_color?, accent_color? }) }
  constructor(callbacks) {
    this.cb = callbacks;
    $("add-page").addEventListener("click", () => this.cb.addChild());
    $("up").addEventListener("click", () => this.cb.up());
    $("delete-page").addEventListener("click", () => this.cb.remove());
    $("page-name").addEventListener("input", (e) => this.cb.rename(e.target.value));
    $("page-color").addEventListener("input", (e) => this.cb.colors({ color: toConfigColor(e.target.value) }));
    $("page-text-color").addEventListener("input", (e) => this.cb.colors({ label_color: toConfigColor(e.target.value) }));
    $("page-accent-color").addEventListener("input", (e) => this.cb.colors({ accent_color: toConfigColor(e.target.value) }));
    $("page-colors-reset").addEventListener("click", () => this.cb.colors({ color: null, label_color: null, accent_color: null }));
  }

  // Drag a page row onto another row to move it (with its subpages) below that page.
  bindDrag(button, cfg, id) {
    if (id !== ROOT) {
      button.draggable = true;
      button.addEventListener("dragstart", (e) => {
        this.dragged = id;
        e.dataTransfer.setData("text/plain", id);
        e.dataTransfer.effectAllowed = "move";
        button.classList.add("dragging");
      });
      button.addEventListener("dragend", () => {
        this.dragged = null;
        button.classList.remove("dragging");
      });
    }
    button.addEventListener("dragover", (e) => {
      if (!this.dragged || !canMove(cfg, this.dragged, id)) return;
      e.preventDefault();
      button.classList.add("drop-ok");
    });
    button.addEventListener("dragleave", () => button.classList.remove("drop-ok"));
    button.addEventListener("drop", (e) => {
      e.preventDefault();
      button.classList.remove("drop-ok");
      if (this.dragged && canMove(cfg, this.dragged, id)) this.cb.move(this.dragged, id);
    });
  }

  render(cfg, current) {
    $("tree").replaceChildren(
      ...treeRows(cfg).map((row) => {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "tree-row";
        b.classList.toggle("active", row.id === current);
        b.style.paddingLeft = `${10 + row.depth * 16}px`;
        const prefix = document.createElement("span");
        prefix.className = "prefix";
        prefix.textContent = row.depth === 0 ? "\u25a0" : "\u2514";
        const name = document.createElement("span");
        name.className = "name";
        name.textContent = row.name || row.id;
        b.append(prefix, name);
        if (row.orphan) {
          const free = document.createElement("span");
          free.className = "free";
          free.title = "No key links to this page";
          free.textContent = "FREE";
          b.append(free);
        }
        b.addEventListener("click", () => this.cb.open(row.id));
        this.bindDrag(b, cfg, row.id);
        return b;
      }),
    );
    const path = crumbs(cfg, current);
    const prompt = document.createElement("span");
    prompt.className = "path";
    prompt.textContent = "PATH>";
    const parts = [prompt];
    path.forEach((p, i) => {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "crumb";
      b.textContent = p.name || p.id;
      b.addEventListener("click", () => this.cb.open(p.id));
      parts.push(b);
      if (i < path.length - 1) {
        const sep = document.createElement("span");
        sep.className = "sep";
        sep.textContent = "/";
        parts.push(sep);
      }
    });
    $("crumbs").replaceChildren(...parts);
    const page = path[path.length - 1];
    if (document.activeElement !== $("page-name")) $("page-name").value = page.name;
    const defaults = defaultsOf(cfg, current);
    $("page-color").value = toCssColor(defaults.color);
    $("page-text-color").value = toCssColor(defaults.label_color);
    $("page-accent-color").value = toCssColor(defaults.accent_color);
    $("page-colors-reset").hidden = !page.color && !page.label_color && !page.accent_color;
    $("up").hidden = current === ROOT;
    $("delete-page").hidden = current === ROOT;
    $("page-badge").textContent = page.name || page.id;
  }
}
