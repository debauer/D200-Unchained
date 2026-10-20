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
    $("login-oidc").addEventListener("click", () => this.submitted?.({ oidc: true }));
  }

  // Show the form; resolves with the entered credentials once.
  credentials(message = "", methods = ["password"]) {
    $("login-error").textContent = message;
    $("login-password-part").hidden = !methods.includes("password");
    $("login-oidc").hidden = !methods.includes("oidc");
    $("login").hidden = false;
    lock(true);
    $("login-password").value = "";
    if (!methods.includes("password")) return new Promise((resolve) => (this.submitted = resolve));
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

const OIDC_ERRORS = {
  denied: "The login was cancelled or denied.",
  expired: "The login expired or was started in another browser. Try again.",
  forbidden: "This account may not use the deck (role missing).",
  token: "The identity provider's answer was refused.",
  provider: "The identity provider cannot be reached.",
  clock: "The panel clock is not synchronized yet. Try again or use local login.",
  capacity: "The panel has no free login sessions. Try again later.",
};

// `?auth_error=<code>` is what the OIDC callback leaves in the URL after a failed login.
function oidcError() {
  const code = new URLSearchParams(globalThis.location?.search).get("auth_error");
  if (code === null) return "";
  history.replaceState(null, "", location.pathname);
  return OIDC_ERRORS[code] ?? "The login failed.";
}

export class AuthFlow {
  // hooks: { stop() closes live streams, view, autosave }
  constructor({ view, autosave, stop }) {
    Object.assign(this, { view, autosave, stop });
    this.username = null;
    this.identity = null;
    this.recovering = null;
  }

  // Resolves once the host is usable: open, or after a successful login.
  async init() {
    const session = await api.authSession();
    const message = oidcError();
    if (session.required && !session.authenticated) await this.login(message);
    else this.setUser(session);
  }

  setUser(session) {
    this.username = session.required ? session.username : null;
    this.identity = session.required ? (session.identity ?? session.username) : null;
    this.view.setUser(this.username, session.required);
  }

  async login(message = "") {
    const { methods } = await api.authSession();
    for (;;) {
      const { username, password, oidc } = await this.view.credentials(message, methods);
      try {
        if (oidc) {
          await this.redirectToProvider();
          continue;
        }
        this.setUser(await api.login(username, password));
        this.view.hide();
        return;
      } catch (e) {
        message = e.message;
      }
    }
  }

  // Leaves the page for the identity provider; the callback lands on a fresh page, so memory edits are lost.
  async redirectToProvider() {
    if (this.autosave.pending && !confirm("Unsaved edits are lost when you sign in through Keycloak. Continue?")) return;
    location.assign((await api.oidcStart()).redirect);
    await new Promise(() => {}); // the page is being replaced
  }

  // The session ended (expiry, logout, host restart). Resolves with true if the same user is back, so
  // pending edits may still be saved; they are dropped when somebody else logs in.
  recover() {
    this.recovering ??= this.relogin().finally(() => (this.recovering = null));
    return this.recovering;
  }

  async relogin() {
    const previous = this.identity ?? this.username;
    this.autosave.pause();
    this.stop();
    await this.login();
    const same = previous === this.identity;
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
