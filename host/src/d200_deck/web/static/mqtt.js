// Sidebar form for the MQTT broker. The password is write-only: the host never sends it back.

import { api } from "./api.js";

const $ = (id) => document.getElementById(id);

export class MqttView {
  // ctx: { toast(message, isError) }
  constructor(ctx) {
    this.ctx = ctx;
    this.state = null;
    $("mqtt-save").addEventListener("click", () => this.save({}));
    $("mqtt-clear-pass").addEventListener("click", () => this.save({ password: null }));
  }

  async load() {
    this.apply(await api.mqtt());
  }

  // New state from the host (GET, PUT or the "mqtt" event).
  apply(state) {
    this.state = state;
    const focused = (id) => document.activeElement === $(id);
    if (!focused("mqtt-host")) $("mqtt-host").value = state.host ?? "";
    if (!focused("mqtt-port")) $("mqtt-port").value = state.port ?? 1883;
    if (!focused("mqtt-user")) $("mqtt-user").value = state.username ?? "";
    $("mqtt-tls").checked = Boolean(state.tls);
    $("mqtt-pass").placeholder = state.password_set ? "(unchanged)" : "none";
    $("mqtt-clear-pass").hidden = !state.password_set;
    const led = $("mqtt-led");
    let text = "off";
    let color = "var(--muted)";
    if (state.enabled && state.connected) [text, color] = ["connected", "var(--green)"];
    else if (state.enabled && state.error) [text, color] = [state.error, "var(--red)"];
    else if (state.enabled) [text, color] = ["connecting…", "var(--amber)"];
    $("mqtt-status").textContent = text;
    led.style.background = color;
    led.style.boxShadow = `0 0 8px ${color}`;
  }

  async save(extra) {
    const settings = {
      host: $("mqtt-host").value.trim(),
      port: Number($("mqtt-port").value) || 1883,
      username: $("mqtt-user").value.trim(),
      tls: $("mqtt-tls").checked,
      ...extra,
    };
    if ($("mqtt-pass").value && !("password" in extra)) settings.password = $("mqtt-pass").value;
    try {
      this.apply(await api.putMqtt(settings));
      $("mqtt-pass").value = "";
      this.ctx.toast(settings.host ? "MQTT settings saved" : "MQTT is off");
    } catch (e) {
      this.ctx.toast(`MQTT not saved: ${e.message}`, true);
    }
  }
}
