// Key editor parts for MQTT: the publish action's fields and the colour rules.

import { toConfigColor, toCssColor } from "./rgb565.js";

const $ = (id) => document.getElementById(id);
const BUILTINS = ["time", "date"];
const NEW_RULE_COLOR = "ffcc00";

export class KeyMqtt {
  // cb: { edit(patch) }
  constructor(cb) {
    this.cb = cb;
    this.key = null;
    this.sig = "";
    for (const id of ["mqtt-topic", "mqtt-payload"]) $(id).addEventListener("input", () => this.onAction());
    for (const id of ["mqtt-retain", "mqtt-qos"]) $(id).addEventListener("change", () => this.onAction());
    $("add-rule").addEventListener("click", () => this.addRule());
  }

  // key: the resolved key; names: variable names a rule can test.
  show(key, names) {
    this.key = key;
    const a = key.action;
    $("mqtt-box").hidden = a?.type !== "mqtt";
    if (a?.type === "mqtt") {
      if (document.activeElement !== $("mqtt-topic")) $("mqtt-topic").value = a.topic || "";
      if (document.activeElement !== $("mqtt-payload")) $("mqtt-payload").value = a.payload || "";
      $("mqtt-retain").checked = Boolean(a.retain);
      $("mqtt-qos").value = String(a.qos || 0);
    }
    this.renderRules(key.rules || [], [...names, ...BUILTINS]);
  }

  onAction() {
    const topic = $("mqtt-topic").value.trim();
    const wildcard = /[+#]/.test(topic);
    $("mqtt-topic").classList.toggle("invalid", wildcard);
    $("mqtt-note").textContent = wildcard ? "Not saved: a topic to publish to has no + or # wildcards." : "";
    if (wildcard) return;
    this.cb.edit({
      action: {
        type: "mqtt",
        topic,
        payload: $("mqtt-payload").value,
        retain: $("mqtt-retain").checked,
        qos: Number($("mqtt-qos").value),
      },
    });
  }

  setRules(rules) {
    this.cb.edit({ rules });
  }

  addRule() {
    const names = this.names;
    if (!names.length) return;
    this.setRules([...(this.key.rules || []), { variable: names[0], value: "", color: NEW_RULE_COLOR }]);
  }

  patchRule(i, patch) {
    const rules = (this.key.rules || []).map((r, n) => (n === i ? { ...r, ...patch } : r));
    this.setRules(rules);
  }

  renderRules(rules, names) {
    this.names = names;
    const sig = JSON.stringify([rules.length, names]);
    const box = $("rules");
    if (sig !== this.sig) {
      this.sig = sig;
      box.replaceChildren(...rules.map((_, i) => this.row(i, names)));
    }
    rules.forEach((r, i) => {
      const row = box.children[i];
      const set = (sel, value) => {
        const el = row.querySelector(sel);
        if (document.activeElement !== el) el.value = value;
      };
      set(".rule-var", r.variable);
      set(".rule-value", r.value);
      set(".rule-color", toCssColor(r.color || this.key.color));
      set(".rule-text", toCssColor(r.label_color || this.key.label_color));
    });
    $("add-rule").disabled = !names.length;
  }

  row(i, names) {
    const row = document.createElement("div");
    row.className = "rule";
    const field = (tag, cls, attrs = {}) => Object.assign(document.createElement(tag), { className: cls, ...attrs });
    const variable = field("select", "text-input rule-var");
    variable.append(...names.map((n) => new Option(`{${n}}`, n)));
    const value = field("input", "text-input rule-value", { placeholder: "value", maxLength: 64, spellcheck: false });
    const color = field("input", "rule-color", { type: "color", title: "Key colour while it matches" });
    const text = field("input", "rule-text", { type: "color", title: "Text colour while it matches" });
    const del = field("button", "btn ghost small", { type: "button", textContent: "×", title: "Remove rule" });
    variable.addEventListener("change", () => this.patchRule(i, { variable: variable.value }));
    value.addEventListener("input", () => this.patchRule(i, { value: value.value }));
    color.addEventListener("input", () => this.patchRule(i, { color: toConfigColor(color.value) }));
    text.addEventListener("input", () => this.patchRule(i, { label_color: toConfigColor(text.value) }));
    del.addEventListener("click", () => this.setRules((this.key.rules || []).filter((_, n) => n !== i)));
    row.append(variable, value, color, text, del);
    return row;
  }
}
