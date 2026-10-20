// Client for web API v1 (docs/web-api.md).

const MUTATE = { "X-Deck-Request": "1" };
const keyPath = (page, i) => `/api/pages/${encodeURIComponent(page)}/keys/${i}`;

// The host wants a login (401): thrown for the caller and reported once to the handler of onAuthLost().
export class AuthRequired extends Error {}
let authLost = () => {};

async function check(res, loginRequest = false) {
  if (res.ok) return res;
  if (res.status === 401 && !loginRequest) {
    authLost();
    throw new AuthRequired("login required");
  }
  let message = `${res.status} ${res.statusText}`;
  try {
    message = (await res.json()).error || message;
  } catch {
    // Not a JSON error body; keep the status line.
  }
  throw new Error(message);
}

async function json(path, options = {}) {
  return (await check(await fetch(path, options))).json();
}

async function raw(path) {
  const res = await fetch(path);
  if (res.status === 404) return null;
  return new Uint8Array(await (await check(res)).arrayBuffer());
}

function send(method, path, body, type) {
  const headers = { ...MUTATE };
  if (type) headers["Content-Type"] = type;
  return json(path, { method, headers, body });
}

export const api = {
  onAuthLost: (handler) => (authLost = handler),
  // Authentication capabilities; hosts without /auth/session are open.
  async authSession() {
    const res = await fetch("/auth/session");
    if (res.status === 404) return { required: false, authenticated: true, username: null, methods: [] };
    return (await check(res, true)).json();
  },
  login: async (username, password) =>
    (await check(await fetch("/auth/login", { method: "POST", headers: { ...MUTATE, "Content-Type": "application/json" }, body: JSON.stringify({ username, password }) }), true)).json(),
  logout: () => send("POST", "/auth/logout"),
  oidcStart: () => send("POST", "/auth/oidc/start"), // { redirect } to the identity provider
  info: () => json("/api/info"),
  config: () => json("/api/config"),
  putConfig: (cfg) => send("PUT", "/api/config", JSON.stringify(cfg), "application/json"),
  // Keys are addressed per page (the root page is "root").
  image: (page, i) => raw(`${keyPath(page, i)}/image`),
  putImage: (page, i, bytes) => send("PUT", `${keyPath(page, i)}/image`, bytes, "application/octet-stream"),
  deleteImage: (page, i) => send("DELETE", `${keyPath(page, i)}/image`),
  icon: (page, i) => raw(`${keyPath(page, i)}/icon`),
  putIcon: (page, i, bytes) => send("PUT", `${keyPath(page, i)}/icon`, bytes, "application/octet-stream"),
  deleteIcon: (page, i) => send("DELETE", `${keyPath(page, i)}/icon`),
  altIcon: (page, i) => raw(`${keyPath(page, i)}/alticon`),
  putAltIcon: (page, i, bytes) => send("PUT", `${keyPath(page, i)}/alticon`, bytes, "application/octet-stream"),
  deleteAltIcon: (page, i) => send("DELETE", `${keyPath(page, i)}/alticon`),
  // Host log (optional, `log: true` in /api/info): the text from line `since` on and the number to ask for next.
  async log(since) {
    const res = await check(await fetch(`/api/log?since=${since}`));
    return { text: await res.text(), next: Number(res.headers.get("X-Log-Next")) };
  },
  press: (page, i) => send("POST", `${keyPath(page, i)}/press`),
  mqtt: () => json("/api/mqtt"),
  putMqtt: (settings) => send("PUT", "/api/mqtt", JSON.stringify(settings), "application/json"),
  showPage: (page) => send("POST", "/api/page", JSON.stringify({ page }), "application/json"),
  // handlers: { key, status, action, brightness, page, values, mqtt } -> called with the event data;
  // { offline(), online() } when the stream to the host breaks / (re)opens.
  events(handlers) {
    const source = new EventSource("/api/events");
    for (const kind of ["key", "status", "action", "brightness", "page", "values", "mqtt"]) {
      source.addEventListener(kind, (e) => handlers[kind]?.(JSON.parse(e.data)));
    }
    source.onerror = () => handlers.offline?.();
    source.onopen = () => handlers.online?.();
    return source;
  },
};
