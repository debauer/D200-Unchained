// node --test tests/web/*.test.mjs  (run from host/; also started by tests/test_web_js.py)
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  addChild,
  canMove,
  movePage,
  crumbs,
  keyOf,
  viewOf,
  linkedPages,
  newPageId,
  treeRows,
  withKey,
  withoutSubtree,
} from "../../src/d200_deck/web/static/model.js";

const CFG = {
  version: 2,
  brightness: 255,
  background: "000000",
  pages: [
    { id: "root", name: "Default", parent: null, keys: [{ index: 1, action: { type: "page", target: "lights" } }] },
    { id: "lights", name: "Lights", parent: "root", keys: [{ index: 2, action: { type: "page", target: "studio" } }] },
    { id: "studio", name: "Studio", parent: "lights", keys: [] },
    { id: "spare", name: "Spare", parent: "root", keys: [] },
  ],
};

test("tree rows are depth-first with FREE marks", () => {
  assert.deepEqual(
    treeRows(CFG).map((r) => [r.id, r.depth, r.orphan]),
    [["root", 0, false], ["lights", 1, false], ["studio", 2, false], ["spare", 1, true]],
  );
  assert.deepEqual([...linkedPages(CFG)].sort(), ["lights", "studio"]);
});

test("crumbs lead from the root to the page", () => {
  assert.deepEqual(crumbs(CFG, "studio").map((p) => p.id), ["root", "lights", "studio"]);
});

test("keys: defaults for unset ones, replacing an entry", () => {
  assert.equal(viewOf(CFG, "studio", 4).color, "000000");
  const cfg = withKey(CFG, "studio", { index: 4, label: "X" });
  assert.equal(keyOf(cfg, "studio", 4).label, "X");
  assert.equal(keyOf(CFG, "studio", 4).label, ""); // input unchanged
});

test("new subpages get a free id and a back key", () => {
  assert.equal(newPageId(CFG, "Lights"), "lights-2");
  assert.equal(newPageId(CFG, "Studio Lights!"), "studio-lights");
  assert.equal(newPageId(CFG, "Ää"), "aa");
  assert.equal(newPageId(CFG, "!!!"), "page");
  const { cfg, id } = addChild(CFG, "spare", "Scenes");
  assert.equal(id, "scenes");
  assert.deepEqual(keyOf(cfg, "scenes", 0).action, { type: "back" });
  assert.equal(cfg.pages.find((p) => p.id === "scenes").parent, "spare");
});

test("deleting a page removes its subtree and unlinks keys", () => {
  const cfg = withoutSubtree(CFG, "lights");
  assert.deepEqual(cfg.pages.map((p) => p.id), ["root", "spare"]);
  assert.deepEqual(keyOf(cfg, "root", 1).action, { type: "page", target: "" });
});

test("keys without their own colour follow the page default", () => {
  const lights = { id: "lights", name: "Lights", parent: "root", color: "203040", label_color: "00f0ff", keys: [{ index: 1 }, { index: 2, color: "ff0000" }] };
  const cfg = { ...CFG, background: "111111", pages: [CFG.pages[0], lights] };
  assert.deepEqual([viewOf(cfg, "lights", 5).color, viewOf(cfg, "lights", 5).label_color], ["203040", "00f0ff"]);
  assert.deepEqual([viewOf(cfg, "lights", 1).color, viewOf(cfg, "lights", 2).color], ["203040", "ff0000"]);
  assert.equal(keyOf(cfg, "lights", 1).color, undefined);
  assert.deepEqual([viewOf(cfg, "root", 5).color, viewOf(cfg, "root", 5).label_color], ["111111", "ffffff"]);
});

test("moving a page keeps its subpages and clears the old parent's links", () => {
  assert.equal(canMove(CFG, "root", "lights"), false); // the root stays
  assert.equal(canMove(CFG, "lights", "studio"), false); // not into its own subtree
  assert.equal(canMove(CFG, "lights", "lights"), false);
  assert.equal(canMove(CFG, "lights", "root"), false); // already there
  assert.equal(canMove(CFG, "studio", "spare"), true);
  const moved = movePage(CFG, "lights", "spare");
  assert.deepEqual(crumbs(moved, "studio").map((p) => p.id), ["root", "spare", "lights", "studio"]);
  assert.equal(moved.pages.find((p) => p.id === "root").keys[0].action.target, ""); // link from the old parent gone
  assert.equal(moved.pages.find((p) => p.id === "lights").keys[0].action.target, "studio"); // inner link kept
  assert.equal(linkedPages(moved).has("lights"), false); // FREE under the new parent
});
