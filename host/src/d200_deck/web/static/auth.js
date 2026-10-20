// Login: shows the form when the host asks for it, and keeps pending edits only for the same user.

import { api } from "./api.js";

const $ = (id) => document.getElementById(id);

// The page behind the form is neither clickable nor reachable by keyboard.
const lock = (on) => document.querySelectorAll(".topbar, .layout").forEach((el) => (el.inert = on));

// DOM side: the login form and the logout button.
export class LoginView {
  constructor() {
    this.submitted = null;
    $("login-form").addEventListener("submit", (e) => {
      e.preventDefault();
      this.submitted?.({ username: $("login-user").value, password: $("login-password").value });
    });
    $("logout").addEventListener("click", () => this.onLogout?.());
  }

  // Show the form; resolves with the entered credentials once.
  credentials(message = "") {
    $("login-error").textContent = message;
    $("login").hidden = false;
    lock(true);
    $("login-password").value = "";
    ($("login-user").value ? $("login-password") : $("login-user")).focus();
    return new Promise((resolve) => (this.submitted = resolve));
  }

  hide() {
    $("login").hidden = true;
    lock(false);
    $("login-password").value = "";
  }

  setUser(name, required) {
    $("logout").hidden = !required;
    $("whoami").textContent = name ?? "";
  }
}

export class AuthFlow {
  // hooks: { stop() closes live streams, view, autosave }
  constructor({ view, autosave, stop }) {
    Object.assign(this, { view, autosave, stop });
    this.username = null;
    this.recovering = null;
  }

  // Resolves once the host is usable: open, or after a successful login.
  async init() {
    const session = await api.authSession();
    if (session.required && !session.authenticated) await this.login();
    else this.setUser(session);
  }

  setUser(session) {
    this.username = session.required ? session.username : null;
    this.view.setUser(this.username, session.required);
  }

  async login() {
    let message = "";
    for (;;) {
      const { username, password } = await this.view.credentials(message);
      try {
        this.setUser(await api.login(username, password));
        this.view.hide();
        return;
      } catch (e) {
        message = e.message;
      }
    }
  }

  // The session ended (expiry, logout, host restart). Resolves with true if the same user is back, so
  // pending edits may still be saved; they are dropped when somebody else logs in.
  recover() {
    this.recovering ??= this.relogin().finally(() => (this.recovering = null));
    return this.recovering;
  }

  async relogin() {
    const previous = this.username;
    this.autosave.pause();
    this.stop();
    await this.login();
    const same = previous === this.username;
    if (same) this.autosave.resume();
    else this.autosave.discard();
    return same;
  }

  // The event stream broke: a login problem or just the network?
  async verify() {
    const session = await api.authSession();
    return session.required && !session.authenticated;
  }

  async logout() {
    await this.autosave.flush().catch((e) => console.warn("pending edits not saved before logout:", e));
    await api.logout();
    this.autosave.discard();
  }
}
