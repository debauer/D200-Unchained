import assert from "node:assert/strict";
import { test } from "node:test";
import { AuthRequired, api } from "../../src/d200_deck/web/static/api.js";
import { Autosave } from "../../src/d200_deck/web/static/autosave.js";
import { AuthFlow } from "../../src/d200_deck/web/static/auth.js";

const reply = (status, body = {}) => async () => new Response(JSON.stringify(body), { status });

function flow(t, logins) {
  const calls = { stop: 0, shown: [], user: [], methods: [] };
  const view = {
    credentials: async (message, methods) => (calls.shown.push(message), calls.methods.push(methods), logins.shift()),
    hide() {},
    setUser: (name, required) => calls.user.push([name, required]),
  };
  const messages = [];
  const autosave = new Autosave({ config: () => ({ pages: [] }), entryOf: () => ({}), setEntry() {}, slot: () => ({}) }, (m) => messages.push(m));
  return { calls, autosave, messages, auth: new AuthFlow({ view, autosave, stop: () => calls.stop++ }) };
}

const session = (username) => ({ required: true, authenticated: true, username });

test("a host without /auth/session is open", async (t) => {
  t.mock.method(globalThis, "fetch", reply(404));
  assert.deepEqual(await api.authSession(), { required: false, authenticated: true, username: null, methods: [] });
});

test("401 raises AuthRequired and notifies once; a failed login does not", async (t) => {
  let lost = 0;
  api.onAuthLost(() => lost++);
  t.mock.method(globalThis, "fetch", reply(401, { error: "invalid username or password" }));
  await assert.rejects(api.info(), AuthRequired);
  assert.equal(lost, 1);
  await assert.rejects(api.login("a", "b"), { message: "invalid username or password" });
  assert.equal(lost, 1);
  api.onAuthLost(() => {});
});

test("init shows the form until the login works", async (t) => {
  const answers = [{ username: "a", password: "bad" }, { username: "a", password: "good" }];
  t.mock.method(api, "authSession", async () => ({ required: true, authenticated: false, username: null, methods: ["password"] }));
  t.mock.method(api, "login", async (_user, password) => {
    if (password !== "good") throw new Error("invalid username or password");
    return session("a");
  });
  const { auth, calls } = flow(t, answers);
  await auth.init();
  assert.deepEqual(calls.shown, ["", "invalid username or password"]);
  assert.equal(auth.username, "a");
});

test("the same user keeps pending edits, another user loses them", async (t) => {
  t.mock.method(api, "authSession", async () => ({ ...session("a"), authenticated: false, methods: ["password"] }));
  t.mock.method(api, "login", async (username) => session(username));
  for (const [again, same] of [["a", true], ["b", false]]) {
    const { auth, autosave, calls } = flow(t, [{ username: again, password: "x" }]);
    auth.username = "a";
    autosave.touchConfig();
    clearTimeout(autosave.timer);
    assert.equal(await auth.recover(), same);
    assert.equal(calls.stop, 1);
    assert.equal(autosave.configDirty, same);
    assert.equal(autosave.paused, false);
    clearTimeout(autosave.timer);
  }
});

test("autosave does not save or retry while the login is missing", async (t) => {
  const put = t.mock.method(api, "putConfig", async () => ({}));
  const { autosave, messages } = flow(t, []);
  autosave.pause();
  autosave.touchConfig();
  await assert.rejects(autosave.flush(), AuthRequired);
  assert.equal(put.mock.callCount(), 0);
  assert.equal(messages.at(-1), "unsaved: login required");
  autosave.discard();
  assert.equal(autosave.configDirty, false);
});

test("a save refused with 401 stays pending", async (t) => {
  t.mock.method(api, "putConfig", async () => {
    throw new AuthRequired("login required");
  });
  const { autosave, messages } = flow(t, []);
  autosave.configDirty = true;
  await assert.rejects(autosave.flush(), AuthRequired);
  assert.equal(autosave.configDirty, true);
  assert.equal(messages.at(-1), "unsaved: login required");
});

test("simultaneous 401s share one login", async (t) => {
  let logins = 0;
  t.mock.method(api, "authSession", async () => ({ ...session("a"), authenticated: false, methods: ["password"] }));
  t.mock.method(api, "login", async (username) => (logins++, session(username)));
  const { auth } = flow(t, [{ username: "a", password: "x" }]);
  const [one, two] = await Promise.all([auth.recover(), auth.recover()]);
  assert.equal(one, two);
  assert.equal(logins, 1);
});

test("a colliding display name cannot retain another identity's pending edits", async (t) => {
  t.mock.method(api, "authSession", async () => ({ ...session("admin"), authenticated: false, methods: ["password", "oidc"] }));
  for (const [identity, same] of [["local-identity", true], ["oidc-subject-identity", false]]) {
    t.mock.method(api, "login", async () => ({ ...session("admin"), identity }));
    const { auth, autosave } = flow(t, [{ username: "admin", password: "x" }]);
    auth.setUser({ ...session("admin"), identity: "local-identity" });
    autosave.touchConfig();
    clearTimeout(autosave.timer);
    assert.equal(await auth.recover(), same);
    assert.equal(autosave.configDirty, same);
    clearTimeout(autosave.timer);
  }
});

function browserGlobals(t, values) {
  for (const [name, value] of Object.entries(values)) {
    const original = Object.getOwnPropertyDescriptor(globalThis, name);
    Object.defineProperty(globalThis, name, { value, configurable: true, writable: true });
    t.after(() => (original ? Object.defineProperty(globalThis, name, original) : delete globalThis[name]));
  }
}

const settle = () => new Promise((r) => setTimeout(r, 10));

test("the form offers what the host offers and shows why the provider login failed", async (t) => {
  t.mock.method(api, "authSession", async () => ({ required: true, authenticated: false, username: null, methods: ["password", "oidc"] }));
  t.mock.method(api, "oidcStart", async () => ({ redirect: "https://id.example/auth?x=1" }));
  const assigned = [];
  browserGlobals(t, { location: { assign: (url) => assigned.push(url), search: "?auth_error=forbidden", pathname: "/" }, history: { replaceState() {} } });
  const { auth, calls } = flow(t, [{ oidc: true }]);
  void auth.init();
  await settle();
  assert.deepEqual(calls.methods[0], ["password", "oidc"]);
  assert.equal(calls.shown[0], "This account may not use the deck (role missing).");
  assert.deepEqual(assigned, ["https://id.example/auth?x=1"]);
});

test("pending edits are only dropped by the provider redirect after confirmation", async (t) => {
  t.mock.method(api, "oidcStart", async () => ({ redirect: "https://id.example/auth" }));
  const assigned = [], answers = [false, true];
  browserGlobals(t, { location: { assign: (url) => assigned.push(url) }, confirm: () => answers.shift() });
  const { auth, autosave } = flow(t, []);
  autosave.configDirty = true;
  await auth.redirectToProvider(); // declined: returns, the form stays
  assert.deepEqual(assigned, []);
  void auth.redirectToProvider();
  await settle();
  assert.deepEqual(assigned, ["https://id.example/auth"]);
});
