import assert from "node:assert/strict";
import { test } from "node:test";
import { api } from "../../src/d200_deck/web/static/api.js";
import { Autosave } from "../../src/d200_deck/web/static/autosave.js";
import { bindIO } from "../../src/d200_deck/web/static/io.js";

const config = (name) => ({ version: 2, pages: [{ id: "root", name, parent: null, keys: [{ index: 0 }] }] });
const deferred = () => {
  let resolve;
  const promise = new Promise((r) => (resolve = r));
  return { promise, resolve };
};

for (const [property, dirty, upload] of [["icon", "iconDirty", "putIcon"], ["alt", "altDirty", "putAltIcon"]]) {
  test(`a completed ${property} upload preserves a newer edit`, async (t) => {
    const first = new Blob(["A"]), second = new Blob(["B"]);
    const slot = { [property]: first, [dirty]: true };
    const entered = deferred(), finish = deferred(), uploaded = [];
    t.mock.method(api, upload, async (_page, _index, image) => {
      uploaded.push(image);
      if (image === first) {
        entered.resolve();
        await finish.promise;
      }
    });
    const saver = new Autosave({ config: () => config("old"), slot: () => slot, entryOf: () => ({ index: 0 }) }, () => {});
    const pending = saver.saveImages("root", 0);
    await entered.promise;
    slot[property] = second;
    slot[dirty] = true;
    finish.resolve();
    await pending;
    assert.equal(slot[dirty], true);
    await saver.saveImages("root", 0);
    assert.deepEqual(uploaded, [first, second]);
    assert.equal(slot[dirty], false);
  });
}

function documentMock(t) {
  const elements = new Map();
  const document = {
    body: { inert: false },
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, { addEventListener(name, fn) { this[name] = fn; } });
      return elements.get(id);
    },
  };
  const original = Object.getOwnPropertyDescriptor(globalThis, "document");
  Object.defineProperty(globalThis, "document", { value: document, configurable: true });
  t.after(() => original ? Object.defineProperty(globalThis, "document", original) : delete globalThis.document);
  return document;
}

test("import drains old autosaves before replacement and locks editing until reload", async (t) => {
  const document = documentMock(t);
  let local = config("old"), stored = config("old"), loaded;
  const imported = config("imported"), writes = [];
  t.mock.method(api, "putConfig", async (next) => {
    assert.equal(document.body.inert, true);
    writes.push(next.pages[0].name);
    stored = structuredClone(next);
  });
  const saver = new Autosave({ config: () => local }, () => {});
  saver.touchConfig();
  const messages = [];
  bindIO({
    flush: () => saver.flush(),
    reload: async () => { await saver.flush(); loaded = stored; local = stored; },
    toast: (message) => messages.push(message),
  });
  await document.getElementById("import-file").change({ target: {
    files: [{ text: async () => JSON.stringify(imported) }], value: "file",
  } });
  assert.deepEqual(writes, ["old", "imported"]);
  assert.equal(loaded.pages[0].name, "imported");
  assert.match(messages[0], /Config imported/);
  assert.equal(document.body.inert, false);
});

test("failed pending saves abort import and restore editing", async (t) => {
  const document = documentMock(t);
  const put = t.mock.method(api, "putConfig", async () => { throw Error("must not be called"); });
  const messages = [];
  bindIO({ flush: async () => { throw Error("storage full"); }, toast: (message) => messages.push(message) });
  await document.getElementById("import-file").change({ target: {
    files: [{ text: async () => JSON.stringify(config("new")) }], value: "file",
  } });
  assert.equal(put.mock.callCount(), 0);
  assert.match(messages[0], /storage full/);
  assert.equal(document.body.inert, false);
});
