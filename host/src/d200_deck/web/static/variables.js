// Sidebar list of placeholder variables with their live values, and the edit form.

import { renameVariable } from "./model.js";
import { nameProblem } from "./placeholders.js";

const $ = (id) => document.getElementById(id);
const DEFAULT_INTERVAL = 10;

export class VariablesView {
  // ctx: { config(), setConfig(cfg), changed() -> config edited (save, redraw), values(), types() -> variable types of the host }
  constructor(ctx) {
    this.ctx = ctx;
    this.selected = null;
    $("add-var").addEventListener("click", () => this.add());
    $("var-name").addEventListener("change", (e) => this.rename(e.target.value.trim()));
    $("var-type").addEventListener("change", (e) => this.setType(e.target.value));
    $("var-cmd").addEventListener("input", (e) => this.setCommand(e.target.value));
    $("var-topic").addEventListener("input", (e) => this.update(this.selected, { topic: e.target.value.trim() }));
    $("var-json").addEventListener("input", (e) => this.update(this.selected, { json_path: e.target.value.trim() }));
    $("var-interval").addEventListener("change", (e) => this.setInterval(e.target.value));
    $("var-delete").addEventListener("click", () => this.remove());
  }

  variables() {
    return this.ctx.config().variables || [];
  }

  update(name, patch) {
    const cfg = this.ctx.config();
    this.ctx.setConfig({ ...cfg, variables: this.variables().map((v) => (v.name === name ? { ...v, ...patch } : v)) });
    this.ctx.changed();
  }

  note(text) {
    $("var-note").textContent = text;
  }

  add() {
    const taken = this.variables().map((v) => v.name);
    let n = taken.length + 1;
    while (taken.includes(`var${n}`)) n += 1;
    const v = { name: `var${n}`, type: "command", command: "", interval: DEFAULT_INTERVAL };
    this.ctx.setConfig({ ...this.ctx.config(), variables: [...this.variables(), v] });
    this.selected = v.name;
    this.render();
    $("var-cmd").focus();
  }

  rename(name) {
    const from = this.selected;
    if (!from || name === from) return;
    const problem = nameProblem(name, this.variables().map((v) => v.name).filter((n) => n !== from));
    if (problem) {
      this.note(`Not renamed: ${problem}`);
      $("var-name").value = from;
      return;
    }
    this.ctx.setConfig(renameVariable(this.ctx.config(), from, name));
    this.selected = name;
    this.note(`Key texts now use {${name}}.`);
    this.ctx.changed();
  }

  setType(type) {
    this.update(this.selected, { type });
    this.render();
  }

  setCommand(line) {
    this.update(this.selected, { command: line });
    this.note("Runs on the host with sh -c; what it prints is the value.");
  }

  setInterval(value) {
    const interval = Math.min(3600, Math.max(1, Math.round(Number(value)) || DEFAULT_INTERVAL));
    $("var-interval").value = interval;
    this.update(this.selected, { interval });
  }

  remove() {
    const name = this.selected;
    const dropRules = (k) => (k.rules ? { ...k, rules: k.rules.filter((r) => r.variable !== name) } : k);
    const cfg = this.ctx.config();
    this.ctx.setConfig({
      ...cfg,
      variables: this.variables().filter((v) => v.name !== name),
      pages: cfg.pages.map((p) => ({ ...p, keys: p.keys.map(dropRules) })), // rules need their variable
    });
    this.selected = null;
    this.ctx.changed();
  }

  render() {
    const values = this.ctx.values();
    $("vars").replaceChildren(
      ...this.variables().map((v) => {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "var-row";
        b.classList.toggle("active", v.name === this.selected);
        const name = document.createElement("span");
        name.className = "var-name";
        name.textContent = `{${v.name}}`;
        const value = document.createElement("span");
        value.className = "var-value";
        value.textContent = values[v.name] ?? "\u2014";
        b.append(name, value);
        b.addEventListener("click", () => {
          this.selected = this.selected === v.name ? null : v.name;
          this.note("");
          this.render();
        });
        return b;
      }),
    );
    const v = this.variables().find((x) => x.name === this.selected);
    $("var-form").hidden = !v;
    if (!v) return;
    if (document.activeElement !== $("var-name")) $("var-name").value = v.name;
    const types = this.ctx.types();
    $("var-type").replaceChildren(...[...new Set([...types, v.type])].map((t) => new Option(t, t)));
    $("var-type").value = v.type;
    const isMqtt = v.type === "mqtt";
    $("var-command-box").hidden = isMqtt;
    $("var-mqtt-box").hidden = !isMqtt;
    if (document.activeElement !== $("var-cmd")) $("var-cmd").value = v.type === "command" ? v.command || "" : "";
    $("var-cmd").disabled = v.type !== "command";
    if (isMqtt) {
      if (document.activeElement !== $("var-topic")) $("var-topic").value = v.topic || "";
      if (document.activeElement !== $("var-json")) $("var-json").value = v.json_path || "";
    } else if (v.type !== "command") this.note(`Type "${v.type}" is not read by this host; kept in the config.`);
    $("var-interval-box").hidden = isMqtt;
    if (document.activeElement !== $("var-interval")) $("var-interval").value = v.interval ?? DEFAULT_INTERVAL;
  }
}
