// node --test tests/web/*.test.mjs  (run from host/; also started by tests/test_web_js.py)
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  fitRect,
  joinCommand,
  rgb565ToRgba,
  rgbaToRgb565,
  splitCommand,
  toConfigColor,
  toCssColor,
} from "../../src/d200_deck/web/static/rgb565.js";

test("rgba -> rgb565 matches the Python and Rust encoders", () => {
  const rgba = new Uint8ClampedArray([255, 0, 0, 255, 0, 255, 0, 255, 255, 255, 255, 255]);
  assert.deepEqual([...rgbaToRgb565(rgba)], [0x00, 0xf8, 0xe0, 0x07, 0xff, 0xff]);
});

test("rgb565 -> rgba expands like deckd", () => {
  assert.deepEqual([...rgb565ToRgba(new Uint8Array([0x00, 0xf8, 0xff, 0xff]))], [255, 0, 0, 255, 255, 255, 255, 255]);
});

test("fitRect contain, cover, stretch", () => {
  assert.deepEqual(fitRect(100, 50, 196, 196, "contain"), { x: 0, y: 49, w: 196, h: 98 });
  assert.deepEqual(fitRect(100, 50, 196, 196, "cover"), { x: -98, y: 0, w: 392, h: 196 });
  assert.deepEqual(fitRect(100, 50, 196, 196, "stretch"), { x: 0, y: 0, w: 196, h: 196 });
});

test("colour conversion", () => {
  assert.equal(toConfigColor("#FF8800"), "ff8800");
  assert.equal(toCssColor("ff8800"), "#ff8800");
});

test("splitCommand splits like a shell, without shell features", () => {
  assert.deepEqual(splitCommand("open -a OBS"), ["open", "-a", "OBS"]);
  assert.deepEqual(splitCommand(`say "hi there" 'x y'`), ["say", "hi there", "x y"]);
  assert.deepEqual(splitCommand(`echo "a\\"b" 'c\\d'`), ["echo", 'a"b', "c\\d"]);
  assert.deepEqual(splitCommand("  spaced   out  "), ["spaced", "out"]);
  assert.deepEqual(splitCommand(`x ''`), ["x", ""]);
  assert.deepEqual(splitCommand("a|b && c"), ["a|b", "&&", "c"]); // no pipes, no chaining
  assert.throws(() => splitCommand(`say "oops`), /unterminated " quote/);
  assert.throws(() => splitCommand("say 'oops"), /unterminated ' quote/);
});

test("joinCommand round-trips through splitCommand", () => {
  for (const argv of [["open", "-a", "OBS"], ["say", "it's", "a b", ""], ["x", 'q"uote', "back\\slash"]]) {
    assert.deepEqual(splitCommand(joinCommand(argv)), argv);
  }
  assert.equal(joinCommand(["open", "-a", "My App"]), "open -a 'My App'");
});
